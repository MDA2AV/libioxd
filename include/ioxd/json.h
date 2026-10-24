/*
 * ioxd/json.h - JSON written as you go, and a struct described once, serialized with one call.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ioxd/http.h"

struct ioxd_pipe;                               /* ioxd/pipe.h */

/* ── JSON, written as you go ───────────────────────────────────────────────────────────── */

/* A forward-only JSON writer, the shape of .NET's Utf8JsonWriter: no tree, no allocation. The
 * bytes go straight into the reply slab - or a raw pipe's, or a buffer - written in place at
 * its tail, escaped as they are written, and stream out as the slab fills: a call is a few
 * stores, not a copy through the engine. Nesting and commas are tracked, so a handler just says
 * what it means:
 *
 *     ioxd_json j = ioxd_json_reply(ctx);                  // content-type: application/json
 *     ioxd_json_object(&j);
 *         ioxd_json_key(&j, "id");    ioxd_json_int(&j, id);
 *         ioxd_json_key(&j, "name");  ioxd_json_string(&j, name);
 *         ioxd_json_key(&j, "tags");  ioxd_json_array(&j);
 *             ioxd_json_cstr(&j, "new");
 *         ioxd_json_end(&j);
 *     ioxd_json_end(&j);
 *     if (!ioxd_json_done(&j)) { ... }                     // whole: nothing failed, nothing open
 *
 * Strings are emitted byte for byte, with only '"', '\' and the control characters escaped:
 * invalid UTF-8 goes out exactly as it came in, so untrusted input has to be validated first.
 * Every call returns false once the sink is gone (the peer left; the buffer is full), the nesting
 * passed IOXD_JSON_DEPTH, or the call had no place in the document - a key outside an object, a
 * value where a key was due, an end with nothing open. The rest is then dropped, so checking the
 * last call is enough; check done() after it to catch an end that was never written. */
#define IOXD_JSON_DEPTH 63                          /* levels: one bit of each mask below apiece */
typedef struct ioxd_json {
    enum {
        IOXD_JSON_TO_REPLY,
        IOXD_JSON_TO_PIPE,
        IOXD_JSON_TO_MEM,
    } kind;

    union {
        ioxd_ctx         *ctx;
        struct ioxd_pipe *pipe;
        struct { char *p; size_t cap, *len; } mem;
    } to;
    void    *tail;                              /* private: the slab's writer, where the bytes land */
    char    *run;                               /* private: where a struct's run began, not yet committed */

    uint64_t has_value;                         /* per level: a value is there, so a comma is due */
    uint64_t is_object;                         /* per level: it closes with '}' rather than ']'  */
    unsigned depth;
    bool     after_key;                         /* the next value follows a key: no comma        */
    bool     failed;
} ioxd_json;

ioxd_json ioxd_json_reply(ioxd_ctx *ctx);                        /* into the reply; sets its content type */
ioxd_json ioxd_json_pipe (struct ioxd_pipe *pipe);               /* into a raw pipe's slab                */
ioxd_json ioxd_json_mem  (char *buf, size_t cap, size_t *len);   /* into memory; *len is what was written */

bool ioxd_json_object(ioxd_json *j);                             /* {                          */
bool ioxd_json_array (ioxd_json *j);                             /* [                          */
bool ioxd_json_end   (ioxd_json *j);                             /* } or ], whichever is open  */
bool ioxd_json_done  (ioxd_json *j);                             /* nothing failed, all closed */
bool ioxd_json_key   (ioxd_json *j, const char *name);           /* "name":                    */
bool ioxd_json_string(ioxd_json *j, ioxd_slice s);               /* "...", escaped             */
bool ioxd_json_cstr  (ioxd_json *j, const char *s);              /* NULL is null            */
bool ioxd_json_int   (ioxd_json *j, int64_t v);
bool ioxd_json_uint  (ioxd_json *j, uint64_t v);
bool ioxd_json_double(ioxd_json *j, double v);                   /* the shortest that reads back the same; nan and inf become null */
bool ioxd_json_float (ioxd_json *j, float v);                    /* the same, read back as a float: 0.1f is 0.1 */
bool ioxd_json_bool  (ioxd_json *j, bool v);
bool ioxd_json_null  (ioxd_json *j);
bool ioxd_json_raw   (ioxd_json *j, ioxd_slice json);            /* already JSON: copied as is */

