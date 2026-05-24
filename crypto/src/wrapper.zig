//! crypto plugin — pure-zig wasm32-wasi using std.crypto.
//!
//! Actions (JSON in/out, fields are base64 strings):
//!   hash({algo, data_b64}) → {digest_b64}            algo ∈ sha256|sha512|blake2b
//!   hmac({algo, key_b64, data_b64}) → {mac_b64}      algo ∈ sha256|sha512
//!   kdf({password, salt_b64, opslimit, memlimit, key_bytes}) → {key_b64}   Argon2id
//!   encrypt({key_b64, plaintext_b64, ad_b64}) → {ciphertext_b64, nonce_b64} XChaCha20-Poly1305-IETF
//!   decrypt({key_b64, ciphertext_b64, nonce_b64, ad_b64}) → {plaintext_b64}
//!   keypair() → {pub_b64 (32B), sec_b64 (64B)}       Ed25519
//!   sign({sec_b64, data_b64}) → {sig_b64}
//!   verify({pub_b64, data_b64, sig_b64}) → {ok}
//!   random({n}) → {bytes_b64}                        CSPRNG (getrandom via WASI)

const std = @import("std");
const crypto = std.crypto;

/// Single-threaded std.Io (wasm has no threads). Used wherever zig stdlib
/// requires an Io param (argon2 KDF, Ed25519 keypair generation, etc).
var io_inst: std.Io.Threaded = .init_single_threaded;
fn io() std.Io {
    return io_inst.io();
}

/// WASI CSPRNG. Host stubs `random_get` via its sandboxed RNG (Aglet runtime
/// fills it from the OS getrandom equivalent).
fn csprng(buf: []u8) void {
    _ = std.os.wasi.random_get(buf.ptr, buf.len);
}

// ─── allocator ──────────────────────────────────────────────────────────────
// One global page allocator for host-visible buffers (alloc/free wasm exports).

const allocator = std.heap.wasm_allocator;

// ─── exports ────────────────────────────────────────────────────────────────

export fn alloc(n: u32) u32 {
    const len = if (n == 0) 1 else n;
    const buf = allocator.alloc(u8, len) catch return 0;
    return @intCast(@intFromPtr(buf.ptr));
}

export fn free(p: u32, n: u32) void {
    if (p == 0) return;
    const ptr: [*]u8 = @ptrFromInt(p);
    allocator.free(ptr[0..@max(n, 1)]);
}

export fn dispatch(ap: u32, al: u32, pp: u32, pl: u32) u64 {
    const action = mkSlice(ap, al);
    const params = mkSlice(pp, pl);

    var arena = std.heap.ArenaAllocator.init(allocator);
    defer arena.deinit();
    const a = arena.allocator();

    const out = handle(a, action, params) catch |e|
        errMsg(a, "INTERNAL", @errorName(e)) catch return 0;

    // Allocate persistent buffer (caller frees via free()).
    const buf = allocator.alloc(u8, if (out.len == 0) 1 else out.len) catch return 0;
    @memcpy(buf[0..out.len], out);
    return (@as(u64, @intCast(@intFromPtr(buf.ptr))) << 32) | @as(u64, @intCast(out.len));
}

fn mkSlice(p: u32, n: u32) []const u8 {
    if (n == 0) return &[_]u8{};
    const ptr: [*]const u8 = @ptrFromInt(p);
    return ptr[0..n];
}

// ─── dispatch ───────────────────────────────────────────────────────────────

fn handle(a: std.mem.Allocator, action: []const u8, params: []const u8) ![]const u8 {
    if (std.mem.eql(u8, action, "hash"))     return try doHash(a, params);
    if (std.mem.eql(u8, action, "hmac"))     return try doHmac(a, params);
    if (std.mem.eql(u8, action, "kdf"))      return try doKdf(a, params);
    if (std.mem.eql(u8, action, "encrypt"))  return try doEncrypt(a, params);
    if (std.mem.eql(u8, action, "decrypt"))  return try doDecrypt(a, params);
    if (std.mem.eql(u8, action, "keypair"))  return try doKeypair(a);
    if (std.mem.eql(u8, action, "sign"))     return try doSign(a, params);
    if (std.mem.eql(u8, action, "verify"))   return try doVerify(a, params);
    if (std.mem.eql(u8, action, "random"))   return try doRandom(a, params);
    return try errMsg(a, "UNKNOWN_ACTION", action);
}

