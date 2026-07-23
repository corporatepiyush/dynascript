/* test_path.js -- dynajs:path (in-repo POSIX filesystem-path string utilities).
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_path.js
 * Prints "test_path: all tests passed" on success; throws on failure.
 *
 * Expected values throughout are cross-checked against Node 26's real
 * `path.posix` (join/normalize/dirname/basename/extname/isAbsolute are
 * cwd-independent and match Node directly; resolve()/relative() are checked
 * against Node's `path.posix.resolve("/", ...)`, which is exactly equivalent
 * to this module's notional "cwd is always /" scheme -- see dynajs-path.c). */

import { join, resolve, normalize, clean, dirname, basename, extname,
         isAbsolute, relative, sep, delimiter } from "dynajs:path";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function throwsTypeError(fn, msg) {
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    assert(caught instanceof TypeError, msg + " (got " + caught + ")");
}

/* --- sep / delimiter: plain string properties, not functions --- */
{
    assert(sep === "/", "sep is '/'");
    assert(delimiter === ":", "delimiter is ':'");
    assert(typeof sep === "string" && typeof delimiter === "string",
        "sep/delimiter are strings");
}

/* --- normalize(p) / clean(p): identical implementation, two names --- */
{
    const cases = [
        ["", "."],
        ["/", "/"],
        ["//", "/"],
        ["///", "/"],
        [".", "."],
        ["..", ".."],
        ["../", "../"],
        ["./", "./"],
        ["a/b/../c", "a/c"],
        ["a/./b", "a/b"],
        ["a/b/", "a/b/"],           // trailing slash preserved (Node rule)
        ["a/b//", "a/b/"],
        ["..//..", "../.."],
        ["/a/b/c/../../..", "/"],   // ".." absorbed once absolute root reached
        ["a//b//c", "a/b/c"],
        ["a/b/c", "a/b/c"],
        ["/a/b/c", "/a/b/c"],
        ["/../", "/"],
        ["/..", "/"],
        ["../..", "../.."],
        ["../../", "../../"],
        ["foo/../../bar", "../bar"],
        ["./foo", "foo"],
        ["././foo", "foo"],
        ["foo/./", "foo/"],
        ["foo/.", "foo"],
        ["/foo/", "/foo/"],
        ["//foo//bar//", "/foo/bar/"],
        ["a/../..", ".."],
        ["/a/../..", "/"],
        [".././../a", "../../a"],
        ["a/b/c/../../../../d", "../d"],
        ["/./", "/"],
        ["/.", "/"],
        ["....", "...."],           // 4 dots is an ordinary name, not ".."
        ["..a", "..a"],
        ["a..", "a.."],
        ["a/../b/../c", "c"],
        ["/a//b///c////d", "/a/b/c/d"],
        ["..bashrc", "..bashrc"],
        ["a/b/c/d/../../../../../e", "../e"], // more ".." than segments
    ];
    for (const [input, want] of cases) {
        assert(normalize(input) === want,
            "normalize(" + JSON.stringify(input) + ") === " + JSON.stringify(want) +
            " (got " + JSON.stringify(normalize(input)) + ")");
        assert(clean(input) === normalize(input),
            "clean() is an alias of normalize() for " + JSON.stringify(input));
    }
}

/* --- join(...parts): empty parts skipped, then normalize()d --- */
{
    assert(join() === ".", "join() with no args -> '.'");
    assert(join("") === ".", "join('') -> '.'");
    assert(join("", "", "") === ".", "join of only empty parts -> '.'");
    assert(join("a", "b", "c") === "a/b/c", "join('a','b','c')");
    assert(join("/a", "b", "c") === "/a/b/c", "join with absolute first part");
    assert(join("a", "", "b") === "a/b", "join skips an empty middle part");
    assert(join("a/", "/b") === "a/b", "join collapses the doubled slash at the seam");
    assert(join("a", "../b") === "b", "join normalizes '..' across parts");
    assert(join("/", "a") === "/a", "join('/','a')");
    assert(join("/", "../a") === "/a", "join('/','../a') absorbs '..' at the root");
    assert(join(".", "x") === "x", "join('.','x') drops the leading '.'");
    assert(join("a", ".") === "a", "join('a','.')");
    assert(join("a", "..") === ".", "join('a','..') cancels out to '.'");
    assert(join("foo", "bar", "baz/asdf", "quux", "..") === "foo/bar/baz/asdf",
        "join with a trailing '..' popping the last part");
    assert(join("a", "b/../../c") === "c", "join('a','b/../../c')");
    assert(join("", "a", "") === "a", "join skips leading/trailing empty parts");
    assert(join("/a/", "/b/") === "/a/b/", "join preserves a real trailing slash");
    assert(join("a", "b", "..", "../c") === "c", "join('a','b','..','../c')");
    assert(join("///", "///") === "/", "join of only-slash parts collapses to '/'");
    assert(join("a", "b", "c", "d", "e", "f", "g") === "a/b/c/d/e/f/g",
        "join with more parts than the declared arity (variadic all the way)");
}