/* A value by its C type, and a key with one: the _Generic picks ioxd_json_int for the integer
 * types, _uint for the unsigned ones, _float and _double for those two, _bool, _cstr for a char
 * pointer, _string for a slice. */
#define IOXD_JSON_VALUE(j, x) _Generic((x),                                                        \
        bool: ioxd_json_bool,                                                                      \
        char: ioxd_json_int, signed char: ioxd_json_int, short: ioxd_json_int,                     \
        int: ioxd_json_int, long: ioxd_json_int, long long: ioxd_json_int,                         \
        unsigned char: ioxd_json_uint, unsigned short: ioxd_json_uint, unsigned: ioxd_json_uint,   \
        unsigned long: ioxd_json_uint, unsigned long long: ioxd_json_uint,                         \
        float: ioxd_json_float, double: ioxd_json_double,                                          \
        char *: ioxd_json_cstr, const char *: ioxd_json_cstr,                                      \
        ioxd_slice: ioxd_json_string)((j), (x))

/* A key and its value in one line. The answer goes through a function so that a field written for
 * its effect - `IOXD_JSON_FIELD(j, "n", n);` - is a plain statement and not a value the compiler
 * sees discarded, while `if (IOXD_JSON_FIELD(j, "n", n))` still reads it. The key goes with its
 * length, so a literal's is folded at compile time. */
static inline bool ioxd__json_wrote(bool ok) { return ok; }
bool ioxd__json_key_n(ioxd_json *j, const char *name, size_t len);      /* what the macros call: ioxd_json_key with the length known */
#define IOXD_JSON_FIELD(j, name, x)                                                                \
    ioxd__json_wrote(ioxd__json_key_n((j), (name), strlen(name)) && IOXD_JSON_VALUE((j), (x)))

/* A struct described once, serialized with one call. The description is a list of fields, each
 * line its kind, its C type (or, for a nested struct, that struct's name) and its name:
 *
 *     #define USER_FIELDS(X)                       \
 *         X(VALUE,    int64_t,      id)            \
 *         X(VALUE,    const char *, name)          \
 *         X(OBJECT,   address,      address)       \
 *         X(OPTIONAL, address,      billing)       \
 *         X(ARRAY,    const char *, tags,   n_tags)    \
 *         X(OBJECTS,  order,        orders, n_orders)
 *     IOXD_JSON_STRUCT(user, USER_FIELDS)
 *
 * VALUE is a scalar, written by its C type; OBJECT a nested struct held by value; OPTIONAL a
 * pointer to one, null where the pointer is NULL; ARRAY scalars and the field holding their
 * count; OBJECTS the same for nested structs. IOXD_JSON_STRUCT defines the struct and the
 * function - struct user, and user_to_json(ioxd_json *, const struct user *); IOXD_JSON_WRITER
 * only the function, for a struct declared elsewhere with the same fields. A nested struct's own
 * IOXD_JSON_STRUCT comes first. Counts are size_t; arrays are pointers to their first element.
 * A note beside a field is written as a block comment, the way playground/hello/main.c writes
 * them: a // one would run on through the backslash and swallow the lines after it.
 *
 * The function writes the struct in one run rather than a call per piece: the shape is known
 * when it is compiled, so every key goes out as a token spelled then - its quotes and colon, and
 * the comma before it, one fixed-size copy, never scanned for escapes, a field's name being a C
 * identifier - and every value by an inline writer, with room checked once a piece against the
 * most that piece can take. When a piece needs more room than is left, what is written so far is
 * committed and the sink asked for more - a reply's or a pipe's slab is sent then, as with any
 * other write - so a struct streams past any number of flushes. The bytes, and the failures, are
 * exactly those of the calls above: the same escapes, numbers and nesting limit. The function
 * also defines name__json_run, the run itself, which an enclosing struct's run calls. */
#define IOXD_JSON_STRUCT(name, FIELDS)                                                             \
    struct name { FIELDS(IOXD__JSON_MEMBER) };                                                     \
    IOXD_JSON_WRITER(name, FIELDS)
