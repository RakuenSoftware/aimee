/* code_collect.h: shared, DB-free source-file collector.
 *
 * Walks a directory tree and gathers indexable, non-binary source files as
 * {"rel_path","content"} cJSON objects — the wire shape the aimee-kb
 * /v1/code/scan handler accepts for push-based indexing. Pure filesystem +
 * cJSON, with no kb_client / DB dependency, so it links into BOTH the engine
 * (server-side local scans) and the thin client (which pushes its own tree to
 * a remote server that cannot see the client filesystem). POSIX only; a no-op
 * returning 0 elsewhere.
 */
#ifndef CODE_COLLECT_H
#define CODE_COLLECT_H

#include "cJSON.h"

/* Upper bounds that keep a single push request sane. */
#define CODE_COLLECT_MAX_FILES      4096
#define CODE_COLLECT_MAX_FILE_BYTES (256 * 1024)

/* Recursively walk `root`, appending one {"rel_path","content"} object per
 * indexable file to `files_arr`. Skips VCS/build/hidden directories and binary
 * or oversized files. Best-effort: per-file errors are silently skipped.
 * Returns the number of files appended (clamped at CODE_COLLECT_MAX_FILES). */
int code_collect_files(const char *root, cJSON *files_arr);

#endif /* CODE_COLLECT_H */
