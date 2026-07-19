/*
 * dynascript — `meta@` optimization directive front-end
 *
 * Textually included by src/parser/parser.inc.c (unity build). Provides the
 * lexer-capture, data-driven registry, argument parser, safety-tier gating,
 * legality diagnostics and single-site attachment for `meta@` pragma comments
 * described in META_DIRECTIVES.md.
 *
 * SCOPE (deliberate): this layer RECORDS and VALIDATES directives and reports
 * them (diagnostics + an opt-in dump). It emits NO bytecode differences — a
 * program with no `meta@` comment compiles byte-for-byte as before, so the
 * test262 baseline holds trivially. The actual codegen transforms (unroll,
 * type specialisation, new opcodes, runtime flags) are the follow-on phase and
 * plug in at the marked hook in meta_apply_directive(); per the repo rules they
 * do not land without the differential oracle + a benchmark.
 *
 * `JSMetaHintSet`/`JSMetaDirective` hold only POD + pointers into the live
 * source buffer (valid for the whole parse) — no atoms, no owned JSValues — so
 * a set is freed by releasing one array. `pending_meta` accumulates directives
 * as the lexer skips their comment; meta_begin_statement() drains and processes
 * the whole set at the next statement boundary, then frees it.
 */

/* Construct classes a directive may legally attach to (bitmask). A concrete
   statement is classified as its specific bit OR'd with _STMT, so a directive
   that lists _STMT is legal on any statement while a _LOOP-only directive is
   legal only on a loop. */
#define JS_META_CTX_LOOP    (1u << 0)  /* for / for-in / for-of / while / do */
#define JS_META_CTX_FUNC    (1u << 1)  /* function declaration */
#define JS_META_CTX_CLASS   (1u << 2)  /* class declaration */
#define JS_META_CTX_BRANCH  (1u << 3)  /* if / switch */
#define JS_META_CTX_STMT    (1u << 4)  /* any statement */
#define JS_META_CTX_FILE    (1u << 5)  /* file-level control (enable/strict/dump) */

#define JS_META_SAFE    0  /* semantics-preserving: honouring or ignoring is identical */
#define JS_META_UNSAFE  1  /* assertion: wrong result / UB if the programmer lies */

typedef struct JSMetaDirInfo {
    const char *name;
    uint8_t tier;        /* JS_META_SAFE | JS_META_UNSAFE */
    uint8_t needs_target;/* TRUE if a variable target arg is expected */
    int8_t  min_iargs;   /* min numeric args (for diagnostics) */
    int8_t  max_iargs;   /* max numeric args, -1 = unbounded */
    uint32_t ctx_mask;   /* JS_META_CTX_* the directive is legal on */
    const char *help;    /* one-line description (for --meta-list / docs) */
} JSMetaDirInfo;

/* The registry. A directive's numeric id is its index in this table; the id is
   assigned by js_meta_lookup() at capture time, so the enum-order coupling that
   plagues opcode tables does not exist here. Keep entries grouped by level. */
#define MX_LOOP   JS_META_CTX_LOOP
#define MX_FUNC   JS_META_CTX_FUNC
#define MX_CLASS  JS_META_CTX_CLASS
#define MX_BRANCH JS_META_CTX_BRANCH
#define MX_STMT   JS_META_CTX_STMT
#define MX_FILE   JS_META_CTX_FILE
/* Common combinations. Construct-specific directives use exactly their kind
   bit (no _STMT), so e.g. a class directive is legal only where the construct
   classifies as CLASS. Directives that apply to any statement carry _STMT.
   A statement is classified as its kind bit OR _STMT (see meta_classify_token),
   so legality = (directive_mask & construct_bits) != 0 behaves as intended. */
#define MX_LOOPS  (JS_META_CTX_LOOP)
#define MX_BR     (JS_META_CTX_BRANCH)
#define MX_FN     (JS_META_CTX_FUNC)
#define MX_CL     (JS_META_CTX_CLASS)
#define MX_SCOPE  (JS_META_CTX_STMT | JS_META_CTX_LOOP | JS_META_CTX_FUNC) /* alloc/var hints: any statement, loop or function */
#define MX_HOTCOLD (JS_META_CTX_LOOP | JS_META_CTX_FUNC)

