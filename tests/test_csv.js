/* test_csv.js — dynajs:csv (file CRUD, RFC-4180, SIMD parse + atomic I/O).
 * Hermetic: everything happens inside a temp dir that is removed at the end.
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_csv.js */
import * as csv from "dynajs:csv";
import { makeTempDir, removeAll, exists } from "dynajs:sys";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function eq(a, b, msg) { assert(JSON.stringify(a) === JSON.stringify(b), msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function throws(fn, msg) { let t = false; try { fn(); } catch { t = true; } assert(t, msg); }

const dir = makeTempDir("dynajs-csv-test-");
const P = (name) => dir + "/" + name;

try {
    /* ---------------- create ---------------- */
    {
        const p = P("a.csv");
        const res = csv.create({ path: p, headers: ["Name", "Age", "City"], rows: [["Alice", "30", "NYC"], ["Bob", "25", "LA"]] });
        eq(res.rows, 2, "create returns row count");
        assert(exists(p), "file created");
        const r = csv.read({ path: p });
        eq(r.headers, ["Name", "Age", "City"], "create headers");
        eq(r.rows, [["Alice", "30", "NYC"], ["Bob", "25", "LA"]], "create rows");
        eq(r.totalRows, 2, "create totalRows");
    }
    /* headers-only + fail-if-exists + overwrite */
    {
        const p = P("empty.csv");
        csv.create({ path: p, headers: ["X", "Y"] });
        eq(csv.read({ path: p }).totalRows, 0, "headers-only file has 0 rows");
        throws(() => csv.create({ path: p, headers: ["X"] }), "create fails when file exists");
        csv.create({ path: p, headers: ["Z"], overwrite: true });
        eq(csv.read({ path: p }).headers, ["Z"], "overwrite replaces the file");
        throws(() => csv.create({ path: P("bad.csv"), headers: ["A", "B"], rows: [["only-one"]] }), "create rejects a wrong-width row");
        throws(() => csv.create({ path: P("noheaders.csv"), headers: [] }), "create requires >= 1 header");
        /* parent dirs auto-created */
        csv.create({ path: P("nested/deep/x.csv"), headers: ["a"] });
        assert(exists(P("nested/deep/x.csv")), "parent dirs auto-created");
    }

    /* ---------------- read: pagination + column filter ---------------- */
    {
        const p = P("page.csv");
        const rows = [];
        for (let i = 0; i < 100; i++) rows.push(["id" + i, "v" + i, String(i)]);
        csv.create({ path: p, headers: ["ID", "Val", "N"], rows });
        eq(csv.read({ path: p, offset: 0, limit: 10 }).rows.length, 10, "limit=10 → 10 rows");
        eq(csv.read({ path: p, offset: 95, limit: 10 }).rows.length, 5, "offset near end clamps");
        eq(csv.read({ path: p, offset: 10, limit: 1 }).rows[0], ["id10", "v10", "10"], "offset picks the right row");
        eq(csv.read({ path: p }).totalRows, 100, "totalRows ignores pagination");
        const sel = csv.read({ path: p, columns: ["N", "ID"], offset: 0, limit: 2 });
        eq(sel.headers, ["N", "ID"], "column filter reorders headers");
        eq(sel.rows[0], ["0", "id0"], "column filter reorders + selects cells");
        throws(() => csv.read({ path: p, columns: ["Nope"] }), "read rejects an unknown column");
    }

    /* ---------------- addRow: positional + named ---------------- */
    {
        const p = P("add.csv");
        csv.create({ path: p, headers: ["Name", "Age"] });
        let res = csv.addRow({ path: p, rows: [["Alice", "30"], ["Bob", "25"]] });
        eq(res.added, 2, "addRow positional added count");
        eq(res.totalRows, 2, "addRow totalRows");
        res = csv.addRow({ path: p, rows: [{ Name: "Carol", Age: "40" }, { Age: "22", Name: "Dave" }] });
        eq(res.totalRows, 4, "addRow named grows total");
        /* named form: missing key → empty, extra key ignored */
        csv.addRow({ path: p, rows: [{ Name: "Eve", Bogus: "x" }] });
        const r = csv.read({ path: p });
        eq(r.rows[2], ["Carol", "40"], "named row maps by header");
        eq(r.rows[3], ["Dave", "22"], "named row order-independent");
        eq(r.rows[4], ["Eve", ""], "named row: missing key → empty, extra key ignored");
    }

    /* ---------------- updateCell ---------------- */
    {
        const p = P("upd.csv");
        csv.create({ path: p, headers: ["A", "B", "C"], rows: [["1", "2", "3"], ["4", "5", "6"]] });
        csv.updateCell({ path: p, row: 0, column: "B", value: "99" });
        csv.updateCell({ path: p, row: 1, columnIndex: 2, value: "60" });
        csv.updateCell({ path: p, row: 0, column: "A", value: "" });   /* clear a cell */
        const r = csv.read({ path: p });
        eq(r.rows[0], ["", "99", "3"], "updateCell by name + clear");
        eq(r.rows[1], ["4", "5", "60"], "updateCell by columnIndex");
        throws(() => csv.updateCell({ path: p, row: 9, column: "A", value: "x" }), "updateCell rejects OOB row");
        throws(() => csv.updateCell({ path: p, row: 0, column: "Z", value: "x" }), "updateCell rejects bad column");
    }

    /* ---------------- removeRow ---------------- */
    {
        const p = P("rm.csv");
        csv.create({ path: p, headers: ["V"], rows: [["a"], ["b"], ["c"], ["d"]] });
        const res = csv.removeRow({ path: p, row: 1 });   /* remove "b" */
        eq(res.totalRows, 3, "removeRow new total");
        eq(csv.read({ path: p }).rows, [["a"], ["c"], ["d"]], "removeRow shifts up");
        throws(() => csv.removeRow({ path: p, row: 99 }), "removeRow rejects OOB");
    }

    /* ---------------- addColumn / removeColumn / renameColumn ---------------- */
    {
        const p = P("cols.csv");
        csv.create({ path: p, headers: ["A", "B"], rows: [["1", "2"], ["3", "4"]] });
        csv.addColumn({ path: p, column: "C", defaultValue: "z" });
        eq(csv.read({ path: p }).rows[0], ["1", "2", "z"], "addColumn with default");
        csv.addColumn({ path: p, column: "D" });   /* empty default */
        eq(csv.read({ path: p }).rows[1], ["3", "4", "z", ""], "addColumn empty default");
        throws(() => csv.addColumn({ path: p, column: "A" }), "addColumn rejects a duplicate");

        csv.renameColumn({ path: p, oldName: "B", newName: "Beta" });
        eq(csv.read({ path: p }).headers, ["A", "Beta", "C", "D"], "renameColumn");
        csv.renameColumn({ path: p, oldName: "A", newName: "A" });   /* no-op */
        throws(() => csv.renameColumn({ path: p, oldName: "A", newName: "C" }), "renameColumn rejects an existing name");
        throws(() => csv.renameColumn({ path: p, oldName: "Nope", newName: "X" }), "renameColumn rejects a missing column");

        csv.removeColumn({ path: p, column: "C" });
        eq(csv.read({ path: p }).headers, ["A", "Beta", "D"], "removeColumn by name compacts");
        csv.removeColumn({ path: p, columnIndex: 0 });
        eq(csv.read({ path: p }).headers, ["Beta", "D"], "removeColumn by index");
        eq(csv.read({ path: p }).rows[0], ["2", ""], "removeColumn drops the right cells");
    }

    /* ---------------- range readers ---------------- */
    {
        const p = P("range.csv");
        const rows = [];
        for (let i = 0; i < 50; i++) rows.push([String(i), "name" + i, "e" + i + "@x.com"]);
        csv.create({ path: p, headers: ["ID", "Name", "Email"], rows });

        eq(csv.readColumnValuesRange({ path: p, column: "Email", start: 0, end: 3 }), ["e0@x.com", "e1@x.com", "e2@x.com"], "readColumnValuesRange");
        eq(csv.readColumnValuesRange({ path: p, column: "ID" }).length, 50, "readColumnValuesRange all");
        throws(() => csv.readColumnValuesRange({ path: p, column: "ID", start: 0, end: 2000 }), "readColumnValuesRange caps at 1000");

        const rr = csv.readRowRange({ path: p });   /* default: single row 0 */
        eq(rr.rows.length, 1, "readRowRange default is one row");
        eq(rr.rows[0], ["0", "name0", "e0@x.com"], "readRowRange row 0");
        eq(csv.readRowRange({ path: p, start: 5, end: 8 }).rows.length, 3, "readRowRange window");
        throws(() => csv.readRowRange({ path: p, start: 0, end: 200 }), "readRowRange caps at 100");

        const sc = csv.selectColumnRange({ path: p, columns: ["Email", "ID"], start: 0, end: 2 });
        eq(sc.columns, ["Email", "ID"], "selectColumnRange columns");
        eq(sc.rows[0], ["e0@x.com", "0"], "selectColumnRange projects + reorders");
        throws(() => csv.selectColumnRange({ path: p, columns: ["Nope"] }), "selectColumnRange rejects bad column");
        throws(() => csv.selectColumnRange({ path: p, columns: [] }), "selectColumnRange requires columns");
    }

    /* ---------------- RFC 4180 quoting round-trips ---------------- */
    {
        const p = P("quote.csv");
        const tricky = [
            ["a,b", "plain", "x"],                       /* embedded comma */
            ['he said "hi"', "y", "z"],                  /* embedded quotes */
            ["line1\nline2", "multi", "w"],              /* embedded newline */
            ['"leading quote', "trailing\r", "  spaced  "],
            ["", "empty-first", ""],                     /* empty fields */
        ];
        csv.create({ path: p, headers: ["Weird", "B", "C"], rows: tricky });
        const r = csv.read({ path: p });
        eq(r.rows, tricky, "RFC-4180 quoting round-trips (comma/quote/newline/empty)");
        /* mutate a quoted table + reread — quoting must survive edits */
        csv.updateCell({ path: p, row: 0, column: "B", value: 'now, with "quotes"' });
        eq(csv.read({ path: p }).rows[0], ["a,b", 'now, with "quotes"', "x"], "quoting survives updateCell");
    }

    /* ---------------- errors ---------------- */
    {
        throws(() => csv.read({ path: dir + "/does-not-exist.csv" }), "read of a missing file throws");
        throws(() => csv.read({}), "missing path throws");
        throws(() => csv.create({}), "create without headers throws");
    }

    print("test_csv: all tests passed (" + n + " assertions)");
} finally {
    removeAll(dir);
}
