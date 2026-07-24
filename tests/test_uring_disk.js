/* test_uring_disk.js — dyna:uring disk I/O (Linux, CONFIG_IO_URING only).
 * Proves the io_uring reader returns byte-identical data to a pread reference,
 * then does a crude page-cache timing. A faithful disk-throughput comparison
 * needs O_DIRECT on real storage; on a cached/virtualised fs both hit cache. */
import * as uring from "dyna:uring";
import * as std from "std";

function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }

const path = "/tmp/uring_test.dat";
let chunk = "";
for (let i = 0; i < 1024; i++) chunk += String.fromCharCode(33 + (i * 7) % 94);
let big = "";
for (let i = 0; i < 8192; i++) big += chunk; /* ~8 MB */

const f = std.open(path, "w");
f.puts(big);
f.close();

const viaUring = uring.readFile(path);
const viaPread = uring.readFileSync(path);
assert(viaUring.length === big.length, "uring length == source (" + viaUring.length + ")");
assert(viaUring === viaPread, "io_uring bytes == pread bytes");
assert(viaUring === big, "io_uring bytes == written bytes");

const cu = uring.checksum(path, true);
const cp = uring.checksum(path, false);
assert(cu.bytes === big.length, "checksum byte count");
assert(cu.sum === cp.sum, "io_uring checksum == pread checksum");
print("test_uring_disk: correctness OK (" + cu.bytes + " bytes, sum=" + cu.sum + ")");

function bench(useUring) {
    const t0 = performance.now();
    let s = 0;
    for (let i = 0; i < 30; i++) s ^= uring.checksum(path, useUring).sum;
    return performance.now() - t0;
}
bench(true); bench(false); /* warm the cache */
const tu = bench(true), tp = bench(false);
print("read 8MB x30 -- io_uring: " + tu.toFixed(1) + "ms  pread: " +
      tp.toFixed(1) + "ms (page-cache; understates real-disk io_uring gain)");
