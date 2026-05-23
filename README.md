# aglet-plugins

Community wasm plugin sources for [Aglet](https://github.com/agent-rt/aglet).
Each plugin is a pure-compute capability that aglets can declare in
`manifest.requires[]`. Built via emscripten, sandboxed via wasmtime in the
host runtime.

CI/CD auto-publishes merged plugins to
[aglet-registry](https://github.com/agent-rt/aglet-registry) under
`plugins/<id>/<version>.aplugin`. Users never install plugins directly —
they install aglets, and dependencies are resolved automatically.

## Layout

```
aglet-plugins/
  <plugin-id>/
    plugin.json          # manifest (manifest.plugin: true + namespace + actions[])
    plugin.zig           # data carrier (@embedFile manifest + wasm; used by aglet host for bundled override)
    src/wrapper.cpp      # C/C++ source — exports alloc/free/dispatch/memory
    CMakeLists.txt       # emscripten build config
    build.sh             # one-shot: emcmake + emmake → dist/<id>.wasm
    dist/<id>.wasm       # built artifact (committed; CI rebuilds on PR)
    README.md            # plugin-specific docs
    LICENSE              # SPDX
```

## Plugin format

See [aglet-registry/PLUGINS.md](https://github.com/agent-rt/aglet-registry/blob/main/PLUGINS.md)
for the canonical wasm ABI + meta.json schema.

Wasm exports required:
- `alloc(n: i32) -> i32`
- `free(ptr: i32, n: i32) -> void`
- `dispatch(ap, al, pp, pl) -> i64` (packed `(result_ptr << 32) | result_len`)
- `memory` (WebAssembly.Memory)

Imports whitelist (host stubs these):
- `env.emscripten_notify_memory_growth`
- `wasi_snapshot_preview1.{fd_close, fd_write, fd_seek}`

**Any other import triggers PR rejection** —
see [REVIEW_PROCESS.md Step 6](https://github.com/agent-rt/aglet-registry/blob/main/REVIEW_PROCESS.md).

## Dev setup

Build needs emscripten + libwebp / zxing-cpp / etc as wasm-side deps. Dev
flow uses the aglet repo's `zig-pkg/` (already fetched via `just build-zig`)
as sibling checkout:

```sh
git clone https://github.com/agent-rt/aglet
cd aglet && just build-zig    # fetches deps to ./zig-pkg/
cd ../aglet-plugins/<id> && ./build.sh
```

Override via `$AGLET_DEPS_DIR=/path/to/zig-pkg` if you don't have an aglet
checkout sibling.

CI doesn't have a sibling aglet repo; it'll use `vendor/zig-pkg/` populated
by `scripts/fetch-deps.sh` (TODO Phase B).

## Publishing

`aglet plugin publish <plugin-id>/plugin.json` opens a PR to aglet-registry.
CI runs this automatically on merge to main.

## Current plugins

| id | version | namespace | actions |
|---|---|---|---|
| `barcode` | 1.0.0 | barcode | encode / decode (zxing-cpp v3.0.2) |
| `image` | 2.0.0 | image | metadata / decode / encode / process (stb + libwebp) |

## Contributing

PRs welcome. See aglet-registry's REVIEW_PROCESS.md Step 6 for the wasm-
specific checks your PR must pass before being published.

License: MIT (see per-plugin LICENSE).
