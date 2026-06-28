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
                                    /* cf.vendored = 0: a vendored caller file is the
                                     * dep's code, not the host's (recall §3). */
                                    "WHERE ci.project <> pc.name AND cf.vendored = 0 AND ("
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

/* H5 generated/build-header reject (H4 live finding): a small seed set of headers
 * that are per-project BUILD-GENERATED (CMake configure_file etc.), never a
 * cross-repo API header. They collide by basename across repos (each generates its
 * own) but the generated copy is often not even indexed, so neither prefer-local
 * nor the §2 IDF catches them — wolf→Sunshine survived H1–H3b via `config.h`.
 *
 * NOTGEN(n) rejects ONLY the BARE include `#include "config.h"` (an exact equality,
 * no LIKE) — deliberately NOT the path-qualified `foo/config.h`, which is a
 * namespaced header far more likely to be a real API (a project that genuinely
 * exports a config header almost always namespaces it). Exact-equality also sidesteps
 * LIKE-metachar escaping. Tradeoff (accepted, precision-over-recall): a project that
 * exports a literally-bare `config.h`/`version.h` as cross-repo API is suppressed;
 * that convention is vanishingly rare, and the principled fix is marker-based
 * generated-output attribution (§1.6, a later slice) rather than a basename list. */
#define NOTGEN(n) "imp.name <> '" n "'"

/* import_header (MEDIUM): a caller C/C++ #include matched to a definer file whose
 * path equals the include or ends with '/'+include (component boundary). The
 * caller file's language (H0b) restricts this to C/C++ so module specifiers from
 * other languages don't masquerade as headers; the definer file must not be
 * vendored (H0b); the include must clear the §2 header-IDF threshold; it must not
 * be a generated/build header (NOTGEN); and — H5 prefer-local (H4 finding) — the
 * CALLER repo must NOT have its OWN file matching the include (a `#include "x.h"`
 * where the caller has its own x.h resolves LOCALLY, not cross-repo: this killed
 * the moonlight-qt→Sunshine FPs via the caller's own cuda.h/input.h/vaapi.h). The
 * repo-unique HIGH-vs-MEDIUM is applied by H1's resolver (raw candidate adjacency). */
/* clang-format off — hand-formatted SQL (the ESC()/NOTGEN() macros embedded in the
 * string concatenation defeat clang-format's literal reflow, mangling it to one
 * token per line). */
/* clang-format off */
static const char *const SQL_HEADER_ROUTES =
    "INSERT INTO cross_repo_route (caller_project, definer_project, kind, confidence, evidence) "
    "SELECT DISTINCT pc.name, pd.name, 'import_header', 'medium', imp.name "
    "FROM file_imports imp "
    "JOIN files cf ON cf.id = imp.file_id "
    "JOIN projects pc ON pc.id = cf.project_id "
    "JOIN files fd ON (fd.path = imp.name OR fd.path LIKE '%/' || " ESC("imp.name") " ESCAPE '\\') "
    "JOIN projects pd ON pd.id = fd.project_id "
    /* cf.vendored = 0: a VENDORED caller file (e.g. a monorepo's subprojects/ or a
     * fetched _deps/ tree) is the dep's code, not the host repo's — its #includes
     * must not generate routes attributed to the host (recall §3, mirrors the
     * definer-side fd.vendored = 0 and the originated-vendored exclusion). */
    "WHERE cf.language IN ('c', 'cpp') AND cf.vendored = 0 AND fd.vendored = 0 "
    "  AND pd.name <> pc.name "
    /* §2 header IDF: drop ubiquitous (>=4 non-vendored repos) include specifiers. */
    "  AND (SELECT COUNT(DISTINCT fx.project_id) FROM files fx "
    "       WHERE (fx.path = imp.name OR fx.path LIKE '%/' || " ESC("imp.name") " ESCAPE '\\') "
    "         AND fx.vendored = 0) < 4 "
    /* H5 generated/build-header reject. */
    "  AND " NOTGEN("config.h") " AND " NOTGEN("config.hpp") " "
    "  AND " NOTGEN("version.h") " AND " NOTGEN("version.hpp") " "
    /* H5 prefer-local — applies ONLY to QUOTED includes (imp.is_system = 0). Skip
     * when the caller repo has its OWN non-vendored file for the include: a quoted
     * #include resolves to the including file's directory first (C quote-include
     * locality), so a caller-owned copy means local, not cross-repo, resolution. An
     * ANGLE include `<...>` (H6, is_system = 1) does NOT resolve to the caller's own
     * dir, so prefer-local must NOT suppress it (else a real <lib.h> dep would be
     * dropped when the caller happens to have a same-named file). fl.vendored = 0 is
     * deliberate: a VENDORED caller copy is NOT treated as local, so a caller that
     * vendors a lib still routes to the canonical definer (H2 then prefers it). */
    "  AND (imp.is_system = 1 OR NOT EXISTS (SELECT 1 FROM files fl "
    "       WHERE fl.project_id = cf.project_id AND fl.vendored = 0 "
    "         AND (fl.path = imp.name OR fl.path LIKE '%/' || " ESC("imp.name") " ESCAPE '\\'))) "
    "ON CONFLICT (caller_project, definer_project, kind, evidence) DO NOTHING";
/* clang-format on */

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
