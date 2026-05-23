// image plugin v2 —— primitives + pipeline (decode / encode / process / metadata)。
//
// 平台原则：暴露原语让作者组合。process 是 decode → ops[] → encode 的单
// wasm-call 管道，省 base64 marshal；decode/encode/metadata 单独可用。
//
// 协议（JSON in/out）：
//   metadata({input_b64}) → {w, h, channels, format}
//   decode({input_b64})   → {pixels_b64, width, height, channels, src_format}
//   encode({pixels_b64, width, height, format, quality?, lossless?})
//                          → {output_b64, format}
//   process({input_b64, ops?:[...], output_format?, quality?, lossless?})
//                          → {output_b64, width, height, format}
//     ops kinds:
//       {kind:"resize", w, h}                 // 双线性等效（stb_image_resize2 默认）
//       {kind:"crop",   x, y, w, h}
//       {kind:"rotate", degrees}              // 仅 90/180/270；其它返 INVALID_OP
//       {kind:"flip",   axis:"x"|"y"}
//
// 解码统一 RGBA8888；transform 操作 RGBA buffer；encode 时按目标格式
// 处理 alpha（JPEG drop alpha）。stb_image_resize2 / 手写 rotate/flip。

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#include "webp/encode.h"
#include "webp/decode.h"

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

// ─── tiny JSON + base64 ─────────────────────────────────────────────────────

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
        if (s[i] == '\\' && i+1 < s.size()) { out.push_back(s[i+1]); i += 2; }
        else out.push_back(s[i++]);
    }
    return out;
}

static int jsonGetInt(const std::string& s, const std::string& key, int d) {
    size_t i; if (!jsonFindKey(s, key, i)) return d;
    bool neg = false;
    if (i < s.size() && s[i] == '-') { neg = true; i++; }
    int v = 0; bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { v = v*10 + (s[i++]-'0'); any = true; }
    return any ? (neg ? -v : v) : d;
}

static bool jsonGetBool(const std::string& s, const std::string& key, bool d) {
    size_t i; if (!jsonFindKey(s, key, i)) return d;
    if (i+4 <= s.size() && s.compare(i, 4, "true")  == 0) return true;
    if (i+5 <= s.size() && s.compare(i, 5, "false") == 0) return false;
    return d;
}

// 找 key 对应 array 的 [start, end) 闭区间括号位置（不含外层 []）。返 false = 没找到 array。
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

// 切 array 内的每个对象元素 {...}（顶层逗号分隔）。返回 substring 列表。
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

// ─── helpers ────────────────────────────────────────────────────────────────

static std::string err(const char* code, const std::string& msg) {
    return std::string("{\"ok\":false,\"error\":{\"code\":\"") + code + "\",\"message\":\"" + msg + "\"}}";
}

static void stbWriteCb(void* ctx, void* data, int size) {
    auto* v = (std::vector<uint8_t>*)ctx;
    v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
}

static const char* sniffFormat(const uint8_t* p, size_t n) {
    if (n >= 8 && p[0]==0x89 && p[1]=='P' && p[2]=='N' && p[3]=='G') return "png";
    if (n >= 3 && p[0]==0xFF && p[1]==0xD8 && p[2]==0xFF) return "jpeg";
    if (n >= 12 && p[0]=='R' && p[1]=='I' && p[2]=='F' && p[3]=='F' &&
        p[8]=='W' && p[9]=='E' && p[10]=='B' && p[11]=='P') return "webp";
    if (n >= 2 && p[0]=='B' && p[1]=='M') return "bmp";
    if (n >= 6 && p[0]=='G' && p[1]=='I' && p[2]=='F') return "gif";
    return "unknown";
}

// ─── decode / encode 工具 ────────────────────────────────────────────────────

// 解 encoded bytes 到统一 RGBA8888 buffer（堆分配，caller free）。
struct Pixels {
    std::vector<uint8_t> rgba;
    int w, h;
    const char* src_format;  // 静态字符串，sniff 结果
};

