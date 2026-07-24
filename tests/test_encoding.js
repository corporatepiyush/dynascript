/* test_encoding.js — dyna:encoding (hex / base32 / base64 / varint / base85
 * codecs, mirroring Go's encoding/hex, encoding/base32, encoding/base64,
 * encoding/binary, encoding/ascii85). RFC 4648 vectors are cross-checked
 * against Python's `base64` stdlib module (an independent oracle, including
 * its a85encode/a85decode for base85); varint vectors against a from-scratch
 * Python re-implementation of Go's LEB128/zigzag algorithm.
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_encoding.js
 * Prints "test_encoding: all tests passed" on success; throws on failure. */

import {
    hexEncode, hexDecode,
    base64Encode, base64Decode, base64UrlEncode, base64UrlDecode,
    base32Encode, base32Decode, base32HexEncode, base32HexDecode,
    putUvarint, uvarint, putVarint, varint,
    base85Encode, base85Decode,
} from "dyna:encoding";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function eqArr(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}
function assertThrows(fn, msg, ErrType) {
    n++;
    let threw = false, err = null;
    try { fn(); } catch (e) { threw = true; err = e; }
    if (!threw) throw new Error("assertion failed (expected throw): " + msg);
    if (ErrType && !(err instanceof ErrType))
        throw new Error("assertion failed (wrong error type, got " + err + "): " + msg);
}
function u8(...bytes) { return new Uint8Array(bytes); }
function bytesToStr(u8arr) {
    /* raw-byte "string" (each element becomes one code unit) for eqArr-style
     * comparisons against a JS string built the same way. */
    let s = "";
    for (const b of u8arr) s += String.fromCharCode(b);
    return s;
}

let seed = 0x2f6e2b1;
function rnd() { seed = (seed * 1103515245 + 12345) >>> 0; return seed; }
function randBytes(len) {
    const a = new Uint8Array(len);
    for (let i = 0; i < len; i++) a[i] = rnd() & 0xFF;
    return a;
}

