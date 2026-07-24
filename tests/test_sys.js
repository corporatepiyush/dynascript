/* test_sys.js -- dyna:file filesystem ops + dyna:sys process/environment.
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_sys.js
 * Prints "test_sys: all tests passed (N assertions)" on success; throws on
 * failure.
 *
 * The filesystem surface (metadata, directories, links, globbing, temp) lives
 * in dyna:file (alongside buffered content I/O); dyna:sys keeps only the
 * process/environment surface. This test exercises both.
 *
 * HERMETIC + IDEMPOTENT: every filesystem mutation happens inside a private
 * temp directory created with makeTempDir(); the whole tree (plus the few
 * makeTempFile/makeTempDir siblings) is removed at the end (and, on failure,
 * in a finally block), so a successful OR failed run leaves nothing behind. */

import {
    stat, lstat, exists,
    readDir, makeDir, remove, removeAll, rename,
    symlink, readLink, realPath, chmod,
    glob,
    tempDir, makeTempDir, makeTempFile,
} from "dyna:file";
import {
    env, getEnv, setEnv, args, cwd, chDir, platform, pid, hostName, homeDir,
} from "dyna:sys";
import { writeFile } from "dyna:file";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function throws(fn, msg) {
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    assert(caught !== null, msg + " (expected throw)");
    assert(caught instanceof Error, msg + " (throws an Error, got " + caught + ")");
    return caught;
}
function throwsCode(fn, code, msg) {
    const e = throws(fn, msg);
    assert(e.code === code, msg + " (code " + e.code + " expected " + code + ")");
}
function eqArray(actual, expected, msg) {
    assert(Array.isArray(actual), msg + " (is array, got " + typeof actual + ")");
    assert(actual.length === expected.length,
        msg + " (length " + actual.length + " vs " + expected.length +
        "): [" + actual.join(",") + "]");
    for (let i = 0; i < expected.length; i++)
        assert(actual[i] === expected[i],
            msg + " [" + i + "]: '" + actual[i] + "' vs '" + expected[i] + "'");
}

const origCwd = cwd();
const root = makeTempDir("dyna-sys-test-");
let passed = false;

