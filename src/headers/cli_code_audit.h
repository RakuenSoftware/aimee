/* cli_code_audit.h: P4 code-health audit (`aimee code audit`).
 *
 * A self-contained client command that walks the working tree and reports
 * file-level health signals — untested code files (stem-matched against test
 * files) and orphaned TODO/FIXME/HACK/XXX markers — with a 0..100 debt score
 * (100 = clean). It runs locally over the repo and needs no kb/server.
 *
 * This is the self-contained slice of docs/proposals/pending/code-health-audit.md;
 * the graph-derived checks there (dead exports, import cycles, body-hash clones)
 * need new kb-side graph queries + a body_hash index column and remain a kb
 * follow-on.
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

/* Debt score in [0,100] (100 = clean) from the audit tallies. Pure. */
int audit_debt_score(int code_files, int untested, int todo_markers);

/* `aimee code audit [dir] [--json]` entry. Walks `dir` (default cwd), runs the
 * file-level checks, prints a report (or JSON), and returns 0. */
int handle_code_audit(int argc, char **argv, int json_output);

#endif /* DEC_CLI_CODE_AUDIT_H */