static const JSMetaDirInfo js_meta_table[] = {
    /* name             tier            needt min max  ctx        help */
    /* ---- 4.1 loop level ---- */
    { "unroll",         JS_META_SAFE,   0,   0,  1, MX_LOOPS,  "replicate the loop body N times per test" },
    { "autovec",        JS_META_SAFE,   0,   0,  0, MX_LOOPS,  "route an elementwise numeric loop to a SIMD kernel" },
    { "int32",          JS_META_UNSAFE, 0,   0,  0, MX_SCOPE,  "assume loop/variable arithmetic is int32" },
    { "float64",        JS_META_UNSAFE, 0,   0,  0, MX_SCOPE,  "assume loop/variable arithmetic is float64" },
    { "nobounds",       JS_META_UNSAFE, 0,   0,  0, MX_LOOPS,  "assume every array index is in bounds" },
    { "nopoll",         JS_META_UNSAFE, 0,   0,  0, MX_LOOPS,  "skip the interrupt poll on the loop back-edge" },
    { "reduce",         JS_META_SAFE,   1,   0,  0, MX_LOOPS,  "associative reduction (sum|prod|min|max)" },
    { "trip",           JS_META_SAFE,   0,   1,  1, MX_LOOPS,  "known trip count N" },
    { "fixed",          JS_META_SAFE,   0,   0,  0, MX_LOOPS,  "constant trip count (full unroll if small)" },
    { "stride1",        JS_META_UNSAFE, 0,   0,  0, MX_LOOPS,  "unit-stride contiguous access" },
    { "contiguous",     JS_META_UNSAFE, 0,   0,  0, MX_LOOPS,  "contiguous access (enables block paths)" },
    { "prefetch",       JS_META_SAFE,   0,   0,  1, MX_LOOPS,  "software-prefetch the next iteration's element" },
    { "independent",    JS_META_UNSAFE, 0,   0,  0, MX_LOOPS,  "assert no cross-iteration dependency" },
    { "parallel",       JS_META_UNSAFE, 0,   0,  0, MX_LOOPS,  "assert iterations may run in parallel" },
    /* ---- 4.2 function level ---- */
    { "inline",         JS_META_SAFE,   0,   0,  0, MX_FN,     "force inlining at call sites" },
    { "noinline",       JS_META_SAFE,   0,   0,  0, MX_FN,     "forbid inlining at call sites" },
    { "pure",           JS_META_UNSAFE, 0,   0,  0, MX_FN,     "no observable side effects (CSE/memoize/DCE)" },
    { "nosideeffects",  JS_META_UNSAFE, 0,   0,  0, MX_FN,     "alias of pure (/*#__NO_SIDE_EFFECTS__*/)" },
    { "arena",          JS_META_UNSAFE, 0,   0,  1, MX_FN,     "allocate the frame from a bump arena freed on return" },
    { "scoped_alloc",   JS_META_UNSAFE, 0,   0,  1, MX_FN,     "alias of arena" },
    { "noalloc",        JS_META_UNSAFE, 0,   0,  0, MX_FN,     "hot path allocates nothing (debug-trapped)" },
    { "memoize",        JS_META_SAFE,   0,   0,  0, MX_FN,     "cache results by argument tuple" },
    { "tailcall",       JS_META_SAFE,   0,   0,  0, MX_FN,     "proper tail calls, reuse the frame" },
    { "monomorphic",    JS_META_UNSAFE, 0,   0, -1, MX_FN,     "specialise the body for given arg types" },
    /* ---- 4.3 class level ---- */
    { "sealed",         JS_META_SAFE,   0,   0,  0, MX_CL,     "instances never add/delete properties" },
    { "fixed_layout",   JS_META_SAFE,   0,   0,  0, MX_CL,     "alias of sealed" },
    { "pod",            JS_META_UNSAFE, 0,   0,  0, MX_CL,     "plain data: no getters/setters/proxy/toPrimitive" },
    { "final",          JS_META_UNSAFE, 0,   0,  0, MX_CL,     "methods not overridden (devirtualise)" },
    { "preallocate_fields", JS_META_SAFE, 0, 0,  1, MX_CL,     "reserve the property array at construction" },
    { "soa",            JS_META_SAFE,   0,   0,  0, MX_CL,     "store arrays-of-instances struct-of-arrays" },
    { "noproto",        JS_META_UNSAFE, 0,   0,  0, MX_CL,     "own-property access only (skip proto walk)" },
    /* ---- 4.4 control flow ---- */
    { "likely",         JS_META_SAFE,   0,   0,  0, MX_BR,     "bias branch: likely arm as fall-through" },
    { "unlikely",       JS_META_SAFE,   0,   0,  0, MX_BR,     "bias branch: unlikely arm as forward jump" },
    { "unpredictable",  JS_META_SAFE,   0,   0,  0, MX_BR,     "50/50 branch: emit branchless select" },
    { "jumptable",      JS_META_SAFE,   0,   0,  0, MX_BR,     "force switch to a computed jump table" },
    { "dense",          JS_META_SAFE,   0,   0,  0, MX_BR,     "switch cases are dense (jump table)" },
    { "assume",         JS_META_UNSAFE, 1,   0, -1, MX_SCOPE,  "optimiser may assume the predicate holds" },
    { "invariant",      JS_META_UNSAFE, 1,   0, -1, MX_SCOPE,  "alias of assume" },
    { "noexcept",       JS_META_UNSAFE, 0,   0,  0, (JS_META_CTX_STMT|JS_META_CTX_BRANCH|JS_META_CTX_LOOP|JS_META_CTX_FUNC), "block cannot throw (elide unwind bookkeeping)" },
    { "nothrow",        JS_META_UNSAFE, 0,   0,  0, (JS_META_CTX_STMT|JS_META_CTX_BRANCH|JS_META_CTX_LOOP|JS_META_CTX_FUNC), "alias of noexcept" },
    /* ---- 4.5 allocation level ---- */
    { "preallocate",    JS_META_SAFE,   0,   0,  1, MX_SCOPE,  "reserve capacity on new Array/{}/Map/Set" },
    { "reuse",          JS_META_UNSAFE, 0,   0,  0, MX_SCOPE,  "reuse a scratch object across iterations" },
    { "pool",           JS_META_UNSAFE, 0,   0,  1, MX_SCOPE,  "pool/free-list a reused buffer" },
    { "stack",          JS_META_UNSAFE, 0,   0,  0, MX_SCOPE,  "object does not escape (scalar-replace)" },
    { "noescape",       JS_META_UNSAFE, 0,   0,  0, MX_SCOPE,  "alias of stack" },
    { "transient",      JS_META_SAFE,   0,   0,  0, MX_SCOPE,  "short-lived (young-generation treatment)" },
    { "weak",           JS_META_SAFE,   0,   0,  0, MX_SCOPE,  "alias of transient" },
    /* ---- shared ---- */
    { "hot",            JS_META_SAFE,   0,   0,  0, MX_HOTCOLD,"optimise for this loop/function (IC priming)" },
    { "cold",           JS_META_SAFE,   0,   0,  0, MX_HOTCOLD,"outline this loop/function (I-cache)" },
    /* ---- variable-based directives (dynascript additions) ---- */
    { "range",          JS_META_UNSAFE, 1,   2,  2, MX_SCOPE,  "range(v, lo, hi): v stays in [lo,hi] (branchless/unsigned)" },
    { "nonnull",        JS_META_UNSAFE, 1,   0,  0, MX_SCOPE,  "nonnull(v): v is never null/undefined (skip null checks)" },
    { "nonzero",        JS_META_UNSAFE, 1,   0,  0, MX_SCOPE,  "nonzero(v): v is never 0 (skip div-by-zero guard)" },
    { "type",           JS_META_UNSAFE, 1,   0,  0, MX_SCOPE,  "type(v, i32|f64|str|...): v has this primitive type" },
    { "const",          JS_META_UNSAFE, 1,   0,  0, MX_SCOPE,  "const(v): v is never reassigned (const-fold/SSA)" },
    { "frozen",         JS_META_UNSAFE, 1,   0,  0, MX_SCOPE,  "frozen(v): v references a frozen object (cache loads)" },
    { "align",          JS_META_UNSAFE, 1,   1,  1, MX_SCOPE,  "align(buf, N): buffer aligned to N bytes (SIMD)" },
    { "length",         JS_META_UNSAFE, 1,   1,  1, MX_SCOPE,  "length(arr, n): arr length is exactly n (hoist checks)" },
    { "init",           JS_META_UNSAFE, 1,   0,  0, MX_SCOPE,  "init(v): v is definitely initialised (skip TDZ check)" },
    { "volatile",       JS_META_SAFE,   1,   0,  0, MX_SCOPE,  "volatile(v): never cache v in a register/IC" },
    /* ---- file-level control ---- */
    { "enable",         JS_META_SAFE,   1,   0,  0, MX_FILE,   "enable(unsafe): opt in to UNSAFE directives for this file" },
    { "strict",         JS_META_SAFE,   0,   0,  0, MX_FILE,   "promote meta diagnostics to hard errors" },
    { "dump",           JS_META_SAFE,   0,   0,  0, MX_FILE,   "print captured directives (also DYNASCRIPT_META_DUMP=1)" },
};