// ─── JSON helpers (parse the input via std.json; write output via raw fmt) ──

const JsonValue = std.json.Value;

fn parseObj(a: std.mem.Allocator, json: []const u8) !std.json.Parsed(JsonValue) {
    return std.json.parseFromSlice(JsonValue, a, json, .{});
}

fn getStr(obj: JsonValue, key: []const u8) ?[]const u8 {
    if (obj != .object) return null;
    const v = obj.object.get(key) orelse return null;
    return if (v == .string) v.string else null;
}

fn getInt(obj: JsonValue, key: []const u8, default: i64) i64 {
    if (obj != .object) return default;
    const v = obj.object.get(key) orelse return default;
    return switch (v) {
        .integer => |i| i,
        .float => |f| @intFromFloat(f),
        else => default,
    };
}

fn decodeB64(a: std.mem.Allocator, s: []const u8) ![]u8 {
    const dec = std.base64.standard.Decoder;
    const n = try dec.calcSizeForSlice(s);
    const out = try a.alloc(u8, n);
    try dec.decode(out, s);
    return out;
}

fn encodeB64(a: std.mem.Allocator, bytes: []const u8) ![]const u8 {
    const enc = std.base64.standard.Encoder;
    const n = enc.calcSize(bytes.len);
    const out = try a.alloc(u8, n);
    return enc.encode(out, bytes);
}

fn okOne(a: std.mem.Allocator, key: []const u8, value_b64: []const u8) ![]const u8 {
    return try std.fmt.allocPrint(a, "{{\"ok\":true,\"{s}\":\"{s}\"}}", .{ key, value_b64 });
}

fn errMsg(a: std.mem.Allocator, code: []const u8, msg: []const u8) ![]const u8 {
    return try std.fmt.allocPrint(a,
        "{{\"ok\":false,\"error\":{{\"code\":\"{s}\",\"message\":\"{s}\"}}}}",
        .{ code, msg });
}

fn errInvalid(a: std.mem.Allocator, msg: []const u8) ![]const u8 {
    return try errMsg(a, "INVALID_PARAMS", msg);
}

// ─── actions ────────────────────────────────────────────────────────────────

fn doHash(a: std.mem.Allocator, params: []const u8) ![]const u8 {
    var p = try parseObj(a, params);
    defer p.deinit();
    const algo = getStr(p.value, "algo") orelse "blake2b";
    const data_b64 = getStr(p.value, "data_b64") orelse "";
    const data = try decodeB64(a, data_b64);

    if (std.mem.eql(u8, algo, "sha256")) {
        var out: [crypto.hash.sha2.Sha256.digest_length]u8 = undefined;
        crypto.hash.sha2.Sha256.hash(data, &out, .{});
        return okOne(a, "digest_b64", try encodeB64(a, &out));
    }
    if (std.mem.eql(u8, algo, "sha512")) {
        var out: [crypto.hash.sha2.Sha512.digest_length]u8 = undefined;
        crypto.hash.sha2.Sha512.hash(data, &out, .{});
        return okOne(a, "digest_b64", try encodeB64(a, &out));
    }
    if (std.mem.eql(u8, algo, "blake2b")) {
        var out: [crypto.hash.blake2.Blake2b256.digest_length]u8 = undefined;
        crypto.hash.blake2.Blake2b256.hash(data, &out, .{});
        return okOne(a, "digest_b64", try encodeB64(a, &out));
    }
    return errInvalid(a, "algo must be sha256 / sha512 / blake2b");
}

fn doHmac(a: std.mem.Allocator, params: []const u8) ![]const u8 {
    var p = try parseObj(a, params);
    defer p.deinit();
    const algo = getStr(p.value, "algo") orelse "sha256";
    const key = try decodeB64(a, getStr(p.value, "key_b64") orelse "");
    const data = try decodeB64(a, getStr(p.value, "data_b64") orelse "");

    if (std.mem.eql(u8, algo, "sha256")) {
        const H = crypto.auth.hmac.sha2.HmacSha256;
        var out: [H.mac_length]u8 = undefined;
        H.create(&out, data, key);
        return okOne(a, "mac_b64", try encodeB64(a, &out));
    }
    if (std.mem.eql(u8, algo, "sha512")) {
        const H = crypto.auth.hmac.sha2.HmacSha512;
        var out: [H.mac_length]u8 = undefined;
        H.create(&out, data, key);
        return okOne(a, "mac_b64", try encodeB64(a, &out));
    }
    return errInvalid(a, "algo must be sha256 / sha512");
}