/* --- resolve(...parts): right-to-left, notional cwd "/", always absolute --- */
{
    assert(resolve() === "/", "resolve() with no args -> notional cwd root");
    assert(resolve("") === "/", "resolve('') -> notional cwd root");
    assert(resolve("a", "b") === "/a/b", "resolve('a','b') against notional root");
    assert(resolve("/a", "../../..") === "/", "excess '..' absorbed at the root");
    assert(resolve("a/b", "../../../c") === "/c",
        "resolve('a/b','../../../c') absorbs the extra '..' at the root");
    assert(resolve("/foo/bar", "./baz") === "/foo/bar/baz", "resolve with a './' part");
    assert(resolve("/foo/bar", "/baz") === "/baz",
        "a later absolute part overrides everything before it");
    assert(resolve("foo", "/baz") === "/baz", "same, from a relative first part");
    assert(resolve("/a/b", "../../..", "c") === "/c",
        "resolve('/a/b','../../..','c')");
    assert(resolve("a/b/c", "../../../../x") === "/x",
        "resolve('a/b/c','../../../../x')");
    assert(resolve("/a", "b", "c", "d", "e") === "/a/b/c/d/e",
        "resolve with more parts than the declared arity");
    /* property: resolve() is ALWAYS absolute, for any input */
    for (const args of [[], [""], ["a"], ["a", "b"], ["../../.."], ["/x"], ["x", "/y", "z"]]) {
        const r = resolve(...args);
        assert(r.length > 0 && r[0] === "/",
            "resolve(...)" + JSON.stringify(args) + " is always absolute, got " + JSON.stringify(r));
    }
}

/* --- dirname(p): purely lexical, no "."/".." resolution --- */
{
    const cases = [
        ["", "."],
        ["/", "/"],
        ["//", "/"],
        ["///", "/"],
        [".", "."],
        ["..", "."],
        ["a", "."],
        ["a/b", "a"],
        ["/a", "/"],
        ["/a/b", "/a"],
        ["/a/b/", "/a"],
        ["a/b/", "a"],
        ["a/", "."],
        ["/a/b/c", "/a/b"],
        ["//a", "//"],              // POSIX: exactly 2 leading slashes are kept
        ["/a//b", "/a/"],
        ["a//b", "a/"],
        ["../..", ".."],
        ["/..", "/"],
        ["a/..", "a"],              // lexical: dirname does not resolve ".."
        ["foo", "."],
        ["/foo", "/"],
        ["/foo/bar", "/foo"],
        ["/foo/bar/", "/foo"],
        ["foo/bar/baz", "foo/bar"],
        ["...", "."],
    ];
    for (const [input, want] of cases) {
        assert(dirname(input) === want,
            "dirname(" + JSON.stringify(input) + ") === " + JSON.stringify(want) +
            " (got " + JSON.stringify(dirname(input)) + ")");
    }
}

/* --- basename(p): no ext argument --- */
{
    const cases = [
        ["", ""],
        ["/", ""],
        ["//", ""],
        ["///", ""],
        ["a", "a"],
        ["/a", "a"],
        ["/a/b", "b"],
        ["a/b/", "b"],
        ["a/b//", "b"],
        ["/a/b/c.txt", "c.txt"],
        [".", "."],
        ["..", ".."],
        ["a/", "a"],
    ];
    for (const [input, want] of cases) {
        assert(basename(input) === want,
            "basename(" + JSON.stringify(input) + ") === " + JSON.stringify(want) +
            " (got " + JSON.stringify(basename(input)) + ")");
    }
}