#define JS_META_COUNT ((int)countof(js_meta_table))

#undef MX_LOOP
#undef MX_FUNC
#undef MX_CLASS
#undef MX_BRANCH
#undef MX_STMT
#undef MX_FILE
#undef MX_LOOPS
#undef MX_BR
#undef MX_FN
#undef MX_CL
#undef MX_SCOPE
#undef MX_HOTCOLD

/* One parsed directive. Holds only POD and pointers into the live source
   buffer (valid for the whole parse) — no atoms, no owned JSValues — so a set
   is released by freeing one array. */
typedef struct JSMetaDirective {
    int id;                /* registry index (js_meta_table), or -1 if unknown */
    const uint8_t *pos;    /* start of the directive, for diagnostics */
    const uint8_t *name;   /* directive name span (into source) */
    int name_len;
    const uint8_t *target; /* first identifier arg (variable target), or NULL */
    int target_len;
    const uint8_t *sarg;   /* keyword-style arg (reduce op, type name), or NULL */
    int sarg_len;
    int n_iargs;
    int64_t iargs[3];      /* signed integer args */
} JSMetaDirective;

struct JSMetaHintSet {
    JSMetaDirective *items;
    int count;
    int size;
};

static BOOL meta_is_ident_first(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

static BOOL meta_is_ident_char(int c)
{
    return meta_is_ident_first(c) || (c >= '0' && c <= '9');
}

/* cheap gate on the common comment path: at the comment body, skip blanks and
   test for the literal `meta@`. Returns the pointer just past `meta@`, or NULL. */
static const uint8_t *js_meta_prefix(const uint8_t *p, const uint8_t *end)
{
    /* skip leading blanks incl. newlines so a block comment may carry the
       directive on a later line: `/`+`*` \n meta@... */
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    if (end - p >= 5 && p[0] == 'm' && p[1] == 'e' && p[2] == 't' &&
        p[3] == 'a' && p[4] == '@')
        return p + 5;
    return NULL;
}

/* return the registry index for a directive name span, or -1 if unknown */
static int js_meta_lookup(const uint8_t *name, int len)
{
    int i, n = JS_META_COUNT;
    for (i = 0; i < n; i++) {
        const char *e = js_meta_table[i].name;
        if ((int)strlen(e) == len && memcmp(e, name, len) == 0)
            return i;
    }
    return -1;
}

static int meta_set_append(JSContext *ctx, JSMetaHintSet **pset,
                           const JSMetaDirective *dir)
{
    JSMetaHintSet *set = *pset;
    if (!set) {
        set = js_mallocz(ctx, sizeof(*set));
        if (!set)
            return -1;
        *pset = set;
    }
    if (js_resize_array(ctx, (void *)&set->items, sizeof(set->items[0]),
                        &set->size, set->count + 1))
        return -1;
    set->items[set->count++] = *dir;
    return 0;
}

static void meta_set_free(JSContext *ctx, JSMetaHintSet *set)
{
    if (!set)
        return;
    js_free(ctx, set->items);
    js_free(ctx, set);
}

/* parse an argument list starting just after '(' until the matching ')'.
   Forgiving by design: identifiers become the target then a keyword arg,
   signed integers fill iargs[], operators (assume's `>=` etc.) are skipped. */
static void js_parse_meta_args(const uint8_t **pp, const uint8_t *end,
                               JSMetaDirective *dir)
{
    const uint8_t *p = *pp;
    while (p < end && *p != ')') {
        int c = *p;
        if (c == ' ' || c == '\t' || c == ',' || c == '\n' || c == '\r') {
            p++;
        } else if (meta_is_ident_first(c)) {
            const uint8_t *id = p;
            while (p < end && meta_is_ident_char(*p))
                p++;
            if (!dir->target) {
                dir->target = id;
                dir->target_len = (int)(p - id);
            } else if (!dir->sarg) {
                dir->sarg = id;
                dir->sarg_len = (int)(p - id);
            }
        } else if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
            int neg = (c == '-');
            int64_t v = 0;
            BOOL has = FALSE;
            if (c == '-' || c == '+')
                p++;
            while (p < end && *p >= '0' && *p <= '9') {
                /* saturate rather than wrap: hint values are advisory, but a
                   silently-wrapped/negative count would mislead the dump and a
                   future codegen consumer (signed overflow is defined here by
                   -fwrapv, so this is a validation choice, not UB avoidance). */
                if (v > (INT64_MAX - 9) / 10)
                    v = INT64_MAX;
                else
                    v = v * 10 + (*p - '0');
                p++;
                has = TRUE;
            }
            if (has && dir->n_iargs < (int)countof(dir->iargs))
                dir->iargs[dir->n_iargs++] = neg ? -v : v;
        } else {
            p++; /* operator / stray char */
        }
    }
    if (p < end && *p == ')')
        p++;
    *pp = p;
}

