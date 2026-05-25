# aglet-plugin-sdk

Helpers for writing **wasm plugins** for the [Aglet](https://aglet.dev) runtime.

## What this gives you

| Layer | Without SDK | With SDK |
|-------|-------------|----------|
| wasm runtime exports (`alloc` / `free` / `dispatch`) | hand-rolled per plugin (~40 LOC) | `comptime { sdk.exportRuntime(); }` |
| Action dispatch switch | hand-rolled (~10 LOC + 1 per action) | `sdk.runDispatch(Handlers, ...)` (zero-LOC dispatch) |
| Input JSON parsing | hand-rolled getters (~30 LOC) | `p.str("algo")` / `p.bytes("data_b64")` |
| Output envelope | hand-rolled `std.fmt.allocPrint` per result type | `sdk.okBytes(a, "key", &bytes)` / `sdk.ok(a, .{...})` |
| base64 encode/decode | hand-rolled (~10 LOC) | `sdk.encodeB64` / `sdk.decodeB64` |
| Error envelope | hand-rolled per code | `sdk.err(a, code, msg)` / `sdk.errInvalid(a, msg)` |

**Net**: a typical plugin loses ~100 lines of boilerplate.

## Minimal plugin (Zig wasm32-wasi)

```zig
// my-plugin/src/wrapper.zig
const std = @import("std");
const sdk = @import("aglet_plugin_sdk");

const Handlers = struct {
    pub fn echo(p: *sdk.Params) anyerror![]const u8 {
        const msg = p.str("msg") orelse "(empty)";
        return sdk.okStr(p.arena, "echo", msg);
    }

    pub fn add(p: *sdk.Params) anyerror![]const u8 {
        const a = p.int("a", 0);
        const b = p.int("b", 0);
        return sdk.okInt(p.arena, "sum", a + b);
    }
};

comptime { sdk.exportRuntime(); }

export fn dispatch(ap: u32, al: u32, pp: u32, pl: u32) callconv(.c) u64 {
    return sdk.runDispatch(Handlers, ap, al, pp, pl);
}
```

That's the entire plugin module. Pair it with a `plugin.json` describing the
actions (host validates declarations against runtime calls) and you have a
shipping plugin.

## What's in this directory

```
sdk/
├── README.md              # this file
├── plugin.schema.json     # JSON Schema for plugin.json (IDE / CI validation)
├── zig/
│   ├── plugin.zig         # SDK module — Params / Result / runDispatch / exportRuntime
│   └── build.zig.zon      # Zig package metadata (Aglet plugins import via b.dependency)
├── c/                     # (TODO) C/C++ SDK for emscripten plugins
└── templates/             # (TODO) `aglet plugin new <id>` scaffolding source
```

## Build integration

The repo-level `build.zig` exposes `addZigPlugin(b, "<id>")`. It auto-wires
`aglet_plugin_sdk` as an import. Inside plugin source:

```zig
const sdk = @import("aglet_plugin_sdk");
```

## Static plugins

Sandboxed wasm is the right shape for community plugins. If you need OS
APIs (clipboard / filesystem / camera / etc), you're writing a **static
plugin** which lives in the closed-source Aglet runtime repo. Those still
follow the same `plugin.json` schema but link against the runtime directly.
The SDK here is wasm-only.

## License

MIT (see ../LICENSE)
