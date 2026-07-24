/*
 * dyna:search -- native substring search (KMP) backed by secure-c-libs.
 *
 * Build a dynajs with the module, then run this script:
 *   make CONFIG_SCL_MODULES=y CONFIG_SCL_MODULE_SEARCH=y \
 *        SCL_DIR=../secure-c-libs -j4 dynajs
 *   ./dynajs examples/dynajs_search.js
 */
import { indexOf, indexOfAll } from "dyna:search";

// indexOf -> byte offset of the first match, or -1.
print(indexOf("the quick brown fox", "quick")); // 4
print(indexOf("the quick brown fox", "cat"));   // -1

// indexOfAll -> ascending array of byte offsets of every match.
print(JSON.stringify(indexOfAll("abababab", "ab"))); // [0,2,4,6]

// Matches are OVERLAPPING (KMP semantics):
print(JSON.stringify(indexOfAll("aaaa", "aa")));     // [0,1,2]

// Empty needle: indexOf -> 0 (like String.prototype.indexOf("")),
// indexOfAll -> [].
print(indexOf("abc", ""));                 // 0
print(JSON.stringify(indexOfAll("abc", ""))); // []

// CAVEAT: offsets are UTF-8 BYTE offsets, not JS UTF-16 code-unit indices.
// For ASCII they coincide; for non-ASCII they differ:
print(indexOf("héllo", "llo"));   // 3  (byte offset; 'é' is 2 UTF-8 bytes)
print("héllo".indexOf("llo"));    // 2  (JS UTF-16 code-unit index)
