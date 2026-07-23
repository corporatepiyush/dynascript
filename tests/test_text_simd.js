/* test_text_simd.js — differential tests for the dynajs:text SIMD kernels
 * (count_u8, find_first_of, validate_utf8, base64). Oracles are independent JS
 * reimplementations + known vectors; lengths sweep the 16/32B block boundaries. */
import { count, indexOfAny, isValidUtf8, base64Encode, base64Decode }
    from "dynajs:text";

function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }

let seed = 0x12345678 >>> 0;
function rnd() { seed = (seed * 1664525 + 1013904223) >>> 0; return seed; }
const AB = "abcd";
function randAscii(len) {
    let s = "";
    for (let i = 0; i < len; i++) s += AB[rnd() % AB.length];
    return s;
}

/* ---- count_u8 vs JS oracle ---- */
let cases = 0;
for (let len = 0; len <= 80; len++) {
    for (let rep = 0; rep < 20; rep++) {
        const s = randAscii(len);
        const ch = AB[rnd() % AB.length];
        let ref = 0;
        for (let i = 0; i < s.length; i++) if (s[i] === ch) ref++;
        assert(count(s, ch) === ref, "count mismatch len=" + len);
        cases++;
    }
}

/* ---- find_first_of vs JS oracle (set sizes 1..8 exercise the SIMD path) ---- */
for (let len = 0; len <= 80; len++) {
    for (let sl = 1; sl <= 8; sl++) {
        for (let rep = 0; rep < 8; rep++) {
            const s = randAscii(len);
            let set = "";
            for (let k = 0; k < sl; k++) set += "cdefghij"[rnd() % 8];
            let ref = -1;
            for (let i = 0; i < s.length; i++)
                if (set.indexOf(s[i]) !== -1) { ref = i; break; }
            assert(indexOfAny(s, set) === ref,
                   "indexOfAny mismatch len=" + len + " set=" + set);
            cases++;
        }
    }
}
print("test_text_simd: " + cases + " count/find_first_of cases OK");

/* ---- validate_utf8 ---- */
function utf8Bytes(cp) {
    if (cp < 0x80) return [cp];
    if (cp < 0x800) return [0xC0 | (cp >> 6), 0x80 | (cp & 0x3F)];
    if (cp < 0x10000)
        return [0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F)];
    return [0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3F),
            0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F)];
}
function buf(arr) { return new Uint8Array(arr).buffer; }

/* random well-formed UTF-8 (skip surrogates) must validate true */
for (let rep = 0; rep < 3000; rep++) {
    const bytes = [];
    const ncp = rnd() % 40;
    for (let i = 0; i < ncp; i++) {
        let cp;
        do { cp = rnd() % 0x110000; } while (cp >= 0xD800 && cp <= 0xDFFF);
        bytes.push(...utf8Bytes(cp));
    }
    assert(isValidUtf8(buf(bytes)) === true, "valid utf8 rejected @rep" + rep);
}
/* known-invalid sequences must validate false */
const invalid = [
    [0x80],                         // stray continuation
    [0xC0, 0x80],                   // overlong /
    [0xE0, 0x80, 0x80],             // overlong
    [0xF0, 0x80, 0x80, 0x80],       // overlong
    [0xED, 0xA0, 0x80],             // surrogate U+D800
    [0xC2],                         // truncated 2-byte
    [0xE2, 0x82],                   // truncated 3-byte
    [0xF4, 0x90, 0x80, 0x80],       // > U+10FFFF
    [0x41, 0xFF, 0x42],             // 0xFF invalid byte
    [0xC2, 0x41],                   // bad continuation
];
for (const seq of invalid)
    assert(isValidUtf8(buf(seq)) === false,
           "invalid utf8 accepted: " + seq);
/* ASCII-with-one-bad-byte at various offsets (exercises the SIMD ASCII skip) */
for (let off = 0; off < 40; off++) {
    const b = [];
    for (let i = 0; i < 40; i++) b.push(i === off ? 0xFF : 0x61);
    assert(isValidUtf8(buf(b)) === false, "bad byte @" + off + " accepted");
}
print("test_text_simd: utf8 validation OK");

/* ---- base64 ---- */
const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
function b64ref(bytes) {
    let o = "";
    for (let i = 0; i < bytes.length; i += 3) {
        const b0 = bytes[i], b1 = bytes[i + 1], b2 = bytes[i + 2];
        const rem = bytes.length - i;
        const x = (b0 << 16) | ((rem > 1 ? b1 : 0) << 8) | (rem > 2 ? b2 : 0);
        o += B64[(x >> 18) & 63] + B64[(x >> 12) & 63];
        o += rem > 1 ? B64[(x >> 6) & 63] : "=";
        o += rem > 2 ? B64[x & 63] : "=";
    }
    return o;
}
/* known vectors */
assert(base64Encode("") === "", "b64 empty");
assert(base64Encode("f") === "Zg==", "b64 f");
assert(base64Encode("fo") === "Zm8=", "b64 fo");
assert(base64Encode("foo") === "Zm9v", "b64 foo");
assert(base64Encode("foobar") === "Zm9vYmFy", "b64 foobar");
/* random roundtrip + vs oracle, lengths across block boundaries */
for (let len = 0; len <= 100; len++) {
    const bytes = [];
    for (let i = 0; i < len; i++) bytes.push(rnd() & 0xFF);
    const ab = new Uint8Array(bytes).buffer;
    const enc = base64Encode(ab);
    assert(enc === b64ref(bytes), "b64 encode mismatch len=" + len);
    const dec = new Uint8Array(base64Decode(enc));
    assert(dec.length === len, "b64 decode length len=" + len);
    for (let i = 0; i < len; i++)
        assert(dec[i] === bytes[i], "b64 roundtrip byte " + i + " len=" + len);
}
/* invalid inputs throw */
for (const bad of ["A", "AB", "ABC", "A===", "====", "ab*d", "abcde"]) {
    let threw = false;
    try { base64Decode(bad); } catch (e) { threw = true; }
    assert(threw, "base64Decode should reject " + JSON.stringify(bad));
}
print("test_text_simd: base64 OK");
print("test_text_simd: all tests passed");