fn doKdf(a: std.mem.Allocator, params: []const u8) ![]const u8 {
    var p = try parseObj(a, params);
    defer p.deinit();
    const password = getStr(p.value, "password") orelse "";
    const salt = try decodeB64(a, getStr(p.value, "salt_b64") orelse "");
    const opslimit = getInt(p.value, "opslimit", 3);
    const memlimit_kb = getInt(p.value, "memlimit", 64 * 1024); // 64 MB in KiB
    const key_bytes = getInt(p.value, "key_bytes", 32);

    if (salt.len < 8) return errInvalid(a, "salt must be ≥ 8 bytes");
    if (key_bytes < 16 or key_bytes > 1024) return errInvalid(a, "key_bytes out of range (16..1024)");

    const out = try a.alloc(u8, @intCast(key_bytes));
    const params_arg = crypto.pwhash.argon2.Params{
        .t = @intCast(opslimit),
        .m = @intCast(memlimit_kb),
        .p = 1,
    };
    crypto.pwhash.argon2.kdf(a, out, password, salt, params_arg, .argon2id, io()) catch
        return errMsg(a, "KDF_FAILED", "argon2id failed (likely out of memory)");
    return okOne(a, "key_b64", try encodeB64(a, out));
}

fn doEncrypt(a: std.mem.Allocator, params: []const u8) ![]const u8 {
    var p = try parseObj(a, params);
    defer p.deinit();
    const key = try decodeB64(a, getStr(p.value, "key_b64") orelse "");
    const pt = try decodeB64(a, getStr(p.value, "plaintext_b64") orelse "");
    const ad = try decodeB64(a, getStr(p.value, "ad_b64") orelse "");
    if (key.len != 32) return errInvalid(a, "key must be 32 bytes");

    const AEAD = crypto.aead.chacha_poly.XChaCha20Poly1305;
    var nonce: [AEAD.nonce_length]u8 = undefined;
    csprng(&nonce);

    const ct = try a.alloc(u8, pt.len);
    var tag: [AEAD.tag_length]u8 = undefined;
    var key_arr: [AEAD.key_length]u8 = undefined;
    @memcpy(&key_arr, key);
    AEAD.encrypt(ct, &tag, pt, ad, nonce, key_arr);

    // libsodium-style: append tag to ciphertext, single output.
    const full = try a.alloc(u8, ct.len + tag.len);
    @memcpy(full[0..ct.len], ct);
    @memcpy(full[ct.len..], &tag);

    return try std.fmt.allocPrint(a,
        "{{\"ok\":true,\"ciphertext_b64\":\"{s}\",\"nonce_b64\":\"{s}\"}}",
        .{ try encodeB64(a, full), try encodeB64(a, &nonce) });
}

fn doDecrypt(a: std.mem.Allocator, params: []const u8) ![]const u8 {
    var p = try parseObj(a, params);
    defer p.deinit();
    const key = try decodeB64(a, getStr(p.value, "key_b64") orelse "");
    const ct_with_tag = try decodeB64(a, getStr(p.value, "ciphertext_b64") orelse "");
    const nonce_in = try decodeB64(a, getStr(p.value, "nonce_b64") orelse "");
    const ad = try decodeB64(a, getStr(p.value, "ad_b64") orelse "");
    const AEAD = crypto.aead.chacha_poly.XChaCha20Poly1305;
    if (key.len != AEAD.key_length) return errInvalid(a, "key must be 32 bytes");
    if (nonce_in.len != AEAD.nonce_length) return errInvalid(a, "nonce must be 24 bytes");
    if (ct_with_tag.len < AEAD.tag_length) return errInvalid(a, "ciphertext too short");

    const ct_len = ct_with_tag.len - AEAD.tag_length;
    const ct = ct_with_tag[0..ct_len];
    var tag: [AEAD.tag_length]u8 = undefined;
    @memcpy(&tag, ct_with_tag[ct_len..]);
    var key_arr: [AEAD.key_length]u8 = undefined;
    @memcpy(&key_arr, key);
    var nonce_arr: [AEAD.nonce_length]u8 = undefined;
    @memcpy(&nonce_arr, nonce_in);

    const pt = try a.alloc(u8, ct_len);
    AEAD.decrypt(pt, ct, tag, ad, nonce_arr, key_arr) catch
        return errMsg(a, "DECRYPT_FAILED", "auth tag mismatch or corrupt");

    return okOne(a, "plaintext_b64", try encodeB64(a, pt));
}

