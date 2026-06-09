/* cli_code_audit.h: P4 code-health audit (`aimee code audit`).
 *
 * Walks the working tree and reports file-level health signals (untested files,
 * TODO/FIXME/HACK/XXX markers, DB-in-routes, missing HTTP error handling) with a
 * 0..100 debt score (100 = clean). Normal audits attempt one kb-side graph
 * request to /v1/code/audit (dead exports, import cycles, body-hash clones) and
 * quietly degrade when the server is unavailable. They also write a short,
 * scope-labelled AUDIT_CONTEXT block for ingress pre-injection.
 *
 * The classification/scoring helpers are pure and unit-tested; the tree walk
 * and reporting live in the .c.
 */
#ifndef DEC_CLI_CODE_AUDIT_H
#define DEC_CLI_CODE_AUDIT_H 1

#include <stddef.h>

/* True if `path` looks like a source file we audit (by extension). Pure. */
int audit_is_code_file(const char *path);

/* True if `path` is a test file (…/tests?/…, *.test.*, *_test.*, test_*, spec).
 * Pure. */
int audit_is_test_file(const char *path);

/* Write the comparison stem of `path` (basename minus extension and common
 * test affixes like `_test` / `.test` / `test_`) into `out`. Pure. */
void audit_stem(const char *path, char *out, size_t cap);

/* Count orphaned-work markers (TODO/FIXME/HACK/XXX) in `content`. Pure. */
int audit_count_todos(const char *content);

/* Count route-layer DB access smells in `content` for paths that look like
 * routes/controllers/API handlers. Pure. */
int audit_count_db_in_routes(const char *path, const char *content);

/* Count likely HTTP/fetch calls without obvious nearby error handling. Pure,
 * heuristic, and intentionally conservative. */
int audit_count_unhandled_http(const char *content);

/* Debt score in [0,100] (100 = clean) from the audit tallies. Pure. */
int audit_debt_score(int code_files, int untested, int todo_markers, int db_in_routes,
                     int unhandled_http);

/* `aimee code audit [dir] [--json] [--project NAME] [--graph] [--fix]` entry. */
int handle_code_audit(int argc, char **argv, int json_output);

#endif /* DEC_CLI_CODE_AUDIT_H */