try {
    /* ---------------- root / basic metadata ---------------- */
    assert(typeof root === "string" && root.length > 0, "makeTempDir returns a path");
    assert(root.indexOf("dyna-sys-test-") >= 0, "temp dir carries the prefix");
    assert(exists(root), "fresh temp dir exists");
    {
        const s = stat(root);
        assert(s.isDir === true, "root isDir");
        assert(s.isFile === false, "root not isFile");
        assert(s.isSymlink === false, "root not symlink (via stat)");
        assert(typeof s.mode === "number", "mode is a number");
        assert(typeof s.mtimeMs === "number" && s.mtimeMs > 1e12, "mtimeMs sane");
        assert(typeof s.atimeMs === "number" && s.atimeMs > 1e12, "atimeMs sane");
        assert(typeof s.ctimeMs === "number" && s.ctimeMs > 1e12, "ctimeMs sane");
        assert(typeof s.uid === "number" && s.uid >= 0, "uid sane");
        assert(typeof s.gid === "number" && s.gid >= 0, "gid sane");
        assert(typeof s.ino === "number" && s.ino > 0, "ino sane");
        assert(typeof s.nlink === "number" && s.nlink >= 1, "nlink >= 1");
        assert(typeof s.size === "number", "size is a number");
    }

    /* ---------------- process / environment sanity ---------------- */
    {
        const plat = platform();
        assert(plat === "darwin" || plat === "linux", "platform is darwin/linux: " + plat);
        assert(Number.isInteger(pid()) && pid() > 0, "pid is a positive integer");
        assert(typeof hostName() === "string" && hostName().length > 0, "hostName non-empty");
        const a = args();
        assert(Array.isArray(a) && a.length >= 1, "args is a non-empty array");
        assert(typeof a[0] === "string" && a[0].length > 0, "args[0] is a string");
        const c = cwd();
        assert(typeof c === "string" && c[0] === "/", "cwd is an absolute path");
        assert(typeof tempDir() === "string" && tempDir().length > 0, "tempDir non-empty");
        assert(tempDir()[tempDir().length - 1] !== "/" || tempDir() === "/", "tempDir has no trailing slash");
        assert(typeof homeDir() === "string" && homeDir().length > 0, "homeDir non-empty");
        const e = env();
        assert(typeof e === "object" && e !== null, "env() is an object");
        assert(!Array.isArray(e), "env() is not an array");
    }

    /* env round-trip */
    {
        const key = "DYNAJS_SYS_TEST_" + pid();
        assert(getEnv(key) === undefined, "unset env var is undefined");
        setEnv(key, "hello123");
        assert(getEnv(key) === "hello123", "getEnv sees setEnv value");
        assert(env()[key] === "hello123", "env() reflects setEnv value");
        setEnv(key, "changed-value");
        assert(getEnv(key) === "changed-value", "setEnv overwrites");
        setEnv(key, "");
        assert(getEnv(key) === "", "setEnv empty string round-trips");
        assert(getEnv("DYNAJS_SYS_DEFINITELY_UNSET_" + pid()) === undefined,
            "random unset var undefined");
    }

    /* ---------------- makeDir ---------------- */
    {
        const sub = root + "/sub";
        makeDir(sub);
        assert(exists(sub) && stat(sub).isDir, "makeDir creates a directory");

        const deep = root + "/a/b/c";
        makeDir(deep, { recursive: true });
        assert(exists(deep) && stat(deep).isDir, "makeDir recursive creates nested dirs");
        assert(exists(root + "/a") && exists(root + "/a/b"), "recursive makes all parents");

        /* recursive on an existing dir is a no-op (no throw) */
        makeDir(deep, { recursive: true });
        assert(exists(deep), "makeDir recursive on existing is a no-op");

        /* non-recursive on an existing dir throws EEXIST */
        throwsCode(() => makeDir(sub), "EEXIST", "makeDir non-recursive on existing throws");

        /* non-recursive with a missing parent throws ENOENT */
        throwsCode(() => makeDir(root + "/nope/child"), "ENOENT",
            "makeDir non-recursive missing parent throws");
    }

    /* ---------------- stat / lstat / exists on a file ---------------- */
    {
        const f = root + "/hello.txt";
        writeFile(f, "hello world");        /* 11 bytes */
        const s = stat(f);
        assert(s.isFile === true, "file isFile");
        assert(s.isDir === false, "file not isDir");
        assert(s.isSymlink === false, "file not symlink");
        assert(s.size === 11, "file size is 11 (got " + s.size + ")");
        assert(s.nlink >= 1, "file nlink >= 1");
        const ls = lstat(f);
        assert(ls.isFile === true && ls.isSymlink === false, "lstat regular file");

        assert(exists(f) === true, "exists true for a real file");
        assert(exists(root + "/no-such-file") === false, "exists false for missing");
    }

    /* ---------------- chmod (deterministic perm bits) ---------------- */
    {
        const f = root + "/hello.txt";
        const modes = [0o600, 0o644, 0o755, 0o700, 0o640];
        for (const m of modes) {
            chmod(f, m);
            assert((stat(f).mode & 0o777) === m,
                "chmod sets perm bits to 0o" + m.toString(8) +
                " (got 0o" + (stat(f).mode & 0o777).toString(8) + ")");
        }
        /* full st_mode keeps the regular-file type bit set */
        assert((stat(f).mode & 0o170000) !== 0, "full st_mode carries type bits");
    }

    /* ---------------- symlink / readLink / realPath ---------------- */
    {
        const link = root + "/link.txt";
        symlink("hello.txt", link);            /* relative target */
        assert(lstat(link).isSymlink === true, "lstat sees the symlink");
        assert(stat(link).isFile === true, "stat follows the symlink to a file");
        assert(readLink(link) === "hello.txt", "readLink returns the target");
        assert(realPath(link) === realPath(root + "/hello.txt"),
            "realPath resolves the symlink to the same canonical path");

        /* dangling symlink: the link node exists, but the target does not */
        const dangling = root + "/dangling";
        symlink("target-does-not-exist", dangling);
        assert(exists(dangling) === true, "exists true for a dangling symlink (lstat)");
        assert(lstat(dangling).isSymlink === true, "lstat sees the dangling symlink");
        throws(() => stat(dangling), "stat on a dangling symlink throws");
        assert(readLink(dangling) === "target-does-not-exist", "readLink of dangling link");
    }

    /* ---------------- readDir ---------------- */
    {
        const rd = root + "/rd";
        makeDir(rd);
        writeFile(rd + "/a.txt", "A");
        writeFile(rd + "/b.txt", "BB");
        makeDir(rd + "/d");
        symlink("a.txt", rd + "/s");
        const ents = readDir(rd);
        assert(ents.length === 4, "readDir returns 4 entries (got " + ents.length + ")");
        eqArray(ents.map(e => e.name), ["a.txt", "b.txt", "d", "s"], "readDir sorted by name");
        const byName = {};
        for (const e of ents) byName[e.name] = e;
        assert(byName["a.txt"].isFile && !byName["a.txt"].isDir && !byName["a.txt"].isSymlink,
            "a.txt is a file");
        assert(byName["b.txt"].isFile, "b.txt is a file");
        assert(byName["d"].isDir && !byName["d"].isFile, "d is a directory");
        assert(byName["s"].isSymlink && !byName["s"].isDir, "s is a symlink");
        /* "." and ".." are never reported */
        assert(ents.every(e => e.name !== "." && e.name !== ".."),
            "readDir skips . and ..");

        /* readDir on an empty directory is an empty array */
        makeDir(root + "/empty-rd");
        eqArray(readDir(root + "/empty-rd"), [], "readDir of empty dir is []");
    }

    /* ---------------- rename ---------------- */
    {
        const oldp = root + "/old.txt", newp = root + "/renamed.txt";
        writeFile(oldp, "xyz");
        rename(oldp, newp);
        assert(exists(oldp) === false, "rename removes the source name");
        assert(exists(newp) === true, "rename creates the destination");
        assert(stat(newp).size === 3, "renamed file keeps its content");
        remove(newp);
    }

    /* ---------------- remove ---------------- */
    {
        const f = root + "/rm.txt";
        writeFile(f, "y");
        remove(f);
        assert(!exists(f), "remove deletes a file");

        const d = root + "/rmdir";
        makeDir(d);
        remove(d);
        assert(!exists(d), "remove deletes an empty directory");

        /* remove of a non-empty directory throws; remove of a missing path throws */
        const full = root + "/full";
        makeDir(full);
        writeFile(full + "/f", "z");
        throws(() => remove(full), "remove of a non-empty dir throws");
        throwsCode(() => remove(root + "/missing-xyz"), "ENOENT", "remove of a missing path throws");
        removeAll(full);
        assert(!exists(full), "removeAll cleans the non-empty dir");
    }

    /* ---------------- removeAll (recursive, symlink-safe) ---------------- */
    {
        /* external targets that must SURVIVE removeAll of the tree */
        writeFile(root + "/external.txt", "keep-me");
        makeDir(root + "/extdir");
        writeFile(root + "/extdir/keep.txt", "keep-me-too");

        /* a nested tree with symlinks pointing OUT of it */
        makeDir(root + "/tree/a/b/c", { recursive: true });
        writeFile(root + "/tree/f0", "0");
        writeFile(root + "/tree/a/f1", "1");
        writeFile(root + "/tree/a/b/f2", "2");
        writeFile(root + "/tree/a/b/c/f3", "3");
        symlink(root + "/external.txt", root + "/tree/a/linkout");   /* -> external file */
        symlink(root + "/extdir", root + "/tree/a/b/dirlink");       /* -> external dir  */
        assert(exists(root + "/tree/a/b/c/f3"), "deep file created");
        assert(lstat(root + "/tree/a/linkout").isSymlink, "linkout is a symlink");

        removeAll(root + "/tree");
        assert(exists(root + "/tree") === false, "removeAll removes the whole tree");
        assert(exists(root + "/external.txt") === true,
            "removeAll is symlink-safe: external file survived");
        assert(exists(root + "/extdir") === true && exists(root + "/extdir/keep.txt") === true,
            "removeAll is symlink-safe: external dir + its contents survived");

        /* missing path is a no-op (no throw) */
        removeAll(root + "/does-not-exist");
        assert(true, "removeAll of a missing path is a no-op");

        /* removeAll of a single file */
        writeFile(root + "/single.txt", "s");
        removeAll(root + "/single.txt");
        assert(!exists(root + "/single.txt"), "removeAll deletes a single file");

        /* removeAll of a top-level symlink removes only the link */
        symlink(root + "/external.txt", root + "/toplink");
        removeAll(root + "/toplink");
        assert(exists(root + "/toplink") === false, "removeAll deletes the symlink");
        assert(exists(root + "/external.txt") === true, "removeAll did not follow the symlink");

        remove(root + "/external.txt");
        removeAll(root + "/extdir");
    }

    /* ---------------- glob battery ---------------- */
    {
        const g = root + "/glob";
        makeDir(g);
        for (const name of ["one.txt", "two.txt", "three.md",
                            "abc", "axc", "azc", "ab", "bob", "cat", "dog",
                            ".hidden", "top.js"])
            writeFile(g + "/" + name, "x");
        makeDir(g + "/sub");
        writeFile(g + "/sub/nested.js", "x");
        makeDir(g + "/sub/deep");
        writeFile(g + "/sub/deep/deeper.js", "x");

        eqArray(glob("*.txt", { cwd: g }), ["one.txt", "two.txt"], "glob *.txt");
        eqArray(glob("*.js", { cwd: g }), ["top.js"], "glob *.js (single segment, no cross-dir)");
        eqArray(glob("**/*.js", { cwd: g }),
            ["sub/deep/deeper.js", "sub/nested.js", "top.js"], "glob **/*.js");
        eqArray(glob("a?c", { cwd: g }), ["abc", "axc", "azc"], "glob a?c");
        eqArray(glob("?ne.txt", { cwd: g }), ["one.txt"], "glob ?ne.txt");
        eqArray(glob("[abc]*", { cwd: g }),
            ["ab", "abc", "axc", "azc", "bob", "cat"], "glob [abc]*");
        eqArray(glob("[a-c]*", { cwd: g }),
            ["ab", "abc", "axc", "azc", "bob", "cat"], "glob [a-c]* (range)");
        eqArray(glob("[!a-c]*", { cwd: g }),
            ["dog", "one.txt", "sub", "three.md", "top.js", "two.txt"],
            "glob [!a-c]* (negated range)");
        eqArray(glob("*/nested.js", { cwd: g }), ["sub/nested.js"], "glob */nested.js");
        eqArray(glob("*/*.js", { cwd: g }), ["sub/nested.js"], "glob */*.js");
        eqArray(glob("*.xyz", { cwd: g }), [], "glob with no match is []");
        eqArray(glob("nope/**", { cwd: g }), [], "glob under a missing dir is []");

        /* '*' must never match a leading '.' */
        assert(glob("*", { cwd: g }).indexOf(".hidden") === -1, "glob * skips dotfiles");
        assert(glob("*", { cwd: g }).indexOf("one.txt") >= 0, "glob * includes plain files");

        /* '**' spans directories, matches files AND dirs, skips dotfiles + "." */
        {
            const all = glob("**", { cwd: g });
            assert(all.length === 15, "glob ** count is 15 (got " + all.length + ")");
            for (const want of ["sub", "sub/deep", "sub/deep/deeper.js",
                                "sub/nested.js", "one.txt", "top.js", "abc"])
                assert(all.indexOf(want) >= 0, "glob ** includes " + want);
            assert(all.indexOf(".hidden") === -1, "glob ** skips dotfiles");
            assert(all.indexOf(".") === -1, "glob ** does not emit .");
            assert(all.every(p => p[0] !== "."), "glob ** emits no dot-prefixed path");
        }

        /* literal (no-wildcard) pattern resolves an explicit existing name */
        eqArray(glob("sub/nested.js", { cwd: g }), ["sub/nested.js"], "glob literal nested path");
        eqArray(glob("sub", { cwd: g }), ["sub"], "glob literal directory name");
        eqArray(glob(".hidden", { cwd: g }), [".hidden"], "glob explicit dotfile name");
        eqArray(glob("no-such-thing", { cwd: g }), [], "glob literal missing name is []");

        /* absolute pattern -> absolute results */
        {
            const abs = glob(g + "/*.txt");
            assert(abs.length === 2, "absolute glob returns 2 results");
            assert(abs.every(p => p[0] === "/"), "absolute glob results are absolute");
            assert(abs[0].endsWith("/one.txt") && abs[1].endsWith("/two.txt"),
                "absolute glob resolves the right names");
            assert(abs[0] === g + "/one.txt" && abs[1] === g + "/two.txt",
                "absolute glob reconstructs the full path");
        }

        /* default cwd is "." -- glob from inside g via chDir */
        {
            const save = cwd();
            chDir(g);
            eqArray(glob("*.txt"), ["one.txt", "two.txt"], "glob default cwd is current dir");
            chDir(save);
        }

        removeAll(g);
        assert(!exists(g), "glob tree cleaned");
    }

    /* ---------------- glob must not recurse into a symlink cycle ---------------- */
    {
        const gc = root + "/globcycle";
        makeDir(gc + "/inner", { recursive: true });
        writeFile(gc + "/inner/leaf.txt", "L");
        symlink(gc, gc + "/inner/loop");   /* symlink back up to an ancestor => cycle */

        const all = glob("**", { cwd: gc });     /* must TERMINATE */
        assert(Array.isArray(all), "glob ** over a cycle terminates (returns an array)");
        assert(all.indexOf("inner") >= 0 && all.indexOf("inner/leaf.txt") >= 0,
            "glob ** over a cycle still finds the real entries");
        assert(all.indexOf("inner/loop") >= 0, "glob ** emits the symlink as a leaf");
        assert(all.every(p => p.indexOf("loop/") === -1),
            "glob ** never descends through the symlink loop");
        eqArray(glob("**/*.txt", { cwd: gc }), ["inner/leaf.txt"],
            "glob **/*.txt over a cycle terminates with the real match");

        removeAll(gc);
        assert(!exists(gc), "glob-cycle tree cleaned");
    }

    /* ---------------- tempDir / makeTempDir / makeTempFile ---------------- */
    {
        const td = tempDir();
        assert(exists(td) && stat(td).isDir, "tempDir points at a real directory");

        const tf = makeTempFile("dyna-sys-tf-");
        assert(typeof tf === "string" && exists(tf) && stat(tf).isFile,
            "makeTempFile creates an empty file");
        assert(tf.indexOf("dyna-sys-tf-") >= 0, "makeTempFile carries the prefix");
        remove(tf);                 /* sibling in the system temp dir -- clean it up */
        assert(!exists(tf), "makeTempFile cleaned up");

        const tdir = makeTempDir("dyna-sys-td-");
        assert(typeof tdir === "string" && exists(tdir) && stat(tdir).isDir,
            "makeTempDir creates a directory");
        assert(tdir !== root, "each makeTempDir is unique");
        removeAll(tdir);            /* sibling -- clean it up */
        assert(!exists(tdir), "makeTempDir cleaned up");
    }

    /* ---------------- cwd / chDir round-trip ---------------- */
    {
        const before = cwd();
        chDir(root);
        assert(realPath(cwd()) === realPath(root), "chDir changes cwd to the target");
        chDir(before);
        assert(cwd() === before, "chDir restores the original cwd");
        throwsCode(() => chDir(root + "/no-such-dir"), "ENOENT", "chDir to a missing dir throws");
    }

    /* ---------------- error paths throw descriptive Errors ---------------- */
    {
        const missing = root + "/definitely-missing";
        throwsCode(() => stat(missing), "ENOENT", "stat missing throws ENOENT");
        throwsCode(() => lstat(missing), "ENOENT", "lstat missing throws ENOENT");
        throwsCode(() => readDir(missing), "ENOENT", "readDir missing throws ENOENT");
        throwsCode(() => realPath(missing), "ENOENT", "realPath missing throws ENOENT");
        throwsCode(() => chmod(missing, 0o644), "ENOENT", "chmod missing throws ENOENT");
        throwsCode(() => rename(missing, root + "/x"), "ENOENT", "rename missing source throws");

        /* readLink on a non-symlink is an error (EINVAL) */
        writeFile(root + "/plain.txt", "p");
        const e = throws(() => readLink(root + "/plain.txt"), "readLink of a non-symlink throws");
        assert(typeof e.message === "string" && e.message.indexOf("readLink") >= 0,
            "error message names the operation");
        assert(typeof e.errno === "number", "error carries a numeric errno");
        remove(root + "/plain.txt");

        /* readDir on a file (not a directory) throws */
        writeFile(root + "/notdir", "x");
        throws(() => readDir(root + "/notdir"), "readDir on a file throws");
        remove(root + "/notdir");
    }

    /* ---------------- final cleanup (happy path) ---------------- */
    chDir(origCwd);
    removeAll(root);
    assert(exists(root) === false, "temp root fully removed -- hermetic");

    passed = true;
} finally {
    if (!passed) {
        try { chDir(origCwd); } catch (e) { /* ignore */ }
        try { removeAll(root); } catch (e) { /* ignore */ }
    }
}

print("test_sys: all tests passed (" + n + " assertions)");
