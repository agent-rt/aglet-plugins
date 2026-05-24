// archive plugin —— libarchive standalone wasm wrapper.
//
// 协议（JSON in/out）：
//   list({input_b64}) → {ok: true, entries: [{path, size, is_dir, mtime?}]}
//   extract({input_b64, path}) → {ok: true, bytes_b64}
//   create({format, entries: [{path, bytes_b64}]}) → {ok: true, output_b64}
//     format ∈ {"zip", "tar", "tar.gz"}
//
// Read formats: zip / tar / tar.{gz,bz2} / rar (4 + 5) / ar / cpio / iso /
//   mtree / 7z (without lzma; will fail). libarchive's archive_read_support_*_all
//   enables every format / filter compiled in.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "archive.h"
#include "archive_entry.h"

// ─── C ABI ────────────────────────────────────────────────────────────────────

extern "C" {

__attribute__((export_name("alloc")))
uint32_t plugin_alloc(uint32_t n) {
    return (uint32_t)(uintptr_t)std::malloc(n ? n : 1);
}

__attribute__((export_name("free")))
void plugin_free(uint32_t p, uint32_t n) {
    (void)n; std::free((void*)(uintptr_t)p);
}

uint64_t plugin_dispatch(uint32_t ap, uint32_t al, uint32_t pp, uint32_t pl);

__attribute__((export_name("dispatch")))
uint64_t plugin_dispatch_export(uint32_t ap, uint32_t al, uint32_t pp, uint32_t pl) {
    return plugin_dispatch(ap, al, pp, pl);
}

}

// ─── tiny JSON helpers (string-scan; not robust to escapes inside keys) ────

static bool jsonFindKey(const std::string& s, const std::string& key, size_t& vs) {
    std::string k = "\"" + key + "\"";
    size_t i = s.find(k);
    if (i == std::string::npos) return false;
    i += k.size();
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == ':')) i++;
    vs = i; return true;
}

static std::string jsonGetString(const std::string& s, const std::string& key, const std::string& d = "") {
    size_t i; if (!jsonFindKey(s, key, i) || i >= s.size() || s[i] != '"') return d;
    i++; std::string out;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[i+1];
            if (c == 'n') out += '\n';
            else if (c == 't') out += '\t';
            else if (c == 'r') out += '\r';
            else out += c;
            i += 2;
        } else { out += s[i++]; }
    }
    return out;
}

static bool jsonFindArray(const std::string& s, const std::string& key, size_t& aStart, size_t& aEnd) {
    size_t i; if (!jsonFindKey(s, key, i)) return false;
    if (i >= s.size() || s[i] != '[') return false;
    int depth = 1; size_t j = i + 1;
    while (j < s.size() && depth > 0) {
        if (s[j] == '[') depth++;
        else if (s[j] == ']') depth--;
        else if (s[j] == '"') { j++; while (j < s.size() && s[j] != '"') { if (s[j]=='\\') j++; j++; } }
        j++;
    }
    if (depth != 0) return false;
    aStart = i + 1; aEnd = j - 1;
    return true;
}

static std::vector<std::string> jsonSplitObjArray(const std::string& s, size_t a, size_t b) {
    std::vector<std::string> out;
    size_t i = a;
    while (i < b) {
        while (i < b && (s[i] == ' ' || s[i] == ',' || s[i] == '\n' || s[i] == '\t')) i++;
        if (i >= b || s[i] != '{') break;
        int depth = 1; size_t st = i; i++;
        while (i < b && depth > 0) {
            if (s[i] == '{') depth++;
            else if (s[i] == '}') depth--;
            else if (s[i] == '"') { i++; while (i < b && s[i] != '"') { if (s[i]=='\\') i++; i++; } }
            i++;
        }
        out.push_back(s.substr(st, i - st));
    }
    return out;
}

