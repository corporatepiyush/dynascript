// run_all.js — spawn `qjs` on every example in this directory and aggregate the
// results into one overall PASS/FAIL. qjs runs a single file at a time, so this
// runner shells out with os.exec (block:true inherits stdio and returns the
// child's exit code).
//
// Usage:
//   qjs examples/js/run_all.js                 # uses $QJS, else "qjs" on PATH
//   QJS=/path/to/qjs qjs examples/js/run_all.js
//   qjs examples/js/run_all.js /path/to/qjs    # explicit interpreter as arg
//
// NB: the async_streams example needs Array.fromAsync (a dynascript addition);
// point QJS at the dynascript qjs to run the whole suite.

import * as std from "std";
import * as os from "os";

// The example suite, in a sensible reading order. harness.js and this file are
// intentionally excluded.
const EXAMPLES = [
  "json5_parser.js",
  "mini_vm.js",
  "parser_combinators.js",
  "csv_engine.js",
  "regex_router.js",
  "state_machine.js",
  "persistent_list.js",
  "lru_cache.js",
  "event_emitter.js",
  "bignum_crypto.js",
  "reactive_signals.js",
  "bytebuffer_codec.js",
  "promise_scheduler.js",
  "async_streams.js",
];

/** Directory containing this script, derived from how it was invoked. */
function scriptDir() {
  const self = scriptArgs[0] ?? "examples/js/run_all.js";
  const slash = self.lastIndexOf("/");
  return slash < 0 ? "." : self.slice(0, slash);
}

/** Which qjs to spawn: explicit arg, then $QJS, then "qjs" on PATH. */
function resolveQjs() {
  return scriptArgs[1] || std.getenv("QJS") || "qjs";
}

/** print + flush, so our lines interleave correctly with child stdio. */
function log(line = "") {
  print(line);
  std.out.flush();
}

function main() {
  const dir = scriptDir();
  const qjs = resolveQjs();

  log(`running ${EXAMPLES.length} examples with: ${qjs}\n`);

  const results = [];
  for (const file of EXAMPLES) {
    const path = `${dir}/${file}`;
    log(`──────── ${file} ────────`);
    const started = Date.now();
    const code = os.exec([qjs, path], { block: true });
    const ms = Date.now() - started;
    results.push({ file, code, ms });
    log("");
  }

  // Summary table.
  log("════════ summary ════════");
  let failed = 0;
  for (const { file, code, ms } of results) {
    const status = code === 0 ? "PASS" : `FAIL (exit ${code})`;
    if (code !== 0) failed++;
    log(`  ${status.padEnd(16)} ${file.padEnd(24)} ${ms}ms`);
  }

  const passed = results.length - failed;
  log(`\n  ${passed}/${results.length} suites passed`);

  const ok = failed === 0;
  log(ok ? "\nALL PASS" : "\nSOME FAILED");
  std.exit(ok ? 0 : 1);
}

main();