static bool decodeBytes(const std::vector<uint8_t>& bytes, Pixels& out) {
    out.w = out.h = 0;
    out.src_format = sniffFormat(bytes.data(), bytes.size());

    if (std::strcmp(out.src_format, "webp") == 0) {
        int w = 0, h = 0;
        uint8_t* p = WebPDecodeRGBA(bytes.data(), bytes.size(), &w, &h);
        if (!p) return false;
        out.rgba.assign(p, p + (size_t)w * h * 4);
        WebPFree(p);
        out.w = w; out.h = h;
        return true;
    }

    int w = 0, h = 0, ch = 0;
    uint8_t* p = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &ch, 4);
    if (!p) return false;
    out.rgba.assign(p, p + (size_t)w * h * 4);
    stbi_image_free(p);
    out.w = w; out.h = h;
    return true;
}

// pixels → encoded bytes。format 已 normalize（jpg/jpeg 统一）。
static bool encodePixels(const std::vector<uint8_t>& rgba, int w, int h,
                         std::string& format, int quality, bool lossless,
                         std::vector<uint8_t>& out) {
    if (format == "jpg") format = "jpeg";

    if (format == "png") {
        return stbi_write_png_to_func(stbWriteCb, &out, w, h, 4, rgba.data(), w * 4);
    } else if (format == "jpeg") {
        // JPEG 没 alpha：展平 RGBA → RGB
        std::vector<uint8_t> rgb((size_t)w * h * 3);
        for (int i = 0; i < w * h; i++) {
            rgb[i*3+0] = rgba[i*4+0];
            rgb[i*3+1] = rgba[i*4+1];
            rgb[i*3+2] = rgba[i*4+2];
        }
        int q = quality > 0 && quality <= 100 ? quality : 85;
        return stbi_write_jpg_to_func(stbWriteCb, &out, w, h, 3, rgb.data(), q);
    } else if (format == "bmp") {
        return stbi_write_bmp_to_func(stbWriteCb, &out, w, h, 4, rgba.data());
    } else if (format == "webp") {
        uint8_t* webp_out = nullptr; size_t webp_len = 0;
        if (lossless) {
            webp_len = WebPEncodeLosslessRGBA(rgba.data(), w, h, w * 4, &webp_out);
        } else {
            float q = quality > 0 && quality <= 100 ? (float)quality : 85.0f;
            webp_len = WebPEncodeRGBA(rgba.data(), w, h, w * 4, q, &webp_out);
        }
        if (webp_len == 0 || !webp_out) return false;
        out.assign(webp_out, webp_out + webp_len);
        WebPFree(webp_out);
        return true;
    }
    return false;
}

// ─── transforms：操作 RGBA pixels 原地或新 buffer ─────────────────────────

static bool opResize(Pixels& p, int new_w, int new_h) {
    if (new_w <= 0 || new_h <= 0) return false;
    std::vector<uint8_t> dst((size_t)new_w * new_h * 4);
    // stb_image_resize2: 单 call 默认 sRGB 高质量
    if (!stbir_resize_uint8_srgb(p.rgba.data(), p.w, p.h, p.w * 4,
                                  dst.data(), new_w, new_h, new_w * 4,
                                  STBIR_RGBA)) return false;
    p.rgba = std::move(dst);
    p.w = new_w; p.h = new_h;
    return true;
}

static bool opCrop(Pixels& p, int x, int y, int cw, int ch) {
    if (x < 0 || y < 0 || cw <= 0 || ch <= 0) return false;
    if (x + cw > p.w || y + ch > p.h) return false;
    std::vector<uint8_t> dst((size_t)cw * ch * 4);
    for (int j = 0; j < ch; j++) {
        std::memcpy(dst.data() + (size_t)j * cw * 4,
                    p.rgba.data() + (size_t)(y + j) * p.w * 4 + (size_t)x * 4,
                    (size_t)cw * 4);
    }
    p.rgba = std::move(dst); p.w = cw; p.h = ch;
    return true;
}

static bool opRotate(Pixels& p, int deg) {
    // 仅 90 倍数；其它返 false 让 caller 出 INVALID_OP envelope
    deg = ((deg % 360) + 360) % 360;
    if (deg == 0) return true;
    int w = p.w, h = p.h;
    std::vector<uint8_t> dst(p.rgba.size());
    if (deg == 180) {
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
            const uint8_t* s = &p.rgba[(j * w + i) * 4];
            uint8_t* d = &dst[((h-1-j) * w + (w-1-i)) * 4];
            d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3];
        }
        p.rgba = std::move(dst);
        return true;
    }
    if (deg == 90 || deg == 270) {
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
            const uint8_t* s = &p.rgba[(j * w + i) * 4];
            int ni, nj;
            if (deg == 90)  { ni = h - 1 - j; nj = i; }
            else            { ni = j;         nj = w - 1 - i; }
            uint8_t* d = &dst[(nj * h + ni) * 4];
            d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3];
        }
        p.rgba = std::move(dst);
        std::swap(p.w, p.h);
        return true;
    }
    return false;
}

