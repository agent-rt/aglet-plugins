// markdown plugin —— md4c standalone wasm wrapper（CommonMark + GFM → HTML）。
//
// Built on aglet_plugin_sdk（sdk/c/aglet_plugin.h）：SDK 拥有 alloc/free/dispatch
// 三个 wasm export、JSON 解析、错误信封，本文件只放 action handler。
//
// 纯计算插件：md4c 是纯 C，无 throw、无 I/O、无 OS 依赖，故不需要 -fwasm-exceptions
// （不像 barcode/zxing），产物无 EH 指令 → 任意 wasm 运行模式都能跑。
//
// Protocol (JSON in/out):
//   action="render"
//     params:  {"text":"# hello\n\n- a\n- b"}
//     result:  {"ok":true,"data":{"html":"<h1>hello</h1>\n<ul>..."}}
//   error:     {"ok":false,"error":{"code":"...","message":"..."}}

#include <aglet_plugin.h>

#include <string>
#include <string_view>

extern "C" {
#include "md4c-html.h"
}

// md_html 把渲染结果分块回调出来；攒进 std::string。
static void appendChunk(const MD_CHAR* text, MD_SIZE size, void* userdata) {
    static_cast<std::string*>(userdata)->append(text, size);
}

static std::string doRender(const aglet::Params& p) {
    const std::string text = p.strOr("text", "");
    if (text.empty()) return aglet::Result::ok().str("html", "");

    // GFM 方言（表格 / 删除线 / 任务列表 / 自动链接 / 脚注 / admonition）。
    const unsigned parser_flags = MD_DIALECT_GITHUB;
    const unsigned renderer_flags = 0;

    std::string html;
    const int rc = md_html(
        text.data(), static_cast<MD_SIZE>(text.size()),
        appendChunk, &html, parser_flags, renderer_flags);
    if (rc != 0) return aglet::err("RENDER", "md4c md_html failed");

    return aglet::Result::ok().str("html", html);
}

std::string aglet_dispatch_action(std::string_view action,
                                  std::string_view params_json) {
    aglet::Params p(params_json);
    if (action == "render") return doRender(p);
    return aglet::errUnknown(action);
}

AGLET_PLUGIN_EXPORTS