/* capture: scan a `meta@`-bearing comment and append every directive it lists
   to s->pending_meta. Returns -1 (with the exception left by js_mallocz set) if
   an allocation fails, so the caller propagates the OOM rather than continuing
   with a pending exception; returns 0 otherwise. 'p' points at the first byte of
   the comment body; 'is_block' selects the terminator. */
static __exception int js_parse_meta_comment(JSParseState *s, const uint8_t *p,
                                             BOOL is_block)
{
    const uint8_t *end = s->buf_end;
    const uint8_t *q;

    /* bound the scan to this comment's content */
    if (is_block) {
        for (q = p; q + 1 < s->buf_end; q++) {
            if (q[0] == '*' && q[1] == '/')
                break;
        }
        end = q;
    } else {
        for (q = p; q < s->buf_end; q++) {
            if (*q == '\n' || *q == '\r')
                break;
        }
        end = q;
    }

    while (p < end) {
        JSMetaDirective dir;
        const uint8_t *name;
        const uint8_t *body;

        while (p < end && (*p == ' ' || *p == '\t' || *p == ',' ||
                           *p == '\n' || *p == '\r'))
            p++;
        body = js_meta_prefix(p, end);
        if (!body)
            break; /* no further directive; trailing prose in a block is fine */
        memset(&dir, 0, sizeof(dir));
        dir.pos = p;
        p = body;
        name = p;
        while (p < end && meta_is_ident_char(*p))
            p++;
        dir.name = name;
        dir.name_len = (int)(p - name);
        if (dir.name_len == 0)
            break;
        dir.id = js_meta_lookup(name, dir.name_len);
        while (p < end && (*p == ' ' || *p == '\t'))
            p++;
        if (p < end && *p == '(') {
            p++;
            js_parse_meta_args(&p, end, &dir);
        }
        if (meta_set_append(s->ctx, &s->pending_meta, &dir))
            return -1;
    }
    return 0;
}

