/* code_span.h: server-side bounded source-span resolver for the lossy-code-fold
 * recovery path (ingress-compression P2 / proposal §3.1, §3.5, §6.5 B4).
 *
 * A lossy code fold (§1.2 P1b) replaces an inline snippet with a compact
 * signature + a `file:line-range` reference; the model recovers the full span on
 * demand through the `code_span_get` MCP tool, which calls code_span_read().
 *
 * Safety (B4, fail-closed): the model-supplied path is joined to the project's
 * indexed root and realpath-validated (no `..` / symlink escape, sensitive-path
 * deny-list), the result must stay within the project root, control chars are
 * rejected, and the line span is clamped to `max_lines`. The bytes are pulled
 * through the active workspace provider (shared/detached/mirror), never by
 * opening the indexed path directly. The returned `source_version` (sha256 of the
 * returned span bytes) is the drift baseline a later re-read compares against
 * (§1.1, B3/B7).
 */
#ifndef DEC_CODE_SPAN_H
#define DEC_CODE_SPAN_H 1

#include "cJSON.h"

/* Read [line_start, line_end] (1-based, inclusive) of `file_path` resolved within
 * absolute `project_root`. Clamps the span to `max_lines` lines (and a hard byte
 * cap). Reads through workspace_provider_active(). Returns a malloc'd cJSON the
 * caller frees:
 *   success: {project?, file_path, line_start, line_end, line_count, truncated,
 *             content, source_version}
 *   failure: {error: "<reason>"}
 * Returns NULL only on allocation failure. `project_root` must be absolute (the
 * index's recorded root). No MCP/config dependency — testable with a temp dir as
 * root and the default shared provider. */
cJSON *code_span_read(const char *project, const char *project_root, const char *file_path,
                      int line_start, int line_end, int max_lines);

#endif /* DEC_CODE_SPAN_H */