static bool opFlip(Pixels& p, const std::string& axis) {
    int w = p.w, h = p.h;
    if (axis == "x") {  // 水平翻 = 沿 y 轴对称（左右反）
        for (int j = 0; j < h; j++) {
            uint8_t* row = &p.rgba[(size_t)j * w * 4];
            for (int i = 0; i < w / 2; i++) {
                uint8_t* a = row + i * 4;
                uint8_t* b = row + (w - 1 - i) * 4;
                std::swap(a[0], b[0]); std::swap(a[1], b[1]);
                std::swap(a[2], b[2]); std::swap(a[3], b[3]);
            }
        }
        return true;
    }
    if (axis == "y") {  // 垂直翻 = 沿 x 轴对称（上下反）
        for (int j = 0; j < h / 2; j++) {
            uint8_t* a = &p.rgba[(size_t)j * w * 4];
            uint8_t* b = &p.rgba[(size_t)(h - 1 - j) * w * 4];
            for (int i = 0; i < w * 4; i++) std::swap(a[i], b[i]);
        }
        return true;
    }
    return false;
}

// 应用一个 op JSON object。
static bool applyOp(Pixels& p, const std::string& op_json, std::string& errMsg) {
    std::string kind = jsonGetString(op_json, "kind");
    if (kind == "resize") {
        int w = jsonGetInt(op_json, "w", 0);
        int h = jsonGetInt(op_json, "h", 0);
        if (!opResize(p, w, h)) { errMsg = "resize: bad dimensions"; return false; }
        return true;
    } else if (kind == "crop") {
        int x = jsonGetInt(op_json, "x", 0);
        int y = jsonGetInt(op_json, "y", 0);
        int w = jsonGetInt(op_json, "w", 0);
        int h = jsonGetInt(op_json, "h", 0);
        if (!opCrop(p, x, y, w, h)) { errMsg = "crop: out of bounds"; return false; }
        return true;
    } else if (kind == "rotate") {
        int deg = jsonGetInt(op_json, "degrees", 0);
        if (!opRotate(p, deg)) { errMsg = "rotate: only 90/180/270 supported"; return false; }
        return true;
    } else if (kind == "flip") {
        std::string axis = jsonGetString(op_json, "axis");
        if (!opFlip(p, axis)) { errMsg = "flip: axis must be 'x' or 'y'"; return false; }
        return true;
    }
    errMsg = "unknown op kind: " + kind;
    return false;
}

// ─── actions ────────────────────────────────────────────────────────────────

static std::string doMetadata(const std::string& params) {
    std::string in_b64 = jsonGetString(params, "input_b64");
    if (in_b64.empty()) return err("INVALID_PARAMS", "need input_b64");
    auto bytes = b64Decode(in_b64);
    if (bytes.empty()) return err("INVALID_PARAMS", "bad base64");
    const char* fmt = sniffFormat(bytes.data(), bytes.size());
    int w = 0, h = 0, ch = 0;
    if (std::strcmp(fmt, "webp") == 0) {
        if (!WebPGetInfo(bytes.data(), bytes.size(), &w, &h)) return err("METADATA", "WebPGetInfo failed");
        ch = 4;
    } else {
        if (!stbi_info_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &ch))
            return err("METADATA", "stbi_info failed");
    }
    std::string r = "{\"ok\":true,\"data\":{\"width\":"; r += std::to_string(w);
    r += ",\"height\":"; r += std::to_string(h);
    r += ",\"channels\":"; r += std::to_string(ch);
    r += ",\"format\":\""; r += fmt; r += "\"}}";
    return r;
}

