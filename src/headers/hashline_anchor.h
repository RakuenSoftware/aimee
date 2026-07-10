/* hashline_anchor.h: composite line anchors + immutable read snapshots.
 *
 * The edit harness identifies lines by a composite anchor `ordinal:tag` where the
 * ordinal is the 1-based line number (the primary key that disambiguates
 * identical lines) and the tag is a SHORT, DISPLAY-ONLY hex rendering of the
 * line's full 64-bit content digest. Correctness never rides on the short tag:
 * edit-time verification always compares the FULL 64-bit digest recorded in an
 * immutable read snapshot (see below), never the truncated display tag.
 *
 * A read mints a snapshot: an opaque, per-read unique identity that records the
 * whole file's content digest plus a per-line full-digest vector. Edits carry
 * the snapshot_id and are verified against THAT snapshot, so a concurrent read of
 * the same file cannot clobber the digests an in-flight edit relies on. Missing
 * or evicted snapshots yield a "re-read" signal to the caller, never a blind
 * apply.
 *
 * This is a LEAF module: it depends only on libc + pthread + sketch (FNV-1a) +
 * util + platform_random. It must not pull in server/kb/tree-sitter surfaces so
 * it links cleanly under every tool surface. (pthread is used for the snapshot
 * store mutex; all supported server/delegate targets are pthread-capable.) See
 * docs/proposals — hashline edit core.
 */
#ifndef DEC_HASHLINE_ANCHOR_H
#define DEC_HASHLINE_ANCHOR_H

#include <stddef.h>
#include <stdint.h>

/* Display tag length in hex chars. DISPLAY-ONLY and non-authoritative; pinned to
 * a constant so tool output is stable. Verification uses the full 64-bit digest. */
#define HASHLINE_DISPLAY_TAG_HEX 3

/* Canonicalize one line's bytes for hashing. `line`/`len` are the line's content
 * WITHOUT its '\n' terminator; `had_terminator` is 1 if a '\n' followed this line
 * in the file (so a trailing '\r' is the CR of a CRLF terminator). Returns a
 * (ptr,len) VIEW into the input buffer (no copy) via out_ptr and out_len; the
 * caller must keep `line` alive while using the view. Rules (fixed + documented
 * so server and any client compute identical anchors):
 *   - a trailing '\r' is dropped ONLY when had_terminator (CRLF <-> LF
 *     equivalence); a bare '\r' at end-of-file (no terminator) is real content
 *     and is hashed verbatim, so `foo` and `foo\r` do NOT collide;
 *   - if is_first_line and the buffer opens with a UTF-8 BOM (EF BB BF), the BOM
 *     is skipped (BOM-like bytes elsewhere are hashed verbatim);
 *   - all remaining bytes are hashed verbatim (trailing whitespace IS an edit). */
void hashline_canonicalize_line(const char *line, size_t len, int is_first_line, int had_terminator,
                                const char **out_ptr, size_t *out_len);

/* Full 64-bit content digest of a line's CANONICAL bytes. See
 * hashline_canonicalize_line for `is_first_line` / `had_terminator`. */
uint64_t hashline_digest64(const char *line, size_t len, int is_first_line, int had_terminator);

/* Full 64-bit digest of an arbitrary byte range (used for the whole-file digest). */
uint64_t hashline_digest64_raw(const void *data, size_t len);

/* Render the display tag (HASHLINE_DISPLAY_TAG_HEX chars + NUL) for a full
 * digest. buf must have room for HASHLINE_DISPLAY_TAG_HEX + 1 bytes. */
void hashline_display_tag(uint64_t digest, char *buf, size_t buf_len);

/* Count the newline-delimited lines in a buffer. A trailing segment with no
 * final '\n' still counts as a line; an empty buffer has 0 lines. */
size_t hashline_line_count(const char *content, size_t len);

/* Compute the per-line full-digest vector for a whole file buffer. Returns the
 * TOTAL line count and writes min(total, max) digests into `out`. Callers that
 * need every digest must size `out` to hashline_line_count() first.
 * Canonicalization is applied per line (BOM only on line 1). */
size_t hashline_line_digests(const char *content, size_t len, uint64_t *out, size_t max);

/* ---- Immutable read-snapshot store (process-local, session-aware, TTL/LRU) ---- */

/* A CALLER-OWNED deep copy of a snapshot's recorded state (no borrowed pointers
 * into the evictable store — safe to use after the store mutates concurrently).
 * Release with hashline_snapshot_view_free when done. */
typedef struct
{
   char *path;             /* file path as read (owned) */
   uint64_t file_digest;   /* digest of the WHOLE file content at read time */
   size_t size;            /* file byte length at read time */
   size_t line_count;      /* number of lines covered */
   uint64_t *line_digests; /* line_count full digests, ordinal-indexed [0..n) (owned) */
   int64_t read_time_ms;   /* mint time (monotonic ms) */
} hashline_snapshot_view_t;

/* Mint a snapshot over the WHOLE file content (coverage is all lines, independent
 * of any offset/limit the caller applied to its emitted window). Returns a newly
 * allocated opaque snapshot_id (caller frees) or NULL on OOM (store left
 * unchanged on OOM). `sid` scopes the snapshot to a session (may be NULL for
 * unscoped/internal use). Ids are guaranteed unique (monotonic-seq + CSPRNG), so
 * N concurrent mints of identical content return N DISTINCT, independently
 * retrievable ids. */
char *hashline_snapshot_mint(const char *sid, const char *path, const char *content, size_t len);

/* Fetch a snapshot by id, DEEP-COPYING its state into caller-owned *out. If `sid`
 * is non-NULL it must match the minting session (NULL is a wildcard reserved for
 * internal/unscoped callers — the model-facing edit surface always passes the
 * authenticated dispatch sid). Returns 1 on a live hit (caller must
 * hashline_snapshot_view_free(out)); 0 on miss/eviction/expiry/OOM. */
int hashline_snapshot_get(const char *sid, const char *snapshot_id, hashline_snapshot_view_t *out);

/* Free the owned buffers in a view filled by hashline_snapshot_get and zero it.
 * Safe on an all-zero view. */
void hashline_snapshot_view_free(hashline_snapshot_view_t *out);

/* Test/maintenance hooks. */
void hashline_snapshot_evict_all(void);            /* drop all snapshots */
void hashline_snapshot_set_ttl_ms(int64_t ttl_ms); /* override TTL (<=0 restores default) */

#endif /* DEC_HASHLINE_ANCHOR_H */