/* --- basename(p, ext): strip a literal trailing ext, with Node's subtle
 * "don't empty the segment" rule --- */
{
    const cases = [
        ["/foo/bar/baz.html", ".html", "baz"],
        ["/foo/bar/baz.html", "html", "baz."],   // suffix without the dot
        ["bar.html", ".html", "bar"],
        ["bar.html", "bar.html", ""],            // suffix === whole path -> ""
        ["bar", ".html", "bar"],                 // no match: suffix longer/absent -> unchanged
        ["index.js", ".js", "index"],
        ["index.js", "js", "index."],
        ["archive.tar.gz", ".gz", "archive.tar"],
        ["archive.tar.gz", ".tar.gz", "archive"],
        ["a.b.c", ".c", "a.b"],
        [".bashrc", ".bashrc", ""],              // suffix === whole path -> ""
        [".bashrc", "bashrc", "."],
        ["foo.js", ".ts", "foo.js"],              // suffix doesn't match -> unchanged
        ["/foo/bar/baz.html/", ".html", "baz"],   // trailing slash tolerated
        /* the obscure quirk: stripping would EMPTY the final segment, so
         * Node keeps it whole -- only an exact suffix===path match returns
         * "". A dotfile literally named ".html" is not emptied out. */
        ["/foo/.html", ".html", ".html"],
        [".html", ".html", ""],
        ["/a/.html", ".html", ".html"],
        ["//.html", ".html", ".html"],
    ];
    for (const [input, ext, want] of cases) {
        const got = basename(input, ext);
        assert(got === want,
            "basename(" + JSON.stringify(input) + "," + JSON.stringify(ext) + ") === " +
            JSON.stringify(want) + " (got " + JSON.stringify(got) + ")");
    }
    /* an explicit undefined or empty-string ext behaves like no ext at all */
    assert(basename("a.txt", undefined) === "a.txt", "basename(p, undefined) === basename(p)");
    assert(basename("a.txt", "") === "a.txt", "basename(p, '') === basename(p)");
}

/* --- extname(p): last '.' of the final segment; dotfile rules --- */
{
    const cases = [
        ["", ""],
        ["/", ""],
        ["index.html", ".html"],
        ["index.", "."],
        ["index", ""],
        [".index", ""],                 // leading dot -> dotfile, no extension
        ["index.coffee.md", ".md"],
        ["..", ""],                     // exactly ".." has no extension
        ["...", "."],                   // three dots: last one is the extension
        ["foo.", "."],
        [".foo", ""],
        [".foo.txt", ".txt"],
        ["a.b.c", ".c"],
        ["/a/b/.bashrc", ""],           // dotfile in a subdirectory
        ["/a/b/c.txt", ".txt"],
        ["/a.b/c", ""],                 // the dot is in a DIRECTORY name, not c
        ["a.", "."],
        ["..a", ".a"],
        ["..a.b", ".b"],
        ["/foo.bar.baz/asdf", ""],
        ["a/b.c/d", ""],
        ["foo..bar", ".bar"],
        ["foo...bar", ".bar"],
        ["..bashrc", ".bashrc"],
    ];
    for (const [input, want] of cases) {
        assert(extname(input) === want,
            "extname(" + JSON.stringify(input) + ") === " + JSON.stringify(want) +
            " (got " + JSON.stringify(extname(input)) + ")");
    }
}

/* --- isAbsolute(p) --- */
{
    const cases = [
        ["", false], ["/", true], ["//", true], ["a", false], ["/a", true],
        ["a/b", false], ["./a", false], ["..", false], ["/..", true], ["//a/b", true],
    ];
    for (const [input, want] of cases) {
        assert(isAbsolute(input) === want,
            "isAbsolute(" + JSON.stringify(input) + ") === " + want);
    }
}

