// barcode plugin —— zxing-cpp 的 standalone WASM 包装。
//
// 不用 emscripten embind（embind 需要 JS host，wasm3 没 JS env）。改导出
// 三个 C-ABI 函数 alloc/free/dispatch，跟 aglet 的 wasm_plugin ABI 对齐：
//   dispatch(ap, al, pp, pl) -> u64  // hi=result_ptr, lo=result_len
//
// 协议（JSON in/out）：
//   action="encode"
//     params:  {"text":"...", "format":"QRCode", "ecc":-1, "margin":4, "width":256, "height":256}
//     result:  {"ok":true,"data":{"dataUrl":"data:image/png;base64,..."}}
//   action="decode"
//     params:  {"width":W, "height":H, "pixels_b64":"..."}   // RGBA8888 pixels
//     result:  {"ok":true,"data":{"text":"...","format":"QRCode"}}
//   error:     {"ok":false,"error":{"code":"...","message":"..."}}
//
// 不带完整 JSON parser/builder：用嵌入式 string scan + handcraft 输出。
// 为了体积，base64 也手写（~50 行）。

#include "BarcodeFormat.h"
#include "MultiFormatWriter.h"
#include "BitMatrix.h"
#include "CharacterSet.h"
#include "ReadBarcode.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <exception>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

using namespace ZXing;

// ─── C ABI exports ────────────────────────────────────────────────────────────

extern "C" {

__attribute__((export_name("alloc")))
uint32_t plugin_alloc(uint32_t n) {
    return (uint32_t)(uintptr_t)std::malloc(n ? n : 1);
}

__attribute__((export_name("free")))
void plugin_free(uint32_t p, uint32_t n) {
    (void)n;
    std::free((void*)(uintptr_t)p);
}

uint64_t plugin_dispatch(uint32_t ap, uint32_t al, uint32_t pp, uint32_t pl);

__attribute__((export_name("dispatch")))
uint64_t plugin_dispatch_export(uint32_t ap, uint32_t al, uint32_t pp, uint32_t pl) {
    return plugin_dispatch(ap, al, pp, pl);
}

} // extern "C"

// ─── tiny JSON helpers ────────────────────────────────────────────────────────
//
// 我们的 params 一直浅平 schema（只有 string / int / nested 不需），手写
// scan-by-key 就够。失败返默认。

static bool jsonFindKey(const std::string& s, const std::string& key, size_t& vstart) {
    std::string k = "\"" + key + "\"";
    size_t i = s.find(k);
    if (i == std::string::npos) return false;
    i += k.size();
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == ':')) i++;
    vstart = i;
    return true;
}

static std::string jsonGetString(const std::string& s, const std::string& key, const std::string& def = "") {
    size_t i;
    if (!jsonFindKey(s, key, i) || i >= s.size() || s[i] != '"') return def;
    i++;
    std::string out;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[i + 1];
            switch (c) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                default: out.push_back(c); break;
            }
            i += 2;
        } else {
            out.push_back(s[i++]);
        }
    }
    return out;
}

static int jsonGetInt(const std::string& s, const std::string& key, int def) {
    size_t i;
    if (!jsonFindKey(s, key, i)) return def;
    bool neg = false;
    if (i < s.size() && s[i] == '-') { neg = true; i++; }
    int v = 0;
    bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i++] - '0');
        any = true;
    }
    if (!any) return def;
    return neg ? -v : v;
}

static std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    o += buf;
                } else {
                    o.push_back(c);
                }
        }
    }
    return o;
}

// ─── base64 ───────────────────────────────────────────────────────────────────