// JSON-escape a string for output (handle quotes, backslash, control chars).
static std::string jsonEscape(const std::string& s) {
    std::string o; o.reserve(s.size() + 2);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) {
            char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            o += buf;
        } else { o += (char)c; }
    }
    return o;
}

// ─── base64 ─────────────────────────────────────────────────────────────────

static const char b64c[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string b64Encode(const uint8_t* d, size_t n) {
    std::string o; o.reserve(((n + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= n) {
        uint32_t v = (d[i]<<16)|(d[i+1]<<8)|d[i+2];
        o += b64c[(v>>18)&63]; o += b64c[(v>>12)&63];
        o += b64c[(v>>6)&63]; o += b64c[v&63]; i += 3;
    }
    if (i < n) {
        uint32_t v = d[i] << 16; if (i+1 < n) v |= d[i+1] << 8;
        o += b64c[(v>>18)&63]; o += b64c[(v>>12)&63];
        o += (i+1 < n ? b64c[(v>>6)&63] : '='); o += '=';
    }
    return o;
}

static std::vector<uint8_t> b64Decode(const std::string& s) {
    int rev[256]; for (int i = 0; i < 256; i++) rev[i] = -1;
    for (int i = 0; i < 64; i++) rev[(int)b64c[i]] = i;
    std::vector<uint8_t> out; out.reserve(s.size() * 3 / 4);
    int bits = 0, nbits = 0;
    for (char c : s) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = rev[(unsigned char)c]; if (v < 0) continue;
        bits = (bits << 6) | v; nbits += 6;
        if (nbits >= 8) { nbits -= 8; out.push_back((bits >> nbits) & 0xFF); }
    }
    return out;
}

// ─── result helpers ────────────────────────────────────────────────────────

static std::string err(const char* code, const std::string& msg) {
    return std::string("{\"ok\":false,\"error\":{\"code\":\"") + code +
           "\",\"message\":\"" + jsonEscape(msg) + "\"}}";
}

// ─── actions ───────────────────────────────────────────────────────────────

static std::string doList(const std::string& params) {
    std::string input_b64 = jsonGetString(params, "input_b64");
    if (input_b64.empty()) return err("INVALID_PARAMS", "input_b64 required");
    auto bytes = b64Decode(input_b64);
    if (bytes.empty()) return err("INVALID_PARAMS", "bad base64");

    struct archive* a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    int r = archive_read_open_memory(a, bytes.data(), bytes.size());
    if (r != ARCHIVE_OK) {
        std::string msg = archive_error_string(a) ? archive_error_string(a) : "open failed";
        archive_read_free(a);
        return err("OPEN_FAILED", msg);
    }

    std::string out = "{\"ok\":true,\"entries\":[";
    bool first = true;
    struct archive_entry* entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        if (!first) out += ",";
        first = false;
        const char* path = archive_entry_pathname(entry);
        if (!path) path = "";
        int64_t size = archive_entry_size(entry);
        bool is_dir = archive_entry_filetype(entry) == AE_IFDIR;
        int64_t mtime = archive_entry_mtime(entry);
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            ",\"size\":%lld,\"is_dir\":%s,\"mtime\":%lld",
            (long long)size, is_dir ? "true" : "false", (long long)mtime);
        out += "{\"path\":\"" + jsonEscape(path) + "\"" + buf + "}";
        archive_read_data_skip(a);
    }
    out += "]}";

    archive_read_free(a);
    return out;
}

