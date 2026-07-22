/* code_outline.h: agent-shaped, anchored views of source structure.
 * (proposal: hashline-edit-and-lean-websearch, Part III — read_file outline,
 *  read_symbol.)
 *
 * Pure over (content, ext): reuses the code index's tree-sitter/hand-rolled
 * definition extractor and Part I line anchors. No I/O — the caller reads the
 * file and mints the snapshot whose id is threaded through so every row is a
 * ready edit anchor. */
#ifndef AIMEE_CODE_OUTLINE_H
#define AIMEE_CODE_OUTLINE_H

#include "index.h" /* definition_t */
#include <stddef.h>

/* File extension (WITH the dot, e.g. ".c") of `path`, or "" if none. */
const char *code_outline_ext(const char *path);

/* Render a symbol skeleton: one "LINE:HASH| <kind> <name>  (lines A-B)" row per
 * top-level definition, bodies omitted, each anchored on its start line so the
 * agent can read/edit one span without paging the file. `snapshot_id` (may be
 * NULL) is echoed in a header. Returns a malloc'd string (caller frees), or NULL
 * on OOM. When no extractor matches the extension, returns a short note. */
char *code_outline_format(const char *content, size_t len, const char *ext,
                          const char *snapshot_id);

/* Collect definitions named `symbol` (exact match) into `out` (<= max). Returns
 * the match count (0 = not found), or -1 on error. */
int code_outline_symbol_defs(const char *content, size_t len, const char *ext, const char *symbol,
                             definition_t *out, int max);

#endif /* AIMEE_CODE_OUTLINE_H */
