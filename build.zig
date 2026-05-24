//! Build entry for community wasm plugins.
//!
//!   zig build           # build everything
//!   zig build barcode   # only barcode/dist/barcode.wasm
//!   zig build image     # only image/dist/image.wasm
//!
//! Deps (zxing-cpp, libwebp) are declared in build.zig.zon and fetched by
//! `zig fetch` automatically. Each plugin still uses cmake + emscripten
//! internally (zxing-cpp throws, image uses libwebp's CMake config).
//!
//! Requires emscripten in PATH (emcc / emcmake).

const std = @import("std");

pub fn build(b: *std.Build) void {
    const all = b.step("all", "Build every plugin");
    b.default_step.dependOn(all);

    const zxing = b.dependency("zxing_cpp", .{}).path("");
    const webp = b.dependency("libwebp", .{}).path("");
    const archive_dep = b.dependency("libarchive", .{}).path("");

    all.dependOn(addPlugin(b, .{ .id = "barcode", .dep_env = "ZXING_CPP_ROOT", .dep_path = zxing }));
    all.dependOn(addPlugin(b, .{ .id = "image", .dep_env = "LIBWEBP_ROOT", .dep_path = webp }));
    all.dependOn(addPlugin(b, .{ .id = "archive", .dep_env = "LIBARCHIVE_ROOT", .dep_path = archive_dep }));
}

const PluginSpec = struct {
    id: []const u8,
    dep_env: []const u8,
    dep_path: std.Build.LazyPath,
};

fn addPlugin(b: *std.Build, spec: PluginSpec) *std.Build.Step {
    const dist_rel = b.fmt("{s}/dist", .{spec.id});
    const cache_rel = b.fmt("{s}/build", .{spec.id});

    const configure = b.addSystemCommand(&.{ "emcmake", "cmake", "-S", b.pathFromRoot(spec.id), "-B" });
    configure.addArg(b.pathFromRoot(cache_rel));
    configure.addArg("-DCMAKE_BUILD_TYPE=Release");
    configure.setEnvironmentVariable(spec.dep_env, spec.dep_path.getPath3(b, null).toString(b.allocator) catch @panic("OOM"));

    const build_cmd = b.addSystemCommand(&.{ "cmake", "--build" });
    build_cmd.addArg(b.pathFromRoot(cache_rel));
    build_cmd.addArg("-j");
    build_cmd.step.dependOn(&configure.step);

    const stage = b.addSystemCommand(&.{ "sh", "-c" });
    stage.addArg(b.fmt(
        \\set -e
        \\mkdir -p {s}
        \\cp {s}/{s}.wasm {s}/{s}.wasm
    , .{
        b.pathFromRoot(dist_rel),
        b.pathFromRoot(cache_rel),
        spec.id,
        b.pathFromRoot(dist_rel),
        spec.id,
    }));
    stage.step.dependOn(&build_cmd.step);

    const step = b.step(spec.id, b.fmt("Build {s}/dist/{s}.wasm", .{ spec.id, spec.id }));
    step.dependOn(&stage.step);
    return step;
}