fn doKeypair(a: std.mem.Allocator) ![]const u8 {
    const Ed = crypto.sign.Ed25519;
    const kp = Ed.KeyPair.generate(io());
    const pub_b = kp.public_key.toBytes();
    const sec_b = kp.secret_key.toBytes();
    return try std.fmt.allocPrint(a,
        "{{\"ok\":true,\"pub_b64\":\"{s}\",\"sec_b64\":\"{s}\"}}",
        .{ try encodeB64(a, &pub_b), try encodeB64(a, &sec_b) });
}

fn doSign(a: std.mem.Allocator, params: []const u8) ![]const u8 {
    var p = try parseObj(a, params);
    defer p.deinit();
    const sec = try decodeB64(a, getStr(p.value, "sec_b64") orelse "");
    const data = try decodeB64(a, getStr(p.value, "data_b64") orelse "");
    const Ed = crypto.sign.Ed25519;
    if (sec.len != Ed.SecretKey.encoded_length) return errInvalid(a, "sec_b64 must be 64 bytes");
    var sec_arr: [Ed.SecretKey.encoded_length]u8 = undefined;
    @memcpy(&sec_arr, sec);
    const sk = Ed.SecretKey.fromBytes(sec_arr) catch
        return errInvalid(a, "invalid secret key");
    const kp = Ed.KeyPair.fromSecretKey(sk) catch
        return errInvalid(a, "invalid secret key");
    const sig = kp.sign(data, null) catch
        return errMsg(a, "SIGN_FAILED", "sign failed");
    const sig_b = sig.toBytes();
    return okOne(a, "sig_b64", try encodeB64(a, &sig_b));
}

fn doVerify(a: std.mem.Allocator, params: []const u8) ![]const u8 {
    var p = try parseObj(a, params);
    defer p.deinit();
    const pub_b = try decodeB64(a, getStr(p.value, "pub_b64") orelse "");
    const data = try decodeB64(a, getStr(p.value, "data_b64") orelse "");
    const sig_b = try decodeB64(a, getStr(p.value, "sig_b64") orelse "");
    const Ed = crypto.sign.Ed25519;
    if (pub_b.len != Ed.PublicKey.encoded_length) return errInvalid(a, "pub_b64 must be 32 bytes");
    if (sig_b.len != Ed.Signature.encoded_length) return errInvalid(a, "sig_b64 must be 64 bytes");

    var pub_arr: [Ed.PublicKey.encoded_length]u8 = undefined;
    @memcpy(&pub_arr, pub_b);
    var sig_arr: [Ed.Signature.encoded_length]u8 = undefined;
    @memcpy(&sig_arr, sig_b);
    const pk = Ed.PublicKey.fromBytes(pub_arr) catch
        return std.fmt.allocPrint(a, "{{\"ok\":true,\"ok\":false}}", .{});
    const sig = Ed.Signature.fromBytes(sig_arr);
    sig.verify(data, pk) catch
        return std.fmt.allocPrint(a, "{{\"ok\":true,\"ok\":false}}", .{});
    return std.fmt.allocPrint(a, "{{\"ok\":true,\"ok\":true}}", .{});
}

fn doRandom(a: std.mem.Allocator, params: []const u8) ![]const u8 {
    var p = try parseObj(a, params);
    defer p.deinit();
    const n = getInt(p.value, "n", 0);
    if (n <= 0 or n > 1024 * 1024) return errInvalid(a, "n must be in (0, 1MB]");
    const buf = try a.alloc(u8, @intCast(n));
    csprng(buf);
    return okOne(a, "bytes_b64", try encodeB64(a, buf));
}