static std::string doDecode(const std::string& params) {
    std::string in_b64 = jsonGetString(params, "input_b64");
    if (in_b64.empty()) return err("INVALID_PARAMS", "need input_b64");
    auto bytes = b64Decode(in_b64);
    if (bytes.empty()) return err("INVALID_PARAMS", "bad base64");

    Pixels p;
    if (!decodeBytes(bytes, p)) return err("DECODE", "unsupported or corrupt input");

    std::string b64 = b64Encode(p.rgba.data(), p.rgba.size());
    std::string r = "{\"ok\":true,\"data\":{\"pixels_b64\":\""; r += b64;
    r += "\",\"width\":";  r += std::to_string(p.w);
    r += ",\"height\":";   r += std::to_string(p.h);
    r += ",\"channels\":4,\"src_format\":\""; r += p.src_format; r += "\"}}";
    return r;
}

static std::string doEncode(const std::string& params) {
    std::string px_b64 = jsonGetString(params, "pixels_b64");
    int w = jsonGetInt(params, "width", 0);
    int h = jsonGetInt(params, "height", 0);
    std::string fmt = jsonGetString(params, "format");
    int quality = jsonGetInt(params, "quality", 85);
    bool lossless = jsonGetBool(params, "lossless", false);

    if (px_b64.empty() || w <= 0 || h <= 0 || fmt.empty())
        return err("INVALID_PARAMS", "need pixels_b64 + width + height + format");

    auto px = b64Decode(px_b64);
    if ((int)px.size() < w * h * 4) return err("INVALID_PARAMS", "pixels too short");

    std::vector<uint8_t> out;
    if (!encodePixels(px, w, h, fmt, quality, lossless, out))
        return err("ENCODE", "unsupported output format: " + fmt);

    std::string b64 = b64Encode(out.data(), out.size());
    std::string r = "{\"ok\":true,\"data\":{\"output_b64\":\""; r += b64;
    r += "\",\"format\":\""; r += fmt; r += "\"}}";
    return r;
}

static std::string doProcess(const std::string& params) {
    std::string in_b64 = jsonGetString(params, "input_b64");
    if (in_b64.empty()) return err("INVALID_PARAMS", "need input_b64");

    auto bytes = b64Decode(in_b64);
    if (bytes.empty()) return err("INVALID_PARAMS", "bad base64");

    Pixels p;
    if (!decodeBytes(bytes, p)) return err("DECODE", "unsupported or corrupt input");

    // 应用 ops 数组（可选）
    size_t aStart = 0, aEnd = 0;
    if (jsonFindArray(params, "ops", aStart, aEnd)) {
        auto items = jsonSplitObjArray(params, aStart, aEnd);
        for (const auto& op : items) {
            std::string errMsg;
            if (!applyOp(p, op, errMsg)) return err("INVALID_OP", errMsg);
        }
    }

    // output_format 缺省 = src_format（纯 transform 场景）
    std::string fmt = jsonGetString(params, "output_format", p.src_format);
    int quality = jsonGetInt(params, "quality", 85);
    bool lossless = jsonGetBool(params, "lossless", false);

    std::vector<uint8_t> out;
    if (!encodePixels(p.rgba, p.w, p.h, fmt, quality, lossless, out))
        return err("ENCODE", "unsupported output format: " + fmt);

    std::string b64 = b64Encode(out.data(), out.size());
    std::string r = "{\"ok\":true,\"data\":{\"output_b64\":\""; r += b64;
    r += "\",\"width\":";  r += std::to_string(p.w);
    r += ",\"height\":";   r += std::to_string(p.h);
    r += ",\"format\":\""; r += fmt; r += "\"}}";
    return r;
}

// ─── dispatch ────────────────────────────────────────────────────────────────

uint64_t plugin_dispatch(uint32_t ap, uint32_t al, uint32_t pp, uint32_t pl) {
    std::string action((const char*)(uintptr_t)ap, al);
    std::string params((const char*)(uintptr_t)pp, pl);

    std::string out;
    if (action == "metadata")     out = doMetadata(params);
    else if (action == "decode")  out = doDecode(params);
    else if (action == "encode")  out = doEncode(params);
    else if (action == "process") out = doProcess(params);
    else out = err("UNKNOWN_ACTION", std::string("image.") + action);

    size_t n = out.size();
    char* buf = (char*)std::malloc(n ? n : 1);
    if (n) std::memcpy(buf, out.data(), n);
    return ((uint64_t)(uint32_t)(uintptr_t)buf << 32) | (uint64_t)n;
}
