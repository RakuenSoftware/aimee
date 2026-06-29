/* kb_blob_store.h: content-addressed blob store for structured-PDF visual crops
 * (structured-PDF Phase C). Binary crops do not belong inline in the DB; they live as one
 * file per sha256 under a configurable root (default <kb_default_config_dir()>/kb-blobs),
 * sharded by the first two hex chars. Content-addressing gives free dedup across re-ingests.
 *
 * SECURITY: the sha256 is a KB-INTERNAL identifier — it is never returned to a client, never
 * placed in a URL/log/error surfaced outside the perimeter, and the directory is served by no
 * file route. The only client read path is the access-gated open_asset, which resolves an
 * opaque kb_doc_assets row id to a blob_ref internally. */
#ifndef AIMEE_KB_BLOB_STORE_H
#define AIMEE_KB_BLOB_STORE_H

#include <stddef.h>

/* Resolve the blob store root into `out` (cfg->kb_pdf_blob_dir if set, else
 * <kb_default_config_dir()>/kb-blobs). Returns out, or NULL on bad args. Does not create it. */
const char *kb_blob_store_root(char *out, size_t cap);

/* Store `bytes`/`n`, writing the 64-char lowercase sha256 hex into sha_out (>= 65 bytes).
 * Idempotent: a byte-identical blob is stored once (dedup). The blob is fsync-durable and
 * atomically renamed into place BEFORE this returns success, so a crash can only ever leave a
 * harmless orphan temp file, never a half-written blob a row points at. Returns 0 on success
 * (including the already-present dedup case), -1 on error. */
int kb_blob_store_put(const void *bytes, size_t n, char *sha_out, size_t sha_cap);

/* Read the blob named by `sha` fully into a heap buffer (*out / *n_out; caller frees *out).
 * Returns 0 on success, -1 if absent or on error. `sha` must be a 64-char hex string. */
int kb_blob_store_read(const char *sha, void **out, size_t *n_out);

/* 1 if a blob exists, 0 if not, -1 on bad args. */
int kb_blob_store_exists(const char *sha);

/* Unlink a blob (reconciliation). A missing blob is success (idempotent). Returns 0/-1. */
int kb_blob_store_unlink(const char *sha);

/* Iterate every stored blob's sha (64-hex) via the callback; used by the orphan
 * reconciliation sweep. Returns the number visited, or -1 on error. The callback returns 0 to
 * continue, non-zero to stop early. `bytes` is that blob file's size. */
typedef int (*kb_blob_visit_fn)(const char *sha, long long bytes, void *ctx);
long long kb_blob_store_foreach(kb_blob_visit_fn fn, void *ctx);

#endif /* AIMEE_KB_BLOB_STORE_H */