#define IOXD_JSON_WRITER(name, FIELDS)                                                             \
    static inline ioxd__json_run name##__json_run(ioxd_json *j, ioxd__json_run r, const struct name *v) \
    {                                                                                              \
        char before = 0;                                                                           \
        r = ioxd__json_down(j, r, '{');                                                            \
        FIELDS(IOXD__JSON_RUN)                                                                     \
        (void)before;                                                                              \
        return ioxd__json_up(j, r, '}');                                                           \
    }                                                                                              \
    static inline bool name##_to_json(ioxd_json *j, const struct name *v)                         \
    {                                                                                              \
        return ioxd__json_close(j, name##__json_run(j, ioxd__json_open(j), v).p);                  \
    }

/* What each kind of line becomes: a member, and a piece of the run. */
#define IOXD__JSON_MEMBER(kind, ...)                  IOXD__JSON_MEMBER_##kind(__VA_ARGS__)
#define IOXD__JSON_MEMBER_VALUE(type, field)          type field;
#define IOXD__JSON_MEMBER_OBJECT(sname, field)        struct sname field;
#define IOXD__JSON_MEMBER_OPTIONAL(sname, field)      const struct sname *field;
#define IOXD__JSON_MEMBER_ARRAY(type, field, count)   type *field; size_t count;
#define IOXD__JSON_MEMBER_OBJECTS(sname, field, count) const struct sname *field; size_t count;
#define IOXD__JSON_RUN(kind, ...)                     IOXD__JSON_RUN_##kind(__VA_ARGS__)
#define IOXD__JSON_TOKEN(field)                       "\"" #field "\":"
#define IOXD__JSON_RUN_KEY(field)                                                                  \
    r = ioxd__json_key(j, r, before, IOXD__JSON_TOKEN(field), sizeof IOXD__JSON_TOKEN(field) - 1); \
    before = ',';
#define IOXD__JSON_RUN_VALUE(type, field)             IOXD__JSON_RUN_KEY(field) r = IOXD__JSON_PUT(j, r, v->field);
#define IOXD__JSON_RUN_OBJECT(sname, field)           IOXD__JSON_RUN_KEY(field) r = sname##__json_run(j, r, &v->field);
#define IOXD__JSON_RUN_OPTIONAL(sname, field)         IOXD__JSON_RUN_KEY(field)                     \
    r = v->field ? sname##__json_run(j, r, v->field) : ioxd__json_lit(j, r, "null", 4);
#define IOXD__JSON_RUN_ARRAY(type, field, count)      IOXD__JSON_RUN_KEY(field) r = ioxd__json_down(j, r, '['); \
    for (size_t i_ = 0; i_ < v->count && r.p; i_++) {                                               \
        if (i_)                                                                                    \
            r = ioxd__json_lit(j, r, ",", 1);                                                      \
        r = IOXD__JSON_PUT(j, r, v->field[i_]);                                                    \
    }                                                                                              \
    r = ioxd__json_up(j, r, ']');
#define IOXD__JSON_RUN_OBJECTS(sname, field, count)   IOXD__JSON_RUN_KEY(field) r = ioxd__json_down(j, r, '['); \
    for (size_t i_ = 0; i_ < v->count && r.p; i_++) {                                               \
        if (i_)                                                                                    \
            r = ioxd__json_lit(j, r, ",", 1);                                                      \
        r = sname##__json_run(j, r, &v->field[i_]);                                                \
    }                                                                                              \
    r = ioxd__json_up(j, r, ']');

/* What IOXD_JSON_STRUCT expands to, below: a struct's run. A run is a cursor into the sink's
 * tail and the end of the room there, passed and returned by value so that both stay in
 * registers. A failed run has a null cursor, and every piece written to it stays failed - the
 * writer is marked failed already. */
typedef struct ioxd__json_run {
    char *p, *end;
} ioxd__json_run;

ioxd__json_run ioxd__json_open (ioxd_json *j);                                 /* the value's comma, and a run at the tail */
ioxd__json_run ioxd__json_room (ioxd_json *j, char *p, size_t need);           /* commit up to p; need bytes of room, the sink flushed for them if it must */
bool           ioxd__json_close(ioxd_json *j, char *p);                        /* commit up to p: the value is written */
ioxd__json_run ioxd__json_quote(ioxd_json *j, ioxd__json_run r, const char *s, size_t n);   /* a string that needs escapes, or is longer than a run takes */
ioxd__json_run ioxd__json_real (ioxd_json *j, ioxd__json_run r, double v, bool as_float);   /* the shortest that reads back the same; nan and inf are null */

#define IOXD__JSON_RUN_MAX 1024                               /* the longest string written in place */

/* need bytes at the cursor: the run as it is when they fit, else more room from the sink. */
static inline ioxd__json_run ioxd__json_need(ioxd_json *j, ioxd__json_run r, size_t need)
{
    if (__builtin_expect((size_t)(r.end - r.p) >= need, 1))
        return r;
    return ioxd__json_room(j, r.p, need);
}

/* Bytes spelled at compile time - a token, a bracket, a literal - in one fixed-size copy. */
static inline ioxd__json_run ioxd__json_lit(ioxd_json *j, ioxd__json_run r, const char *s, size_t n)
{
    r = ioxd__json_need(j, r, n);
    if (!r.p)
        return r;
    memcpy(r.p, s, n);
    r.p += n;
    return r;
}

/* A key: the comma before it (none for the first field: before is 0) and its token. */
static inline ioxd__json_run ioxd__json_key(ioxd_json *j, ioxd__json_run r, char before, const char *token, size_t n)
{
    r = ioxd__json_need(j, r, n + 1);
    if (!r.p)
        return r;
    *r.p = before;
    r.p += before != 0;
    memcpy(r.p, token, n);
    r.p += n;
    return r;
}

/* Into a level and out of it, its bracket written: the nesting limit holds here as it does for
 * ioxd_json_object and ioxd_json_array. A level refused is still counted, so the up that
 * closes it leaves the depth where it was. */
static inline ioxd__json_run ioxd__json_down(ioxd_json *j, ioxd__json_run r, char bracket)
{
    if (j->depth++ == IOXD_JSON_DEPTH) {
        j->failed = true;
        return (ioxd__json_run){ 0 };
    }
    return ioxd__json_lit(j, r, &bracket, 1);
}
static inline ioxd__json_run ioxd__json_up(ioxd_json *j, ioxd__json_run r, char bracket)
{
    j->depth--;
    return ioxd__json_lit(j, r, &bracket, 1);
}

/* A short copy as two fixed-size moves that overlap, not a call into the C library. */
static inline void ioxd__json_copy(char *d, const char *s, size_t n)
{
    if (n > 32) {
        memcpy(d, s, n);
    } else if (n >= 16) {
        memcpy(d, s, 16);
        memcpy(d + n - 16, s + n - 16, 16);
    } else if (n >= 8) {
        memcpy(d, s, 8);
        memcpy(d + n - 8, s + n - 8, 8);
    } else if (n >= 4) {
        memcpy(d, s, 4);
        memcpy(d + n - 4, s + n - 4, 4);
    } else if (n) {
        d[0]     = s[0];
        d[n / 2] = s[n / 2];
        d[n - 1] = s[n - 1];
    }
}

/* The decimal digits of v at p, the end of them returned: written backwards into a few bytes of
 * stack, then moved into place in two fixed-size copies. */
static inline char *ioxd__json_digits(char *p, uint64_t v)
{
    char  tmp[20];
    char *b = tmp + sizeof tmp;
    do {
        *--b = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    size_t n = (size_t)(tmp + sizeof tmp - b);
    ioxd__json_copy(p, b, n);
    return p + n;
}

static inline ioxd__json_run ioxd__json_put_u64(ioxd_json *j, ioxd__json_run r, uint64_t v)
{
    r = ioxd__json_need(j, r, 20);
    if (r.p)
        r.p = ioxd__json_digits(r.p, v);
    return r;
}

static inline ioxd__json_run ioxd__json_put_i64(ioxd_json *j, ioxd__json_run r, int64_t v)
{
    r = ioxd__json_need(j, r, 20);
    if (!r.p)
        return r;
    if (v < 0) {
        *r.p++ = '-';
        r.p    = ioxd__json_digits(r.p, (uint64_t)(-(v + 1)) + 1);         /* INT64_MIN has no positive twin */
    } else {
        r.p = ioxd__json_digits(r.p, (uint64_t)v);
    }
    return r;
}

static inline ioxd__json_run ioxd__json_put_bool(ioxd_json *j, ioxd__json_run r, bool v)
{
    return v ? ioxd__json_lit(j, r, "true", 4) : ioxd__json_lit(j, r, "false", 5);
}

static inline ioxd__json_run ioxd__json_put_f64(ioxd_json *j, ioxd__json_run r, double v) { return ioxd__json_real(j, r, v, false); }
static inline ioxd__json_run ioxd__json_put_f32(ioxd_json *j, ioxd__json_run r, float v)  { return ioxd__json_real(j, r, (double)v, true); }

/* The top bit of each byte of w that a string must escape - below 0x20, '"' or '\' - and nothing
 * when there is none: a borrow can only mark a byte above one that matched. */
static inline uint64_t ioxd__json_escapes(uint64_t w)
{
    const uint64_t ones = 0x0101010101010101U, highs = 0x8080808080808080U;
    uint64_t       q = w ^ (ones * '"'), b = w ^ (ones * '\\');
    return ((w - ones * 0x20) & ~w & highs) | ((q - ones) & ~q & highs) | ((b - ones) & ~b & highs);
}

/* Nothing in s to escape: eight bytes a step, the last step overlapping the one before it, and a
 * short string read in one or two loads - no loop whose exit has to be guessed. */
static inline bool ioxd__json_clean(const char *s, size_t n)
{
    uint64_t w, found = 0;
    if (n >= 8) {
        for (size_t i = 0; i + 8 <= n; i += 8) {
            memcpy(&w, s + i, 8);
            found |= ioxd__json_escapes(w);
        }
        memcpy(&w, s + n - 8, 8);
        return !(found | ioxd__json_escapes(w));
    }
    if (n >= 4) {
        uint32_t lo, hi;
        memcpy(&lo, s, 4);
        memcpy(&hi, s + n - 4, 4);
        return !ioxd__json_escapes((uint64_t)lo | (uint64_t)hi << 32);
    }
    w = 0x4141414141414141U;                                    /* 'A' wherever no byte lands */
    if (n)
        w = (w & ~(uint64_t)0xFFFFFF) | (uint64_t)(unsigned char)s[0] | (uint64_t)(unsigned char)s[n / 2] << 8
          | (uint64_t)(unsigned char)s[n - 1] << 16;
    return !ioxd__json_escapes(w);
}

/* A string: in place, quoted, when it needs no escape and a run can take it; otherwise through
 * the escaping path, which streams it. */
static inline ioxd__json_run ioxd__json_put_string(ioxd_json *j, ioxd__json_run r, const char *s, size_t n)
{
    if (n > IOXD__JSON_RUN_MAX || !ioxd__json_clean(s, n))
        return ioxd__json_quote(j, r, s, n);
    r = ioxd__json_need(j, r, n + 2);
    if (!r.p)
        return r;
    r.p[0] = '"';
    ioxd__json_copy(r.p + 1, s, n);
    r.p[n + 1] = '"';
    r.p += n + 2;
    return r;
}
static inline ioxd__json_run ioxd__json_put_cstr (ioxd_json *j, ioxd__json_run r, const char *s) { return s ? ioxd__json_put_string(j, r, s, strlen(s)) : ioxd__json_lit(j, r, "null", 4); }
static inline ioxd__json_run ioxd__json_put_slice(ioxd_json *j, ioxd__json_run r, ioxd_slice s)  { return ioxd__json_put_string(j, r, s.p, s.len); }

/* A value into a run by its C type: the same choice IOXD_JSON_VALUE makes. */
#define IOXD__JSON_PUT(j, r, x) _Generic((x),                                                      \
        bool: ioxd__json_put_bool,                                                                 \
        char: ioxd__json_put_i64, signed char: ioxd__json_put_i64, short: ioxd__json_put_i64,      \
        int: ioxd__json_put_i64, long: ioxd__json_put_i64, long long: ioxd__json_put_i64,          \
        unsigned char: ioxd__json_put_u64, unsigned short: ioxd__json_put_u64,                     \
        unsigned: ioxd__json_put_u64, unsigned long: ioxd__json_put_u64,                           \
        unsigned long long: ioxd__json_put_u64,                                                    \
        float: ioxd__json_put_f32, double: ioxd__json_put_f64,                                     \
        char *: ioxd__json_put_cstr, const char *: ioxd__json_put_cstr,                            \
        ioxd_slice: ioxd__json_put_slice)((j), (r), (x))
