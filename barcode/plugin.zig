//! barcode plugin —— wasm backend，第一个 wasm 路径 dogfood。
//!
//! 这个模块只做 data carrier：@embedFile 把 manifest 跟 .wasm 字节内联进
//! Aglet.app/CLI binary，注册逻辑在 host.zig / ffi.zig 走 aglet-host 的
//! wasm_plugin.{init, provider} 跟 sysinfo（static）同模式接 runtime.plugins.register。
//!
//! 不引用 aglet-host —— 避免循环（host 也要 import 这个 module）。
//! 真注册路径见 aglet-host/src/host.zig 的 barcode_ctx 块。

pub const NAMESPACE: []const u8 = "barcode";
pub const MANIFEST_JSON: []const u8 = @embedFile("plugin.json");
pub const WASM_BYTES: []const u8 = @embedFile("dist/barcode.wasm");
