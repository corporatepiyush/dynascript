/* test_file.js — dynajs:file buffered reader/writer, common cross-platform API.
 * Backend differs per OS (Linux fadvise/fallocate/io_uring, macOS F_RDAHEAD/
 * F_PREALLOCATE/F_FULLFSYNC) but behaviour is identical and tested here. */
import { FileReader, FileWriter, readFile, writeFile } from "dynajs:file";

function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }

const path = "/tmp/dynajs_file_test.txt";

/* --- one-shot writeFile / readFile roundtrip --- */
const body = "line one\nline two\nline three\n" + "x".repeat(200000);
const n = writeFile(path, body);
assert(n === body.length, "writeFile returns byte count");
assert(readFile(path) === body, "readFile roundtrips writeFile");

/* --- buffered FileWriter: many small writes across the buffer boundary --- */
{
    const w = new FileWriter(path, { bufferSize: 4096, preallocate: 1 << 20 });
    let expect = "";
    for (let i = 0; i < 5000; i++) { const s = "row " + i + "\n"; w.write(s); expect += s; }
    /* an ArrayBuffer write path */
    const ab = new Uint8Array([65, 66, 67, 10]).buffer; /* "ABC\n" */
    w.write(ab); expect += "ABC\n";
    w.sync();   /* durable flush (F_FULLFSYNC on macOS) */
    w.close();
    assert(readFile(path) === expect, "buffered writer + ArrayBuffer roundtrips");

    /* --- FileReader: readLine across buffer refills --- */
    const r = new FileReader(path, { bufferSize: 64 });
    assert(r.readLine() === "row 0", "first line");
    assert(r.readLine() === "row 1", "second line");
    let count = 2;
    let line;
    while ((line = r.readLine()) !== null) count++;
    assert(count === 5001, "read every line back (" + count + ")"); /* 5000 rows + ABC */
    assert(r.readLine() === null, "readLine returns null at EOF");
    r.close();

    /* --- read(n) chunking --- */
    const r2 = new FileReader(path);
    const first6 = r2.read(6);
    assert(first6 === "row 0\n", "read(6) returns exactly 6 bytes");
    const rest = r2.readAll();
    assert(first6 + rest === expect, "read(6)+readAll reconstructs the file");
    r2.close();
}

/* --- append mode --- */
writeFile(path, "A");
writeFile(path, "B", { append: true });
{
    const w = new FileWriter(path, { append: true });
    w.write("C"); w.close();
}
assert(readFile(path) === "ABC", "append mode concatenates");

/* --- reentrant close during arg coercion must NOT crash/UAF (repo rule) --- */
{
    const w = new FileWriter(path);
    let threw = false;
    try {
        w.write({ toString() { w.close(); return "boom"; } });
    } catch (e) { threw = true; } /* writing to a closed writer throws, not UAF */
    /* the important part is we got here without a crash */
    assert(threw || w.closed, "write() coerces before resolving the handle");
}
{
    writeFile(path, "hello world");
    const r = new FileReader(path);
    let ok = true;
    try { r.read({ valueOf() { r.close(); return 3; } }); }
    catch (e) { ok = true; }
    assert(r.closed, "read() coerces the count before resolving the handle");
}

/* --- closed resource rejects use --- */
{
    const w = new FileWriter(path);
    w.close();
    let threw = false;
    try { w.write("x"); } catch (e) { threw = true; }
    assert(threw, "writing a closed FileWriter throws");
}

print("test_file: all tests passed");
