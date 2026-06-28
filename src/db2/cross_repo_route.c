/* cross_repo_route.c: the inter-repo structural-edge route index (H0d). Builds
 * cross_repo_route from file_imports + cross_repo_identity + files at index time;
 * H1 consumes it as the hard structural-edge prerequisite. See cross_repo_route.h
 * and docs/proposals/pending/cross-repo-precision-hardening.md §1. */

#include "cross_repo_route.h"

#include "aimee.h"
#include "db2.h"
#include "db_postgres.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

#define CRR_LOG_TAG "cross_repo.route"
#define CRR_ERRBUF  256

/* SQL expression escaping LIKE metacharacters in column `c` (backslash first,
 * then % and _) so a value like 'foo_bar.h' is matched literally, not as a
 * wildcard. Pair with ESCAPE '\'. Portable across Postgres + the sqlite shim. */
#define ESC(c) "REPLACE(REPLACE(REPLACE(" c ", '\\', '\\\\'), '%', '\\%'), '_', '\\_')"

/* import_module (HIGH): a caller's import/use/require specifier matched to a
 * definer's provided identity — exact, or the identity as a path/namespace prefix
 * (go.mod subpaths, rust `use crate::a::b`). Restricted to module-language callers
 * AND the matching identity kind (go->gomod, rust->crate, js/ts->npm, py->pypi) so
 * a specifier can't match an unrelated ecosystem's identity. Identities already
 * exclude vendored manifests (H0c), so the definer is first-party. */
static const char *const SQL_MODULE_ROUTES =
    "INSERT INTO cross_repo_route (caller_project, definer_project, kind, confidence, evidence) "
    "SELECT DISTINCT pc.name, ci.project, 'import_module', 'high', imp.name "
    "FROM file_imports imp "
    "JOIN files cf ON cf.id = imp.file_id "
    "JOIN projects pc ON pc.id = cf.project_id "
    "JOIN cross_repo_identity ci ON (imp.name = ci.value "
    "  OR imp.name LIKE " ESC(
        "ci.value") " || '/%' ESCAPE '\\' "
                    "  OR imp.name LIKE " ESC(
                        "ci.value") " || '::%' ESCAPE '\\') "
                                    "WHERE ci.project <> pc.name AND ("
                                    "  (cf.language = 'go' AND ci.kind = 'gomod') "
                                    "  OR (cf.language = 'rust' AND ci.kind = 'crate') "
                                    "  OR (cf.language IN ('js', 'ts') AND ci.kind = 'npm') "
                                    "  OR (cf.language = 'python' AND ci.kind = 'pypi')) "
                                    "ON CONFLICT (caller_project, definer_project, kind, evidence) "
                                    "DO NOTHING";

/* §2 header IDF (H3b): a quoted #include that resolves to non-vendored files in
 * >= 4 distinct repos is non-distinctive — a ubiquitous name (config.h, common.h,
 * log.h) where the include almost always means the caller's OWN copy, not a
 * cross-repo dependency. Such includes produce no confident header route (they
 * would otherwise fan a spurious route to every repo that shares the name).
 *
 * Ubiquity is measured on the SAME key the route join uses — the include-specifier
 * path-suffix (`path = imp.name OR path LIKE '%/'||imp.name`) — NOT a bare
 * basename, deliberately: keying on the route's own match means the count reflects
 * the route's actual fan-out. For the common bare `#include "config.h"` this is
 * exactly basename IDF; for a directory-qualified `#include "foo/config.h"` it is
 * the (more specific, lower-fan-out) component-suffix, which is the correct, more
 * recall-preserving measure — counting bare-basename ubiquity there would wrongly
 * exclude a specific path that routes to only one repo.
 *
 * Note: angle-bracket / system <...> includes are ALREADY excluded upstream — the
 * C extractor (c_import_line) records ONLY quoted #include "..." into file_imports
 * — so §2's system-include signal needs no work here. The threshold is the literal
 * `< 4` in SQL_HEADER_ROUTES below (a candidate for config-ization + tuning after
 * H4's live re-validation). */

/* import_header (MEDIUM): a caller C/C++ #include matched to a definer file whose
 * path equals the include or ends with '/'+include (component boundary). The
 * caller file's language (H0b) restricts this to C/C++ so module specifiers from
 * other languages don't masquerade as headers; the definer file must not be
 * vendored (H0b); and the include must clear the §2 header-IDF threshold above
 * (ubiquity counted on the same path-suffix key, vendored files excluded from the
 * count so a vendored copy never inflates ubiquity). The repo-unique HIGH-vs-MEDIUM
 * is applied by H1's resolver (this is the raw candidate adjacency). */
static const char *const SQL_HEADER_ROUTES =
    "INSERT INTO cross_repo_route (caller_project, definer_project, kind, confidence, evidence) "
    "SELECT DISTINCT pc.name, pd.name, 'import_header', 'medium', imp.name "
    "FROM file_imports imp "
    "JOIN files cf ON cf.id = imp.file_id "
    "JOIN projects pc ON pc.id = cf.project_id "
    "JOIN files fd ON (fd.path = imp.name "
    "  OR fd.path LIKE '%/' || " ESC(
        "imp.name") " ESCAPE '\\') "
                    "JOIN projects pd ON pd.id = fd.project_id "
                    "WHERE cf.language IN ('c', 'cpp') AND fd.vendored = 0 AND pd.name <> pc.name "
                    "  AND (SELECT COUNT(DISTINCT fx.project_id) FROM files fx "
                    "       WHERE (fx.path = imp.name OR fx.path LIKE '%/' || " ESC(
                        "imp.name") " ESCAPE '\\') "
                                    "         AND fx.vendored = 0) < 4 "
                                    "ON CONFLICT (caller_project, definer_project, kind, evidence) "
                                    "DO NOTHING";

static int crr_count(void *conn)
{
   char err[CRR_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT COUNT(*) FROM cross_repo_route", err, sizeof(err));
   if (!st)
      return -1;
   int n = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      n = (int)aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}

int db2_cross_repo_rebuild_routes(void)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[CRR_ERRBUF] = "";

   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;

   int ok = (aimee_pg_exec(conn, "DELETE FROM cross_repo_route", err, sizeof(err)) == 0);
   if (ok && aimee_pg_exec(conn, SQL_MODULE_ROUTES, err, sizeof(err)) != 0)
      ok = 0;
   if (ok && aimee_pg_exec(conn, SQL_HEADER_ROUTES, err, sizeof(err)) != 0)
      ok = 0;

   if (!ok)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      LOG_ERROR(CRR_LOG_TAG, "rebuild routes failed: %s", err);
      return -1;
   }
   int n = crr_count(conn);
   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
      return -1;
   return n;
}