/* Diagnostic. Under `meta@strict` this throws a SyntaxError at the directive
   and returns -1 (aborting the parse); otherwise it prints a warning to stderr
   and returns 0. The caller propagates a non-zero return. */
static int __attribute__((format(printf, 3, 4)))
meta_diag(JSParseState *s, const JSMetaDirective *d, const char *fmt, ...)
{
    va_list ap;
    int ret = 0;

    va_start(ap, fmt);
    if (s->meta_strict) {
        ret = js_parse_error_v(s, d->pos, fmt, ap);
    } else {
        int col_num;
        int line_num = get_line_col(&col_num, s->buf_start,
                                    d->pos - s->buf_start);
        fprintf(stderr, "%s:%d: warning: ",
                s->filename ? s->filename : "<input>", line_num + 1);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
    va_end(ap);
    return ret;
}

static BOOL meta_dump_enabled(JSParseState *s)
{
    /* getenv only (never setenv) → safe to call concurrently; no cached static
       so nothing to race under TSan. Only reached when a directive attaches. */
    return s->meta_dump || getenv("DYNASCRIPT_META_DUMP") != NULL;
}

static void meta_dump(JSParseState *s, const JSMetaDirective *d,
                      const JSMetaDirInfo *info)
{
    int col_num, line_num, i;

    if (!meta_dump_enabled(s))
        return;
    line_num = get_line_col(&col_num, s->buf_start, d->pos - s->buf_start);
    fprintf(stderr, "meta: %.*s", d->name_len, (const char *)d->name);
    if (d->target || d->n_iargs || d->sarg) {
        fprintf(stderr, "(");
        if (d->target)
            fprintf(stderr, "%.*s", d->target_len, (const char *)d->target);
        if (d->sarg)
            fprintf(stderr, "%s%.*s", d->target ? "," : "",
                    d->sarg_len, (const char *)d->sarg);
        for (i = 0; i < d->n_iargs; i++)
            fprintf(stderr, "%s%lld",
                    (d->target || d->sarg || i) ? "," : "",
                    (long long)d->iargs[i]);
        fprintf(stderr, ")");
    }
    fprintf(stderr, " @ %s:%d [%s]\n",
            s->filename ? s->filename : "<input>", line_num + 1,
            info->tier == JS_META_UNSAFE ? "UNSAFE" : "SAFE");
}

/* Describe, for diagnostics, the most specific construct a directive targets.
   Used when a directive attached to the wrong construct (usually because it was
   not placed immediately before its intended target). */
static const char *meta_ctx_describe(uint32_t mask)
{
    if (mask & JS_META_CTX_LOOP)   return "a loop (for/while/do)";
    if (mask & JS_META_CTX_CLASS)  return "a class declaration";
    if (mask & JS_META_CTX_FUNC)   return "a function declaration";
    if (mask & JS_META_CTX_BRANCH) return "an if/switch statement";
    return "a statement";
}

/* Map the leading token of a statement to the construct classes a directive
   may target on it (specific bit | _STMT). */
static uint32_t meta_classify_token(int tok)
{
    switch (tok) {
    case TOK_FOR:
    case TOK_WHILE:
    case TOK_DO:
        return JS_META_CTX_LOOP | JS_META_CTX_STMT;
    case TOK_IF:
    case TOK_SWITCH:
        return JS_META_CTX_BRANCH | JS_META_CTX_STMT;
    case TOK_FUNCTION:
        return JS_META_CTX_FUNC | JS_META_CTX_STMT;
    case TOK_CLASS:
        return JS_META_CTX_CLASS | JS_META_CTX_STMT;
    default:
        return JS_META_CTX_STMT;
    }
}

/* Apply a file-level control directive (enable/strict/dump) immediately. */
static int meta_apply_file(JSParseState *s, const JSMetaDirective *d,
                           const JSMetaDirInfo *info)
{
    if (!strcmp(info->name, "enable")) {
        if (d->target && d->target_len == 6 &&
            !memcmp(d->target, "unsafe", 6)) {
            s->meta_unsafe_enabled = TRUE;
        } else {
            return meta_diag(s, d, "meta@enable expects 'unsafe', e.g. "
                             "meta@enable(unsafe)");
        }
    } else if (!strcmp(info->name, "strict")) {
        s->meta_strict = TRUE;
    } else if (!strcmp(info->name, "dump")) {
        s->meta_dump = TRUE;
    }
    return 0;
}

/* Validate + record one directive against the construct it attached to.
   HOOK: this is where a future phase reads the directive and drives codegen
   (unroll/specialise/…) or sets a runtime flag. Today it only diagnoses and
   dumps, so the emitted bytecode is unchanged. Returns -1 only when a strict
   diagnostic threw. */
static int meta_apply_directive(JSParseState *s, const JSMetaDirective *d,
                                uint32_t cbits)
{
    const JSMetaDirInfo *info;

    if (d->id < 0)
        return meta_diag(s, d, "unknown meta directive 'meta@%.*s'",
                         d->name_len, (const char *)d->name);
    info = &js_meta_table[d->id];

    if (info->ctx_mask & JS_META_CTX_FILE)
        return meta_apply_file(s, d, info);

    /* UNSAFE assertions must be opted in per file. */
    if (info->tier == JS_META_UNSAFE && !s->meta_unsafe_enabled)
        return meta_diag(s, d, "unsafe directive 'meta@%.*s' requires a "
                         "'// meta@enable(unsafe)' earlier in the file",
                         d->name_len, (const char *)d->name);

    /* Legality against the attached construct. A directive attaches to the next
       statement, so the usual cause of a mismatch is that it was not placed
       immediately before its intended target. */
    if (!(info->ctx_mask & cbits))
        return meta_diag(s, d, "'meta@%.*s' applies to %s and must immediately "
                         "precede one", d->name_len, (const char *)d->name,
                         meta_ctx_describe(info->ctx_mask));

    /* Shape diagnostics: warn (or, under strict, error) but still record. */
    if (info->needs_target && !d->target) {
        if (meta_diag(s, d, "'meta@%.*s' expects a variable target, e.g. "
                      "'meta@%.*s(x)'", d->name_len, (const char *)d->name,
                      d->name_len, (const char *)d->name))
            return -1;
    }
    if (d->n_iargs < info->min_iargs ||
        (info->max_iargs >= 0 && d->n_iargs > info->max_iargs)) {
        if (meta_diag(s, d, "'meta@%.*s' expects %d..%d numeric argument(s), "
                      "got %d", d->name_len, (const char *)d->name,
                      info->min_iargs, info->max_iargs, d->n_iargs))
            return -1;
    }

    meta_dump(s, d, info);
    /* (future) fd->meta_flags |= ...; loop/branch hint into codegen here. */
    return 0;
}

/* Drain the directives captured immediately before the current construct and
   process the whole set against 'cbits' (the construct classes it may target),
   then free it. Returns -1 iff a strict diagnostic threw. Consuming pending_meta
   here makes a second attach call on the same statement a no-op. */
static __exception int meta_attach(JSParseState *s, uint32_t cbits)
{
    JSMetaHintSet *set = s->pending_meta;
    int i, ret = 0;

    if (!set)
        return 0;
    s->pending_meta = NULL;
    for (i = 0; i < set->count; i++) {
        if (meta_apply_directive(s, &set->items[i], cbits)) {
            ret = -1;
            break;
        }
    }
    meta_set_free(s->ctx, set);
    return ret;
}

/* Attach to the statement whose leading token is current. Called at the
   statement switch, after any leading label has been consumed so the token is
   the construct keyword. */
static __exception int meta_begin_statement(JSParseState *s)
{
    return meta_attach(s, meta_classify_token(s->token.val));
}

/* Release any captured-but-unattached directives at end of parse. */
static void meta_free_state(JSParseState *s)
{
    meta_set_free(s->ctx, s->pending_meta);
    s->pending_meta = NULL;
}
