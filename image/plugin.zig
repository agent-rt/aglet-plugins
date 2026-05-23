//! image plugin —— wasm backend，第二个 wasm 路径 dogfood。
//!
//! 跟 barcode 同模式：data carrier only。@embedFile manifest + .wasm；
//! 注册由 host.zig 用 aglet_host.wasm_plugin.{init, provider} 接 runtime。
//!
//! 不引 aglet 避循环。
//!
//! Actions: convert / metadata。
//! Formats: PNG / JPEG / WebP / BMP (encode)；以上 + GIF / PSD / TGA / PNM (decode via stb)。

pub const NAMESPACE: []const u8 = "image";
pub const MANIFEST_JSON: []const u8 = @embedFile("plugin.json");
pub const WASM_BYTES: []const u8 = @embedFile("dist/image.wasm");
