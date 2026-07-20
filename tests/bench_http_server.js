/* bench_http_server.js — start the scl:http HttpServer for external load testing.
 * Usage: dynajs tests/bench_http_server.js [port] [workers] [uptimeMs]
 * Prints "LISTENING <port>" once bound, then serves for uptimeMs (default 30s). */
import { HttpServer } from "scl:http";
import * as std from "std";

const port = parseInt(scriptArgs[1] || "8098", 10);
const workers = parseInt(scriptArgs[2] || "4", 10);
const uptime = parseInt(scriptArgs[3] || "30000", 10);

const server = new HttpServer({
    port,
    workers,
    routes: {
        "/": "hello world\n",
        "/json": { status: 200, contentType: "application/json",
                   body: JSON.stringify({ msg: "ok", engine: "dynajs" }) },
    },
});
server.start();
print("LISTENING " + server.port);
std.out.flush(); /* unbuffer so a launcher can read the port immediately */

// keep the JS event loop parked (no busy-wait) while native worker threads serve
setTimeout(() => { server.close(); }, uptime);
