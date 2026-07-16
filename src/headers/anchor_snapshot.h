/* anchor_snapshot.h: hashline edit core — stable server-side line anchors.
 *
 * (proposal: hashline-edit-and-lean-websearch, Part I.)
 *
 * A "hashline" anchor is the pair (line-ordinal, content-hash) rendered
 * "12:f1". The ordinal is the primary key and by construction disambiguates
 * identical lines; the hash only *verifies* the ordinal still points at the
 * bytes the model read. Correctness rides on a full-length server-side digest
 * (FNV-1a 64-bit over the line's CANONICAL bytes), never the 2-hex display tag.
 *
 * Each anchored read mints an immutable snapshot (snapshot_id) recording every
 * line's full digest plus the file's line-ending / BOM / trailing-newline
 * shape. Edits carry the snapshot_id; verification runs against that snapshot's
 * digests. Concurrent reads mint independent snapshots and never clobber each
 * other. Snapshots are TTL/LRU-evicted. */
#ifndef AIMEE_ANCHOR_SNAPSHOT_H
#define AIMEE_ANCHOR_SNAPSHOT_H

#include "aimee.h" /* MAX_PATH_LEN */
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define ANCHOR_SNAPSHOT_ID_MAX 40

/* One raw line slice into a file buffer. `content_len` excludes the terminator
 * (the trailing "\n" or "\r\n"); `len` includes it. The last line of a file
 * with no trailing newline has len == content_len. */
typedef struct
{
   const char *ptr;    /* start of the line within the source buffer */
   size_t len;         /* bytes including terminator */
   size_t content_len; /* bytes excluding terminator */
} anchor_line_t;

/* An immutable read snapshot. `line_digests[n-1]` is the full canonical digest
 * of 1-based line n. Copies returned by anchor_snapshot_get_copy own their
 * `line_digests` and must be released with anchor_snapshot_dispose(). */
typedef struct
{
   char id[ANCHOR_SNAPSHOT_ID_MAX];
   char path[MAX_PATH_LEN]; /* absolute path as read */
   uint64_t file_hash;      /* digest over the per-line digests (whole-file identity) */
   uint64_t *line_digests;  /* [line_count], ordinal n -> [n-1] */
   int line_count;
   char eol[3];          /* dominant terminator: "\n" or "\r\n" */
   int had_bom;          /* file began with a UTF-8 BOM */
   int no_final_newline; /* last line had no terminator */
   time_t created;
} anchor_snapshot_t;

/* Canonicalize + hash one raw line (may include its terminator). The trailing
 * "\r"/"\n" is not hashed; when is_first_line, a leading UTF-8 BOM is stripped
 * before hashing. Deterministic — server and client compute identical anchors. */
uint64_t anchor_line_digest(const char *line, size_t len, int is_first_line);

/* Render the 2-hex display tag of a full digest into out (3 bytes incl. NUL). */
void anchor_short_tag(uint64_t digest, char out[3]);

/* Split `bytes` into lines. Returns the line count and, via *out, a malloc'd
 * array the caller frees. Each entry points into `bytes` (no copy). A trailing
 * newline does NOT produce an empty final line. Returns 0 with *out=NULL for an
 * empty buffer. Returns -1 on allocation failure. */
int anchor_split_lines(const char *bytes, size_t len, anchor_line_t **out);

/* Detect the file's dominant terminator over `bytes`: the strict majority of
 * CRLF vs bare-LF line endings; on a tie, or 0-1 lines, "\n". Writes "\n" or
 * "\r\n" into eol_out (>=3 bytes). Sets *had_bom / *no_final_newline. */
void anchor_detect_shape(const char *bytes, size_t len, char eol_out[3], int *had_bom,
                         int *no_final_newline);

/* Mint and store an immutable snapshot for `bytes` (the current contents of
 * abs_path). Writes the new id into id_out (>= ANCHOR_SNAPSHOT_ID_MAX).
 * Returns 0 on success, -1 on failure. */
int anchor_snapshot_create(const char *abs_path, const char *bytes, size_t len,
                           char id_out[ANCHOR_SNAPSHOT_ID_MAX]);

/* Copy a live, non-expired snapshot into *out (dup'ing line_digests). Returns 1
 * if found and fresh, 0 otherwise. Release with anchor_snapshot_dispose(). */
int anchor_snapshot_get_copy(const char *id, anchor_snapshot_t *out);

/* Free the line_digests of a copy from anchor_snapshot_get_copy(). */
void anchor_snapshot_dispose(anchor_snapshot_t *out);

/* Format an anchored read of `bytes` over the 1-based [offset+1 .. offset+limit]
 * window (offset<=0 => from line 1; limit<=0 => to EOF), each line prefixed
 * "LINE:HH| ". `snapshot_id` (may be NULL) is echoed in a leading header line so
 * the model can cite it in edits. Returns a malloc'd string (caller frees) or
 * NULL on allocation failure. */
char *anchor_format_read(const char *bytes, size_t len, int offset, int limit,
                         const char *snapshot_id);

/* Parse an anchor token "LINE:HEX" (e.g. "12:f1"). On success writes the ordinal
 * to *ordinal and the display tag byte to *tag and returns 0; returns -1 if the
 * token is malformed. */
int anchor_parse(const char *token, int *ordinal, unsigned *tag);

/* Drop all snapshots older than the TTL (called opportunistically). */
void anchor_snapshot_gc(void);

#endif /* AIMEE_ANCHOR_SNAPSHOT_H */
