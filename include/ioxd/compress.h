/*
 * ioxd/compress.h - response compression, as middleware: a reply of a compressible type to a
 * client whose Accept-Encoding takes br or gzip goes out coded, brotli, libdeflate or zlib doing
 * the work at the engine's flush. A body that fit the slab is one call and one message with the
 * exact coded length; one that streams is coded flush by flush, chunked.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "ioxd/http.h"

/* The middleware: on the root (IOXD_USE), a group, or listed after an endpoint's handler. It
 * does nothing to a request without Accept-Encoding, to a reply that is not 2xx, that already
 * carries a Content-Encoding, whose type is not text, JSON, XML, JavaScript, SVG or WebAssembly,
 * or whose whole body is smaller than min_bytes. Otherwise it codes with what the client weights
 * highest of what this build has, a tie going the way prefer says, and sets Content-Encoding;
 * Vary tells caches either way. gzip for a body that fit the slab is libdeflate's, in one call,
 * when the build has it, and zlib's otherwise, as it is for a body that streams. Built without
 * some of them (make BROTLI=0, ZLIB=0, LIBDEFLATE=0, or pkg-config not finding them), it is the
 * middleware that compresses with what it has, or nothing. */
void ioxd_compress(ioxd_ctx *ctx, ioxd_next *next);

/* Which coding takes a reply the client weights br and gzip the same for. AUTO is gzip for a
 * body that fit the slab when libdeflate is built in - at the default levels its gzip is both
 * smaller and cheaper than brotli's, measured on JSON, CSS, JavaScript and SVG - and br
 * otherwise, which is also what a body that streams gets: zlib streams gzip, and loses to brotli
 * there. */
typedef enum ioxd_compress_prefer {
    IOXD_PREFER_AUTO,
    IOXD_PREFER_BR,
    IOXD_PREFER_GZIP,
} ioxd_compress_prefer;

/* How hard to compress, process-wide. Zero is the default of every field. Before the run. */
typedef struct ioxd_compress_config {
    int    brotli_quality;   /* 1..11; 1 unless set: fast, and still four to eight times smaller on JSON (zero is unset, so 0 is not on offer) */
    int    brotli_window;    /* lgwin 10..24; 22 unless set                                             */
    int    gzip_level;       /* 1..9; 1 unless set                                                      */
    size_t min_bytes;        /* a whole body smaller than this goes out as it is; 256 unless set        */
    ioxd_compress_prefer prefer;   /* who takes a tie; IOXD_PREFER_AUTO unless set                     */
} ioxd_compress_config;
int ioxd_compress_configure(const ioxd_compress_config *config);   /* -1, with the reason on stderr, when a value is out of range */

/* Which codings this build compresses with, the one a tie goes to for a body that fit the slab
 * first: "gzip, br", "br, gzip", "br", "gzip" or "". */
const char *ioxd_compress_codings(void);