/* Python-cross-checked RFC 4648 progression + a 37-byte random vector. */
const WORDS = ["", "f", "fo", "foo", "foob", "fooba", "foobar"];
const HEX_OF_WORD = ["", "66", "666f", "666f6f", "666f6f62", "666f6f6261", "666f6f626172"];
const B64_OF_WORD = ["", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"];
const B64URL_OF_WORD = ["", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy"];
const B32_OF_WORD = ["", "MY======", "MZXQ====", "MZXW6===", "MZXW6YQ=", "MZXW6YTB", "MZXW6YTBOI======"];
const B32HEX_OF_WORD = ["", "CO======", "CPNG====", "CPNMU===", "CPNMUOG=", "CPNMUOJ1", "CPNMUOJ1E8======"];
const B85_OF_WORD = ["", "Ac", "Ao@", "AoDS", "AoDTs", "AoDTs@/", "AoDTs@<)"];

const RANDOM37 = u8(57,12,140,125,114,71,52,44,216,16,15,47,111,119,13,101,214,
    112,229,142,3,81,216,174,142,79,110,172,52,47,194,49,183,176,135,22,235);
const RANDOM37_HEX = "390c8c7d7247342cd8100f2f6f770d65d670e58e0351d8ae8e4f6eac342fc231b7b08716eb";
const RANDOM37_B64 = "OQyMfXJHNCzYEA8vb3cNZdZw5Y4DUdiujk9urDQvwjG3sIcW6w==";
const RANDOM37_B64URL = "OQyMfXJHNCzYEA8vb3cNZdZw5Y4DUdiujk9urDQvwjG3sIcW6w";
const RANDOM37_B32 = "HEGIY7LSI42CZWAQB4XW65YNMXLHBZMOANI5RLUOJ5XKYNBPYIY3PMEHC3VQ====";
const RANDOM37_B32HEX = "7468OVBI8SQ2PM0G1SNMUTODCNB71PCE0D8THBKE9TNAOD1FO8ORFC472RLG====";

const RANDOM53 = u8(206,194,102,91,117,127,68,44,128,196,45,250,102,215,110,191,
    198,108,77,236,91,173,42,44,240,161,206,21,137,3,217,104,185,41,236,101,236,
    26,183,66,110,82,226,90,90,18,56,11,87,242,214,41,217);
const RANDOM53_B85 = "cGF0tFale1JAa&9B%Lq8`b\\B9>HKA+n=e,BM$)FE\\LHlVll*?\"DGoIt=qWe&=7Xokf`";

/* ============================== hex ============================== */
{
    assert(hexEncode("") === "", "hexEncode: empty string -> empty");
    assert(hexEncode(u8()) === "", "hexEncode: empty Uint8Array -> empty");
    assert(hexEncode("abc") === "616263", "hexEncode: 'abc' (utf8) -> '616263'");
    assert(hexEncode(u8(0x61, 0x62, 0x63)) === "616263", "hexEncode: Uint8Array [0x61,0x62,0x63]");
    assert(hexEncode(new ArrayBuffer(0)) === "", "hexEncode: empty ArrayBuffer -> empty");

    assert(hexDecode("") .length === 0, "hexDecode: '' -> empty Uint8Array");
    assert(eqArr(hexDecode("616263"), u8(0x61, 0x62, 0x63)), "hexDecode: '616263' -> [0x61,0x62,0x63]");
    assert(bytesToStr(hexDecode(hexEncode("abc"))) === "abc", "hex roundtrip: 'abc'");
    assert(eqArr(hexDecode("6162"), hexDecode("6162")), "hexDecode sanity self-eq");
    /* uppercase / mixed case both accepted (matches Go's hex.DecodeString) */
    assert(eqArr(hexDecode("DEAD"), u8(0xDE, 0xAD)), "hexDecode: uppercase hex digits");
    assert(eqArr(hexDecode("DeAd"), u8(0xDE, 0xAD)), "hexDecode: mixed-case hex digits");

    for (let i = 0; i < WORDS.length; i++) {
        assert(hexEncode(WORDS[i]) === HEX_OF_WORD[i], "hexEncode RFC word[" + i + "]");
        assert(bytesToStr(hexDecode(HEX_OF_WORD[i])) === WORDS[i], "hexDecode RFC word[" + i + "]");
    }
    assert(hexEncode(RANDOM37) === RANDOM37_HEX, "hexEncode: 37 random bytes vs python oracle");
    assert(eqArr(hexDecode(RANDOM37_HEX), RANDOM37), "hexDecode: 37 random bytes vs python oracle");

    /* error paths: odd length, invalid digit */
    assertThrows(() => hexDecode("abc"), "hexDecode: odd-length throws", SyntaxError);
    assertThrows(() => hexDecode("gg"), "hexDecode: invalid digit 'g' throws", SyntaxError);
    assertThrows(() => hexDecode("0g"), "hexDecode: invalid digit in 2nd nibble throws", SyntaxError);
    assertThrows(() => hexDecode("g0"), "hexDecode: invalid digit in 1st nibble throws", SyntaxError);

    /* type errors: neither string nor byte view */
    assertThrows(() => hexEncode(42), "hexEncode: Number input throws TypeError", TypeError);
    assertThrows(() => hexEncode({}), "hexEncode: plain object input throws TypeError", TypeError);
    assertThrows(() => hexEncode(null), "hexEncode: null input throws TypeError", TypeError);
    assertThrows(() => hexEncode([1, 2, 3]), "hexEncode: plain Array input throws TypeError", TypeError);
    /* a wide (non-byte) TypedArray view must be rejected, not silently reinterpreted */
    assertThrows(() => hexEncode(new Uint32Array([1, 2])), "hexEncode: Uint32Array rejected", TypeError);

    /* roundtrip fuzz across many lengths */
    for (let len = 0; len <= 64; len++) {
        const b = randBytes(len);
        assert(eqArr(hexDecode(hexEncode(b)), b), "hex roundtrip fuzz len=" + len);
    }
}

/* ============================== base64 (standard) ============================== */
{
    assert(base64Encode("") === "", "base64Encode: empty -> empty");
    assert(base64Decode("").length === 0, "base64Decode: empty -> empty");

    for (let i = 0; i < WORDS.length; i++) {
        assert(base64Encode(WORDS[i]) === B64_OF_WORD[i], "base64Encode RFC word[" + i + "]");
        assert(bytesToStr(base64Decode(B64_OF_WORD[i])) === WORDS[i], "base64Decode RFC word[" + i + "]");
    }
    assert(base64Encode(RANDOM37) === RANDOM37_B64, "base64Encode: 37 random bytes vs python oracle");
    assert(eqArr(base64Decode(RANDOM37_B64), RANDOM37), "base64Decode: 37 random bytes vs python oracle");

    /* Uint8Array/ArrayBuffer input accepted directly (not just strings) */
    assert(base64Encode(u8(0x66, 0x6f, 0x6f)) === "Zm9v", "base64Encode: Uint8Array input");
    assert(base64Encode(u8(0x66, 0x6f, 0x6f).buffer) === "Zm9v", "base64Encode: ArrayBuffer input");

    /* error paths */
    assertThrows(() => base64Decode("Zm9v!"), "base64Decode: bad length throws", SyntaxError);
    assertThrows(() => base64Decode("!!!!"), "base64Decode: invalid chars throws", SyntaxError);
    assertThrows(() => base64Decode("Z==="), "base64Decode: misplaced padding throws", SyntaxError);

    for (let len = 0; len <= 64; len++) {
        const b = randBytes(len);
        assert(eqArr(base64Decode(base64Encode(b)), b), "base64 roundtrip fuzz len=" + len);
    }
}

/* ============================== base64 (url-safe) ============================== */
{
    assert(base64UrlEncode("") === "", "base64UrlEncode: empty -> empty");
    assert(base64UrlDecode("").length === 0, "base64UrlDecode: empty -> empty");

    for (let i = 0; i < WORDS.length; i++) {
        assert(base64UrlEncode(WORDS[i]) === B64URL_OF_WORD[i], "base64UrlEncode RFC word[" + i + "]");
        assert(bytesToStr(base64UrlDecode(B64URL_OF_WORD[i])) === WORDS[i], "base64UrlDecode RFC word[" + i + "]");
        /* no '=' padding ever emitted */
        assert(!base64UrlEncode(WORDS[i]).includes("="), "base64UrlEncode never pads: word[" + i + "]");
    }
    assert(base64UrlEncode(RANDOM37) === RANDOM37_B64URL, "base64UrlEncode: 37 random bytes vs python oracle");
    assert(eqArr(base64UrlDecode(RANDOM37_B64URL), RANDOM37), "base64UrlDecode: 37 random bytes vs python oracle");

    /* url-safe vs standard differ exactly on alphabet positions 62/63 */
    assert(base64Encode(u8(0xF8)) === "+A==", "base64Encode: byte 0xF8 hits alphabet[62] '+'");
    assert(base64UrlEncode(u8(0xF8)) === "-A", "base64UrlEncode: byte 0xF8 hits alphabet[62] '-', unpadded");
    assert(base64Encode(u8(0xFC)) === "/A==", "base64Encode: byte 0xFC hits alphabet[63] '/'");
    assert(base64UrlEncode(u8(0xFC)) === "_A", "base64UrlEncode: byte 0xFC hits alphabet[63] '_', unpadded");
    assert(eqArr(base64UrlDecode("-A"), u8(0xF8)), "base64UrlDecode: '-A' -> [0xF8]");
    assert(eqArr(base64UrlDecode("_A"), u8(0xFC)), "base64UrlDecode: '_A' -> [0xFC]");

    /* url-safe decode must reject the standard-alphabet '+'/'/' characters */
    assertThrows(() => base64UrlDecode("+A"), "base64UrlDecode: rejects '+' (not url-safe alphabet)", SyntaxError);
    assertThrows(() => base64UrlDecode("/A"), "base64UrlDecode: rejects '/' (not url-safe alphabet)", SyntaxError);
    /* url-safe decode also accepts an explicitly-padded string (lenient on input) */
    assert(eqArr(base64UrlDecode("Zg=="), u8(0x66)), "base64UrlDecode: tolerates explicit '=' padding");

    /* no-padding url roundtrip across many lengths, including the ones whose
     * standard encoding needs 1 or 2 '=' pad characters */
    for (let len = 0; len <= 64; len++) {
        const b = randBytes(len);
        const enc = base64UrlEncode(b);
        assert(!enc.includes("="), "base64UrlEncode never pads (len=" + len + ")");
        assert(eqArr(base64UrlDecode(enc), b), "base64url roundtrip fuzz len=" + len);
    }
}

/* ============================== base32 (standard + hex) ============================== */
{
    assert(base32Encode("") === "", "base32Encode: empty -> empty");
    assert(base32Decode("").length === 0, "base32Decode: empty -> empty");
    assert(base32HexEncode("") === "", "base32HexEncode: empty -> empty");
    assert(base32HexDecode("").length === 0, "base32HexDecode: empty -> empty");

    for (let i = 0; i < WORDS.length; i++) {
        assert(base32Encode(WORDS[i]) === B32_OF_WORD[i], "base32Encode RFC word[" + i + "]");
        assert(bytesToStr(base32Decode(B32_OF_WORD[i])) === WORDS[i], "base32Decode RFC word[" + i + "]");
        assert(base32HexEncode(WORDS[i]) === B32HEX_OF_WORD[i], "base32HexEncode RFC word[" + i + "]");
        assert(bytesToStr(base32HexDecode(B32HEX_OF_WORD[i])) === WORDS[i], "base32HexDecode RFC word[" + i + "]");
    }
    /* the exact vectors called out by the task spec */
    assert(base32Encode("f") === "MY======", "base32Encode('f') RFC vector");
    assert(base32Encode("fo") === "MZXQ====", "base32Encode('fo') RFC vector");
    assert(base32Encode("foobar") === "MZXW6YTBOI======", "base32Encode('foobar') RFC vector");

    assert(base32Encode(RANDOM37) === RANDOM37_B32, "base32Encode: 37 random bytes vs python oracle");
    assert(eqArr(base32Decode(RANDOM37_B32), RANDOM37), "base32Decode: 37 random bytes vs python oracle");
    assert(base32HexEncode(RANDOM37) === RANDOM37_B32HEX, "base32HexEncode: 37 random bytes vs python oracle");
    assert(eqArr(base32HexDecode(RANDOM37_B32HEX), RANDOM37), "base32HexDecode: 37 random bytes vs python oracle");

    /* case sensitivity: lowercase is not part of either RFC alphabet */
    assertThrows(() => base32Decode("my======"), "base32Decode: lowercase rejected", SyntaxError);
    /* wrong alphabet: digit '0' isn't in the standard alphabet (A-Z then 2-7) */
    assertThrows(() => base32Decode("0Y======"), "base32Decode: digit '0' invalid in standard alphabet", SyntaxError);
    /* wrong alphabet the other way: letter 'W' isn't in the hex alphabet (0-9 then A-V) */
    assertThrows(() => base32HexDecode("WO======"), "base32HexDecode: letter 'W' invalid in hex alphabet", SyntaxError);
    /* length must be a multiple of 8 */
    assertThrows(() => base32Decode("MY====="), "base32Decode: length not a multiple of 8", SyntaxError);
    /* padding only valid in the final block: "foobar"'s 16-char encoding
     * (first block "MZXW6YTB" has none, second "OI======" does) already
     * proves the valid shape decodes above; here the padding is (invalidly)
     * in the first of two blocks instead of the last. */
    assertThrows(() => base32Decode("MY======MZXW6YTB"), "base32Decode: padding before the final block rejected", SyntaxError);

    for (let len = 0; len <= 64; len++) {
        const b = randBytes(len);
        assert(eqArr(base32Decode(base32Encode(b)), b), "base32 roundtrip fuzz len=" + len);
        assert(eqArr(base32HexDecode(base32HexEncode(b)), b), "base32hex roundtrip fuzz len=" + len);
    }
}

/* ============================== varint (LEB128 unsigned) ============================== */
{
    /* known vectors from the task spec + an independent Python re-implementation */
    assert(eqArr(putUvarint(0), u8(0)), "putUvarint(0) -> [0]");
    assert(eqArr(putUvarint(300), u8(0xAC, 0x02)), "putUvarint(300) -> [0xAC, 0x02]");
    assert(eqArr(putUvarint(1), u8(1)), "putUvarint(1) -> [1]");
    assert(eqArr(putUvarint(127), u8(127)), "putUvarint(127) -> [127] (1-byte boundary)");
    assert(eqArr(putUvarint(128), u8(128, 1)), "putUvarint(128) -> [0x80, 0x01] (2-byte boundary)");
    assert(eqArr(putUvarint(16384), u8(128, 128, 1)), "putUvarint(16384) -> 3-byte boundary");
    assert(eqArr(putUvarint(4294967296), u8(128, 128, 128, 128, 16)), "putUvarint(2^32) -> 5 bytes");
    assert(eqArr(putUvarint(Number.MAX_SAFE_INTEGER),
        u8(255, 255, 255, 255, 255, 255, 255, 15)), "putUvarint(2^53-1) -> 8 bytes");

    {
        let [v, nb] = uvarint(u8(0));
        assert(v === 0 && nb === 1, "uvarint([0]) -> [0, 1]");
    }
    {
        let [v, nb] = uvarint(u8(0xAC, 0x02));
        assert(v === 300 && nb === 2, "uvarint([0xAC,0x02]) -> [300, 2]");
    }
    {
        let [v, nb] = uvarint(u8(0xAC, 0x02, 0xFF, 0xFF));
        assert(v === 300 && nb === 2, "uvarint stops after its own varint, ignoring trailing bytes");
    }

    /* BigInt beyond 2^53: putUvarint(BigInt) and uvarint(...) returning BigInt */
    assert(eqArr(putUvarint(9007199254740992n),
        u8(128, 128, 128, 128, 128, 128, 128, 16)), "putUvarint(2^53 as BigInt) -> 8 bytes");
    assert(eqArr(putUvarint(18446744073709551615n),
        u8(255, 255, 255, 255, 255, 255, 255, 255, 255, 1)), "putUvarint(2^64-1 BigInt) -> 10 bytes (max)");
    {
        let [v, nb] = uvarint(u8(255, 255, 255, 255, 255, 255, 255, 255, 255, 1));
        assert(typeof v === "bigint", "uvarint: value beyond 2^53 comes back as BigInt");
        assert(v === 18446744073709551615n && nb === 10, "uvarint: 2^64-1 roundtrip via BigInt");
    }
    {
        /* exactly at the boundary: 2^53-1 must come back as a Number, not BigInt */
        let [v, nb] = uvarint(u8(255, 255, 255, 255, 255, 255, 255, 15));
        assert(typeof v === "number", "uvarint: MAX_SAFE_INTEGER still comes back as Number");
        assert(v === Number.MAX_SAFE_INTEGER && nb === 8, "uvarint: 2^53-1 exact roundtrip");
    }

    /* putUvarint input validation */
    assertThrows(() => putUvarint(-1), "putUvarint: negative Number rejected", RangeError);
    assertThrows(() => putUvarint(1.5), "putUvarint: non-integer Number rejected", RangeError);
    assertThrows(() => putUvarint(Number.MAX_SAFE_INTEGER + 1),
        "putUvarint: Number beyond 2^53-1 rejected (must use BigInt)", RangeError);

    /* truncated input: buffer ran out before a terminating (high-bit-clear) byte */
    {
        let [v, nb] = uvarint(u8());
        assert(v === 0 && nb === 0, "uvarint([]) -> [0, 0] (truncated: empty)");
    }
    {
        let [v, nb] = uvarint(u8(0x80));
        assert(v === 0 && nb === 0, "uvarint([0x80]) -> [0, 0] (truncated: no terminator)");
    }
    {
        let [v, nb] = uvarint(u8(0x80, 0x80, 0x80));
        assert(v === 0 && nb === 0, "uvarint([0x80,0x80,0x80]) -> [0, 0] (truncated)");
    }
    /* overflow: value would need more than 64 bits -> negative bytesRead, Go-style */
    {
        const overflow11 = new Uint8Array(11).fill(0x80);
        let [v, nb] = uvarint(overflow11);
        assert(nb === -11, "uvarint: 11 continuation bytes overflow -> n = -11");
    }
    {
        /* 9 bytes of 0xFF (max continuation payload) + a final byte of 2 (only
         * 1 is a legal high bit at byte 9) -> overflow at byte index 9 */
        const overflow10 = u8(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02);
        let [v, nb] = uvarint(overflow10);
        assert(nb === -10, "uvarint: final byte > 1 at the 10-byte boundary overflows -> n = -10");
    }

    /* general roundtrip fuzz, spanning every byte-length class (1..10 bytes) */
    const uvarintCases = [0, 1, 2, 127, 128, 129, 16383, 16384, 300, 65535, 65536,
        2097151, 2097152, 268435455, 268435456, 4294967296, 34359738368,
        Number.MAX_SAFE_INTEGER, Number.MAX_SAFE_INTEGER - 1];
    for (const x of uvarintCases) {
        const enc = putUvarint(x);
        const [v, nb] = uvarint(enc);
        assert(v === x && nb === enc.length, "uvarint roundtrip: " + x);
    }
    /* BigInt roundtrip across the full 64-bit range */
    const bigCases = [0n, 1n, 300n, 9007199254740991n, 9007199254740992n,
        (1n << 63n), (1n << 64n) - 1n];
    for (const x of bigCases) {
        const enc = putUvarint(x);
        const [v, nb] = uvarint(enc);
        const vBig = typeof v === "bigint" ? v : BigInt(v);
        assert(vBig === x && nb === enc.length, "uvarint BigInt roundtrip: " + x);
    }
}

/* ============================== varint (zigzag signed) ============================== */
{
    /* known vectors from the task spec */
    assert(eqArr(putVarint(-1), u8(1)), "putVarint(-1) -> zigzag 1 -> [1]");
    assert(eqArr(putVarint(1), u8(2)), "putVarint(1) -> zigzag 2 -> [2]");
    assert(eqArr(putVarint(0), u8(0)), "putVarint(0) -> [0]");
    assert(eqArr(putVarint(-2), u8(3)), "putVarint(-2) -> zigzag 3 -> [3]");
    assert(eqArr(putVarint(2), u8(4)), "putVarint(2) -> zigzag 4 -> [4]");

    {
        let [v, nb] = varint(u8(1));
        assert(v === -1 && nb === 1, "varint([1]) -> [-1, 1]");
    }
    {
        let [v, nb] = varint(u8(2));
        assert(v === 1 && nb === 1, "varint([2]) -> [1, 1]");
    }
    {
        let [v, nb] = varint(u8(0));
        assert(v === 0 && nb === 1, "varint([0]) -> [0, 1]");
    }

    /* putVarint input validation (Number path) */
    assertThrows(() => putVarint(1.5), "putVarint: non-integer Number rejected", RangeError);
    assertThrows(() => putVarint(Number.MAX_SAFE_INTEGER + 1),
        "putVarint: Number beyond safe-integer range rejected", RangeError);
    assertThrows(() => putVarint(-(Number.MAX_SAFE_INTEGER) - 1),
        "putVarint: Number beyond -safe-integer range rejected", RangeError);

    /* BigInt beyond the safe-integer range, both signs */
    assert(eqArr(putVarint(-9007199254740992n),
        u8(255, 255, 255, 255, 255, 255, 255, 31)), "putVarint(-2^53 BigInt)");
    {
        let [v, nb] = varint(u8(255, 255, 255, 255, 255, 255, 255, 31));
        assert(typeof v === "bigint", "varint: value beyond safe range comes back as BigInt");
        assert(v === -9007199254740992n && nb === 8, "varint: -2^53 roundtrip via BigInt");
    }
    {
        /* exactly at the boundary: -(2^53-1) still comes back as a Number */
        const enc = putVarint(-(Number.MAX_SAFE_INTEGER));
        const [v, nb] = varint(enc);
        assert(typeof v === "number", "varint: -MIN_SAFE_INTEGER-ish still comes back as Number");
        assert(v === -(Number.MAX_SAFE_INTEGER), "varint: -(2^53-1) exact roundtrip");
    }

    /* truncated / overflow, same Go-style convention as uvarint */
    {
        let [v, nb] = varint(u8());
        assert(v === 0 && nb === 0, "varint([]) -> [0, 0] (truncated)");
    }
    {
        let [v, nb] = varint(u8(0x80, 0x80));
        assert(v === 0 && nb === 0, "varint([0x80,0x80]) -> [0, 0] (truncated)");
    }
    {
        const overflow11 = new Uint8Array(11).fill(0x80);
        let [v, nb] = varint(overflow11);
        assert(nb === -11, "varint: overflow propagates Uvarint's negative n");
    }

    /* general roundtrip fuzz, both signs */
    const varintCases = [0, 1, -1, 2, -2, 63, -64, 64, -65, 1000000, -1000000,
        Number.MAX_SAFE_INTEGER, -Number.MAX_SAFE_INTEGER];
    for (const x of varintCases) {
        const enc = putVarint(x);
        const [v, nb] = varint(enc);
        assert(v === x && nb === enc.length, "varint roundtrip: " + x);
    }
    const bigSignedCases = [0n, -1n, 1n, -9007199254740992n, 9007199254740992n,
        -(1n << 62n), (1n << 62n)];
    for (const x of bigSignedCases) {
        const enc = putVarint(x);
        const [v, nb] = varint(enc);
        const vBig = typeof v === "bigint" ? v : BigInt(v);
        assert(vBig === x && nb === enc.length, "varint BigInt roundtrip: " + x);
    }
}

/* ============================== base85 (ascii85, optional bonus) ============================== */
{
    assert(base85Encode("") === "", "base85Encode: empty -> empty");
    assert(base85Decode("").length === 0, "base85Decode: empty -> empty");

    for (let i = 0; i < WORDS.length; i++) {
        assert(base85Encode(WORDS[i]) === B85_OF_WORD[i], "base85Encode word[" + i + "]");
        assert(bytesToStr(base85Decode(B85_OF_WORD[i])) === WORDS[i], "base85Decode word[" + i + "]");
    }
    assert(base85Encode(RANDOM53) === RANDOM53_B85, "base85Encode: 53 random bytes vs python oracle");
    assert(eqArr(base85Decode(RANDOM53_B85), RANDOM53), "base85Decode: 53 random bytes vs python oracle");

    /* the 'z' shorthand for an all-zero 4-byte group */
    assert(base85Encode(u8(0, 0, 0, 0)) === "z", "base85Encode: all-zero 4-byte group -> 'z'");
    assert(base85Encode(u8(0, 0, 0, 0, 0, 0, 0, 0)) === "zz", "base85Encode: two all-zero groups -> 'zz'");
    assert(eqArr(base85Decode("z"), u8(0, 0, 0, 0)), "base85Decode: 'z' -> 4 zero bytes");
    assert(eqArr(base85Decode("zz"), u8(0, 0, 0, 0, 0, 0, 0, 0)), "base85Decode: 'zz' -> 8 zero bytes");
    /* 'z' never appears for a PARTIAL trailing all-zero group (< 4 bytes) */
    assert(base85Encode(u8(0, 0, 0)) !== "z", "base85Encode: partial all-zero group does not use 'z'");
    assert(eqArr(base85Decode(base85Encode(u8(0, 0, 0))), u8(0, 0, 0)), "base85 roundtrip: partial all-zero group");

    /* whitespace is skipped on decode (PostScript/PDF line-wrapped ascii85) */
    assert(eqArr(base85Decode("Ao DS"), u8(0x66, 0x6f, 0x6f)), "base85Decode: embedded space skipped");
    assert(eqArr(base85Decode("Ao\nDS\t"), u8(0x66, 0x6f, 0x6f)), "base85Decode: embedded newline/tab skipped");

    /* error paths */
    assertThrows(() => base85Decode("v"), "base85Decode: byte above 'u' is invalid", SyntaxError);
    assertThrows(() => base85Decode(String.fromCharCode(31)),
        "base85Decode: byte below '!' (and not whitespace) is invalid", SyntaxError);
    /* 'z' is also simply outside the '!'-'u' digit range whenever it isn't
     * recognized as the group-boundary shorthand, so this both is off a
     * group boundary AND would be out-of-range as a literal digit -- either
     * way, decode must reject it. */
    assertThrows(() => base85Decode("Ao@Sz"), "base85Decode: 'z' off a group boundary is invalid", SyntaxError);
    /* exactly 1 leftover char at the end can never decode (2/3/4 are valid, 1 is not) */
    assertThrows(() => base85Decode("A"), "base85Decode: a single leftover char is impossible", SyntaxError);
    assertThrows(() => base85Decode("AoDTsA"), "base85Decode: 1 leftover char after full groups is impossible", SyntaxError);

    for (let len = 0; len <= 64; len++) {
        const b = randBytes(len);
        assert(eqArr(base85Decode(base85Encode(b)), b), "base85 roundtrip fuzz len=" + len);
    }
    /* an all-zero buffer at every length, to hammer the 'z'-shorthand boundary
     * (full 4-byte groups use 'z'; the trailing partial group, if any, does not) */
    for (let len = 0; len <= 20; len++) {
        const b = new Uint8Array(len);
        assert(eqArr(base85Decode(base85Encode(b)), b), "base85 all-zero roundtrip len=" + len);
    }
}

/* ============================== coercion / valueOf sanity ============================== */
{
    /* putUvarint/putVarint run ToNumber-ish coercion (JS_ToFloat64), which may
     * invoke valueOf -- verify it still produces the right answer (there is
     * no closable native resource in this module to attack, unlike
     * dyna:bytes, so this is a plain functional check, not a UAF probe). */
    assert(eqArr(putUvarint({ valueOf() { return 300; } }), u8(0xAC, 0x02)),
        "putUvarint: accepts a valueOf-coercible object");
    assert(eqArr(putVarint({ valueOf() { return -1; } }), u8(1)),
        "putVarint: accepts a valueOf-coercible object");
    /* a String OBJECT wrapper is deliberately NOT accepted: JS_IsString is an
     * exact-tag check (no implicit unboxing), so this must throw the same
     * TypeError as any other non-string, non-byte-view object. */
    assertThrows(() => hexEncode(new String("abc")),
        "hexEncode: String wrapper object (boxed) is rejected, not silently unboxed", TypeError);
}

print("test_encoding: all tests passed (" + n + " assertions)");