/* --- relative(from, to): both sides resolve()d, then compared --- */
{
    const cases = [
        ["/a/b", "/a/c", "../c"],
        ["/a/b/c", "/a/b/d", "../d"],
        ["/a/b", "/a/b", ""],
        ["/a/b", "/a/b/c", "c"],
        ["/a/b/c", "/a/b", ".."],
        ["/", "/", ""],
        ["/", "/a", "a"],
        ["/a", "/", ".."],
        ["/a/b/c", "/x/y/z", "../../../x/y/z"],
        ["/foo/bar", "/foo/bar/baz/asdf", "baz/asdf"],
        ["/foo/bar/baz/asdf", "/foo/bar", "../.."],
        ["/foo", "/foo/bar/baz", "bar/baz"],
        ["/a/b/c/d", "/a/b/e/f", "../../e/f"],
        ["/usr/local/bin", "/usr/local/lib", "../lib"],
        /* relative (non-absolute) inputs -- both get resolve()d against the
         * same notional "/" cwd first, so the offset between them is the
         * same regardless of what the cwd is */
        ["a", "b", "../b"],
        ["a/b", "a/c", "../c"],
        ["foo", "foo/bar", "bar"],
        [".", "a", "a"],
        ["a", ".", ".."],
        ["", "", ""],
        ["a", "a", ""],
        ["x/y", "x/y/z", "z"],
        ["a/b/c", "d/e/f", "../../../d/e/f"],
    ];
    for (const [from, to, want] of cases) {
        const got = relative(from, to);
        assert(got === want,
            "relative(" + JSON.stringify(from) + "," + JSON.stringify(to) + ") === " +
            JSON.stringify(want) + " (got " + JSON.stringify(got) + ")");
    }
}

/* --- non-string arguments throw TypeError, are NOT coerced (matches Node's
 * validateString; contrast with dynajs:search's indexOf, which documents
 * the opposite choice) --- */
{
    throwsTypeError(() => join(1, 2), "join(number, number)");
    throwsTypeError(() => join("a", null), "join('a', null)");
    throwsTypeError(() => join("a", undefined), "join('a', undefined)");
    throwsTypeError(() => resolve("a", 5), "resolve('a', 5)");
    throwsTypeError(() => dirname(42), "dirname(number)");
    throwsTypeError(() => dirname(), "dirname() with no args");
    throwsTypeError(() => basename(null), "basename(null)");
    throwsTypeError(() => basename(), "basename() with no args");
    throwsTypeError(() => basename("a.txt", 5), "basename with a numeric suffix");
    throwsTypeError(() => basename("a.txt", null), "basename with an explicit null suffix");
    throwsTypeError(() => extname({}), "extname(object)");
    throwsTypeError(() => isAbsolute(5), "isAbsolute(number)");
    throwsTypeError(() => normalize(5), "normalize(number)");
    throwsTypeError(() => clean(5), "clean(number)");
    throwsTypeError(() => relative(1, "a"), "relative(number, string)");
    throwsTypeError(() => relative("a"), "relative() missing 'to'");
    throwsTypeError(() => relative(), "relative() with no args");
}

/* --- internal consistency properties (no external oracle needed) --- */
{
    /* isAbsolute agrees with the documented "starts with '/'" rule */
    const probes = ["", "/", "a", "/a", "a/b", "//x", "./a", "..", "/.."];
    for (const p of probes) {
        assert(isAbsolute(p) === (p.length > 0 && p[0] === "/"),
            "isAbsolute(" + JSON.stringify(p) + ") matches the documented rule");
    }
    /* normalize() is idempotent: normalizing an already-clean path is a no-op */
    for (const p of ["a/b/c", "/a/b/c", "..", "../..", ".", "/", "a/b/"]) {
        assert(normalize(normalize(p)) === normalize(p),
            "normalize() is idempotent for " + JSON.stringify(p));
    }
    /* dirname(p) + "/" + basename(p) reduces to a normalize()-equivalent
     * shape for ordinary (non-root, non-edge) paths */
    for (const p of ["/a/b/c", "a/b/c", "/foo/bar.txt", "x/y/z.ext"]) {
        const rejoined = join(dirname(p), basename(p));
        assert(rejoined === normalize(p),
            "join(dirname,basename) reconstructs " + JSON.stringify(p) +
            " (got " + JSON.stringify(rejoined) + ")");
    }
}

print("test_path: all tests passed (" + n + " assertions)");