static std::string doExtract(const std::string& params) {
    std::string input_b64 = jsonGetString(params, "input_b64");
    std::string target_path = jsonGetString(params, "path");
    if (input_b64.empty()) return err("INVALID_PARAMS", "input_b64 required");
    if (target_path.empty()) return err("INVALID_PARAMS", "path required");
    auto bytes = b64Decode(input_b64);
    if (bytes.empty()) return err("INVALID_PARAMS", "bad base64");

    struct archive* a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    int r = archive_read_open_memory(a, bytes.data(), bytes.size());
    if (r != ARCHIVE_OK) {
        std::string msg = archive_error_string(a) ? archive_error_string(a) : "open failed";
        archive_read_free(a);
        return err("OPEN_FAILED", msg);
    }

    struct archive_entry* entry;
    std::vector<uint8_t> data;
    bool found = false;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char* path = archive_entry_pathname(entry);
        if (path && target_path == path) {
            found = true;
            int64_t size = archive_entry_size(entry);
            if (size > 0) data.reserve((size_t)size);
            const void* buf;
            size_t len;
            la_int64_t offset;
            while (archive_read_data_block(a, &buf, &len, &offset) == ARCHIVE_OK) {
                if (offset > (la_int64_t)data.size()) data.resize((size_t)offset);
                data.insert(data.end(), (const uint8_t*)buf, (const uint8_t*)buf + len);
            }
            break;
        }
        archive_read_data_skip(a);
    }
    archive_read_free(a);

    if (!found) return err("NOT_FOUND", "path not in archive: " + target_path);
    return "{\"ok\":true,\"bytes_b64\":\"" + b64Encode(data.data(), data.size()) + "\"}";
}

// libarchive expects open/write/close callbacks for streaming output.
struct WriteCtx { std::vector<uint8_t> out; };
static la_ssize_t writeCb(struct archive*, void* ud, const void* buf, size_t n) {
    auto* w = (WriteCtx*)ud;
    w->out.insert(w->out.end(), (const uint8_t*)buf, (const uint8_t*)buf + n);
    return (la_ssize_t)n;
}

static std::string doCreate(const std::string& params) {
    std::string format = jsonGetString(params, "format", "tar.gz");
    size_t aStart, aEnd;
    if (!jsonFindArray(params, "entries", aStart, aEnd)) {
        return err("INVALID_PARAMS", "entries array required");
    }
    auto items = jsonSplitObjArray(params, aStart, aEnd);

    struct archive* a = archive_write_new();
    if (format == "zip") {
        archive_write_set_format_zip(a);
    } else if (format == "tar") {
        archive_write_set_format_pax_restricted(a);
    } else if (format == "tar.gz") {
        archive_write_set_format_pax_restricted(a);
        archive_write_add_filter_gzip(a);
    } else {
        archive_write_free(a);
        return err("INVALID_PARAMS", "format must be zip / tar / tar.gz");
    }

    WriteCtx ctx;
    archive_write_open(a, &ctx, nullptr, writeCb, nullptr);

    for (auto& item : items) {
        std::string path = jsonGetString(item, "path");
        std::string b64 = jsonGetString(item, "bytes_b64");
        if (path.empty()) continue;
        auto data = b64Decode(b64);

        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, path.c_str());
        archive_entry_set_size(entry, (la_int64_t)data.size());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_write_header(a, entry);
        if (!data.empty()) {
            archive_write_data(a, data.data(), data.size());
        }
        archive_entry_free(entry);
    }

    archive_write_close(a);
    archive_write_free(a);

    return "{\"ok\":true,\"output_b64\":\"" + b64Encode(ctx.out.data(), ctx.out.size()) + "\"}";
}

// ─── dispatch ────────────────────────────────────────────────────────────────

uint64_t plugin_dispatch(uint32_t ap, uint32_t al, uint32_t pp, uint32_t pl) {
    std::string action((const char*)(uintptr_t)ap, al);
    std::string params((const char*)(uintptr_t)pp, pl);

    std::string out;
    if (action == "list")         out = doList(params);
    else if (action == "extract") out = doExtract(params);
    else if (action == "create")  out = doCreate(params);
    else out = err("UNKNOWN_ACTION", std::string("archive.") + action);

    size_t n = out.size();
    char* buf = (char*)std::malloc(n ? n : 1);
    if (n) std::memcpy(buf, out.data(), n);
    return ((uint64_t)(uint32_t)(uintptr_t)buf << 32) | (uint64_t)n;
}