static const char b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string b64Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(b64_chars[(n >> 18) & 63]);
        out.push_back(b64_chars[(n >> 12) & 63]);
        out.push_back(b64_chars[(n >> 6) & 63]);
        out.push_back(b64_chars[n & 63]);
        i += 3;
    }
    if (i < len) {
        uint32_t n = data[i] << 16;
        if (i + 1 < len) n |= data[i + 1] << 8;
        out.push_back(b64_chars[(n >> 18) & 63]);
        out.push_back(b64_chars[(n >> 12) & 63]);
        out.push_back(i + 1 < len ? b64_chars[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

static std::vector<uint8_t> b64Decode(const std::string& s) {
    int rev[256];
    for (int i = 0; i < 256; i++) rev[i] = -1;
    for (int i = 0; i < 64; i++) rev[(int)b64_chars[i]] = i;
    std::vector<uint8_t> out;
    out.reserve(s.size() * 3 / 4);
    int bits = 0, nbits = 0;
    for (char c : s) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = rev[(unsigned char)c];
        if (v < 0) continue;
        bits = (bits << 6) | v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            out.push_back((bits >> nbits) & 0xFF);
        }
    }
    return out;
}

// ─── encode / decode ──────────────────────────────────────────────────────────

static std::string makeError(const char* code, const std::string& msg) {
    return std::string("{\"ok\":false,\"error\":{\"code\":\"") + code +
           "\",\"message\":\"" + jsonEscape(msg) + "\"}}";
}

static std::string doEncode(const std::string& params) {
    try {
        std::string text = jsonGetString(params, "text");
        std::string fmt = jsonGetString(params, "format", "QRCode");
        int ecc = jsonGetInt(params, "ecc", -1);
        int margin = jsonGetInt(params, "margin", 4);
        int w = jsonGetInt(params, "width", 0);
        int h = jsonGetInt(params, "height", 0);

        if (text.empty()) return makeError("INVALID_PARAMS", "text empty");

        auto bf = BarcodeFormatFromString(fmt);
        if (bf == BarcodeFormat::None) return makeError("UNSUPPORTED_FORMAT", fmt);

        MultiFormatWriter writer(bf);
        if (margin >= 0) writer.setMargin(margin);
        if (ecc >= 0 && ecc <= 8) writer.setEccLevel(ecc);

        auto bm = ToMatrix<uint8_t>(writer.encode(text, w, h));

        int png_len = 0;
        uint8_t* png = stbi_write_png_to_mem(bm.data(), 0, bm.width(), bm.height(), 1, &png_len);
        if (!png || png_len <= 0) return makeError("ENCODE", "stbi failed");

        std::string b64 = b64Encode(png, (size_t)png_len);
        STBIW_FREE(png);

        std::string out = "{\"ok\":true,\"data\":{\"dataUrl\":\"data:image/png;base64,";
        out += b64;
        out += "\"}}";
        return out;
    } catch (const std::exception& e) {
        return makeError("ENCODE", e.what());
    } catch (...) {
        return makeError("ENCODE", "unknown");
    }
}

static std::string doDecode(const std::string& params) {
    try {
        int w = jsonGetInt(params, "width", 0);
        int h = jsonGetInt(params, "height", 0);
        std::string pb64 = jsonGetString(params, "pixels_b64");
        if (w <= 0 || h <= 0 || pb64.empty())
            return makeError("INVALID_PARAMS", "need width/height/pixels_b64");

        auto px = b64Decode(pb64);
        if ((int)px.size() < w * h * 4)
            return makeError("INVALID_PARAMS", "pixels too short");

        ImageView img(px.data(), w, h, ImageFormat::RGBA);
        auto results = ReadBarcodes(img);
        if (results.empty())
            return makeError("NOT_FOUND", "no barcode detected");

        const auto& r = results.front();
        std::string text = r.text();
        std::string fmt = ToString(r.format());
        std::string out = "{\"ok\":true,\"data\":{\"text\":\"";
        out += jsonEscape(text);
        out += "\",\"format\":\"";
        out += jsonEscape(fmt);
        out += "\"}}";
        return out;
    } catch (const std::exception& e) {
        return makeError("DECODE", e.what());
    } catch (...) {
        return makeError("DECODE", "unknown");
    }
}

// ─── dispatch ────────────────────────────────────────────────────────────────

uint64_t plugin_dispatch(uint32_t ap, uint32_t al, uint32_t pp, uint32_t pl) {
    std::string action((const char*)(uintptr_t)ap, al);
    std::string params((const char*)(uintptr_t)pp, pl);

    std::string out;
    if (action == "encode") {
        out = doEncode(params);
    } else if (action == "decode") {
        out = doDecode(params);
    } else {
        out = makeError("UNKNOWN_ACTION", action);
    }

    // 把结果拷到 caller-owned linear-mem 区（malloc 出来一块），返 packed。
    // host (zig 端) 复制走后 free。
    size_t n = out.size();
    char* buf = (char*)std::malloc(n ? n : 1);
    if (n) std::memcpy(buf, out.data(), n);
    uint32_t ptr = (uint32_t)(uintptr_t)buf;
    return ((uint64_t)ptr << 32) | (uint64_t)n;
}
