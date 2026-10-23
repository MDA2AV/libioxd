/*
 * http/compress.h - the notes of compress.c, whose declarations are public (ioxd/compress.h)
 */
#pragma once

/* ── compress.c: the notes ─────────────────────────────────────────────────────────────── */

/*
 * compress.c - the compression middleware on the engine's two hooks. Its pre-phase does one
 * thing: when the request carries Accept-Encoding, it registers the delegate for the head.
 * That delegate runs at the first flush, with the status, the type, the headers and the size
 * known, decides, and installs the reply's filter; the engine then runs every flushed span
 * through it. gzip for a body that fit the slab is libdeflate's when the build has it: one call,
 * the worker's compressor, nothing held for the reply. Everything else is a coder from this
 * worker's pool: a brotli encoder over an arena - brotli has no reset, so an instance is made per
 * reply, and the arena makes its allocations pointer bumps and its frees no-ops - or a zlib
 * stream, reset per reply. The engine ends a filter as soon as its FINISH has drained it, before
 * the coded bytes are sent, so a coder goes back to the pool while its reply is still on the
 * way, and a worker keeps one per reply it is streaming rather than one per reply in flight.
 */

/* at file scope:
 *   - the codings, in the order they are preferred  [enum kind { KIND_BR, KIND_GZIP, KINDS };]
 *   - a coder: one reply's encoder state, pooled per worker  [struct coder {]
 *   - the arena a brotli instance allocates from: what fits is a bump, the rest malloc, and the
 *     peak is remembered so the arena is grown for next time  [char *arena;]
 *   - the pools, per worker: a free list per coding  [static _Thread_local struct coder
 *     *tl_free[KINDS];]
 *   - libdeflate's compressor, per worker, made at the configured level the first time a whole
 *     body is coded: a call keeps nothing, so there is nothing to pool  [static _Thread_local
 *     struct libdeflate_compressor *tl_deflate;]
 */

/* arena_alloc / arena_free:
 * brotli's allocator: bump what fits, malloc what does not; free only what was malloc'd.
 */

/* coder_take / coder_give:
 * A coder from the pool or fresh, ready for a reply - brotli made with the size hint, so a
 * whole body gets tables sized for it; zlib reset - and back to the pool when the engine ends
 * the filter.
 */

/* br_run / gzip_run:
 * The filter: one call of the encoder with the engine's op, the counts moved to what was
 * consumed and produced, 1 while it holds more output than fit.
 */

/* deflate_whole:
 * The filter for a body that fit the slab: the engine hands it over whole with FINISH, and
 * libdeflate codes it into the coded buffer in one call - which has room for it, the slack past
 * the slab covering a stored block's framing. Anything but FINISH, or a body that did not fit,
 * fails the reply rather than send it cut.
 */

/* code_br / code_gzip:
 * The reply coded one way: the filter installed and Content-Encoding set, or false when this
 * build cannot - no brotli; no gzip for a stream without zlib - so the decision falls to the
 * other coding. gzip takes libdeflate for a whole body and zlib for the rest.
 */

/* gzip_first:
 * Which coding is tried first: the one the client weights higher, else the configured
 * preference - and AUTO is gzip for a whole body when libdeflate codes it, brotli otherwise.
 */

/* compressible:
 * A media type worth coding: text, JSON, XML, JavaScript, SVG, WebAssembly, and any +json or
 * +xml.
 */

/* decide:
 * The delegate for the head: the gates, then the codings in the client's order, the first one
 * this build can code winning, then the filter and the headers.
 */

/* ioxd_compress:
 * The middleware: the delegate when there is an Accept-Encoding, then the chain.
 */

/* ioxd_compress_codings:
 * What this build codes, in the order a tie goes for a whole body - which follows prefer.
 */
