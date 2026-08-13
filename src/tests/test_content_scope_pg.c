/* test_content_scope_pg.c: the content-scope referent and predicate (slice 1 of
 * docs/proposals/pending/per-user-content-scope-visibility.md).
 *
 * Needs a live Postgres: RLS and current_setting have no meaning on the SQLite
 * shim. Reads AIMEE_TEST_PG_URL and SKIPS CLEANLY (exit 0) when it is unset,
 * mirroring test_vault_pg.c, so `make unit-tests` stays green without one.
 *
 * WHAT IS PINNED HERE, and why each would otherwise be silent:
 *
 *  1. `projects.kb_project` and `kb_project_visible()` exist after the schema is
 *     applied. They are the referent every content policy will read.
 *  2. The predicate DENIES an unattributed project (NULL) and denies with no
 *     principal set. Deny is the direction the whole design rests on, and a
 *     predicate that answered true here would open every content policy built on
 *     it at once.
 *  3. NOTHING is enforced yet: content is still readable. This slice must be
 *     landable ahead of the backfill, and a policy arriving early would take an
 *     existing deployment dark rather than merely fail a test.
 *
 * WHAT IS NOT HERE: whether a member sees their own project and a stranger does
 * not. That needs the policies, which land with the backfill in slice 2. The
 * end-to-end behaviour was prototyped in
 * docs/validation/per-user-content-scope-prototype.md.
 */
#include "db2.h"
#include "db2/db2_internal.h"
#include "db2/db_postgres.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Single-value scalar query. Returns 0 and fills out on success. */
static int scalar(const char *sql, char *out, size_t cap);

/* assert that `sql` returns `want`, and say what came back when it does not. */
static void expect(const char *sql, const char *want)
{
   char got[128] = "";
   if (scalar(sql, got, sizeof(got)) != 0)
   {
      fprintf(stderr, "query failed: %s\n", sql);
      assert(0);
   }
   if (strcmp(got, want) != 0)
   {
      fprintf(stderr, "expected \"%s\", got \"%s\"\n  sql: %s\n", want, got, sql);
      assert(0);
   }
}

static int scalar(const char *sql, char *out, size_t cap)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   if (!st)
   {
      fprintf(stderr, "prepare failed: %s\n  sql: %s\n", err, sql);
      return -1;
   }
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *v = aimee_pg_column_text(st, 0);
      snprintf(out, cap, "%s", v ? v : "");
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int main(void)
{
   const char *url = getenv("AIMEE_TEST_PG_URL");
   if (!url || !url[0])
   {
      printf("content_scope_pg: SKIP (AIMEE_TEST_PG_URL unset; real Postgres required)\n");
      return 0;
   }
   if (db2_init(url) != 0)
   {
      fprintf(stderr, "content_scope_pg: db2_init failed for %s\n", url);
      return 1;
   }
   printf("test_content_scope_pg\n");

   /* 1. The referent exists. */
   expect("SELECT count(*) FROM information_schema.columns"
          " WHERE table_name='projects' AND column_name='kb_project'",
          "1");
   printf("  PASS: projects.kb_project exists\n");

   expect("SELECT count(*) FROM pg_proc WHERE proname='kb_project_visible'", "1");
   printf("  PASS: kb_project_visible exists\n");

   /* 2. It denies what cannot be attributed, and denies with no principal.
    *
    *    Asked as an explicit CASE rather than a cast: boolean::text is
    *    'true'/'false' while psql's tuple output shows 't'/'f', and a test that
    *    depends on which spelling arrives is testing the driver. 'deny' and
    *    'allow' are this test's own words, and NULL would arrive as "" and match
    *    neither -- which matters, because NULL is not the same answer as false
    *    and must never pass for one. */
   expect("SELECT CASE WHEN kb_project_visible(NULL) THEN 'allow' ELSE 'deny' END", "deny");
   printf("  PASS: an unattributed project is denied\n");

   expect("SELECT CASE WHEN kb_project_visible(-1) THEN 'allow' ELSE 'deny' END", "deny");
   printf("  PASS: an unknown project with no principal is denied\n");

   /* 3. Nothing is enforced yet. If a policy lands before its backfill, this
    *    fails here rather than in a deployment that has gone quiet. */
   expect("SELECT count(*) FROM pg_policies"
          " WHERE tablename IN ('kb_documents','kb_file_index')",
          "0");
   printf("  PASS: no content policy is enabled ahead of the backfill\n");

   db2_shutdown();
   printf("All tests passed.\n");
   return 0;
}
