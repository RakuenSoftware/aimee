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
 *  3. The policies exist but are INERT: RLS is not enabled on either table, so
 *     applying this schema changes what nobody can read. A policy that switched
 *     itself on would turn an upgrade into an outage for every row not yet
 *     attributed, so the off state is asserted rather than assumed.
 *  4. Enabling REFUSES while content is unattributed, because turning it on
 *     then hides those rows from everyone.
 *  5. A tenant scope does not outlive its transaction. aimee.principal lives on
 *     a POOLED connection, so a leak there is one tenant reading another's rows
 *     rather than merely a stale value.
 *
 * WHAT IS NOT HERE: whether a member sees their own project and a stranger does
 * not. That needs the policies, which land with the backfill in slice 2. The
 * end-to-end behaviour was prototyped in
 * docs/validation/per-user-content-scope-prototype.md.
 */
#include "db2.h"
#include "db2/db2_internal.h"
#include "db2/db_postgres.h"
#include "db2/db2_tenant.h"
#include "kb_identity.h"

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

   /* 3. The policies are DEFINED. They have to be, or enabling would be a
    *     schema change at the worst possible moment. */
   expect("SELECT count(*) FROM pg_policies"
          " WHERE tablename IN ('kb_documents','kb_file_index')",
          "2");
   printf("  PASS: the content policies are defined\n");

   /* 4. And INERT. A policy does nothing until RLS is enabled on its table, and
    *    that is what keeps applying this schema from hiding rows nobody has
    *    attributed yet. If this flips to enabled-on-apply, an upgrade becomes an
    *    outage, so it is asserted rather than assumed.
    *
    *    relrowsecurity is the ENABLE flag, relforcerowsecurity the FORCE one;
    *    both must be off, because FORCE without ENABLE is not a state worth
    *    reasoning about later. */
   expect("SELECT count(*) FROM pg_class"
          " WHERE relname IN ('kb_documents','kb_file_index')"
          "   AND (relrowsecurity OR relforcerowsecurity)",
          "0");
   printf("  PASS: they are inert until an operator enables them\n");

   /* 5. Enabling refuses while any content is unattributed. Turning it on over
    *    unattributed rows does not give a weaker control, it hides those rows
    *    from everyone, which reads as data loss. The refusal is the feature. */
   {
      char err[512] = "";
      aimee_pg_stmt_t *st =
          aimee_pg_prepare(db2_conn(), "SELECT kb_content_scope_enable()", err, sizeof(err));
      int refused = 0;
      if (st)
      {
         if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
            refused = 1;
         aimee_pg_finalize(st);
      }
      else
      {
         refused = 1;
      }
      /* Two independent reasons to refuse, and the reader one comes first: the
         content read paths set no aimee.principal today, so enabling would
         return nothing to everyone rather than hiding unattributed rows. That
         one holds even on an empty database, which is why it is checked
         separately from the attribution refusal below. */
      char marker[64] = "";
      assert(scalar("SELECT coalesce((SELECT value FROM kb_meta"
                    "  WHERE key='content_scope_reader_ready'),'0')",
                    marker, sizeof(marker)) == 0);
      if (strcmp(marker, "1") != 0)
      {
         if (!refused)
            fprintf(stderr, "kb_content_scope_enable() accepted while readers set no principal\n");
         assert(refused);
         printf("  PASS: enabling refuses while the read paths carry no principal\n");
      }

      /* The fixture database has no attributed content, so this must refuse.
         A pass here with an empty kb_documents would prove nothing, so the
         count is checked too. */
      char docs[64] = "";
      assert(scalar("SELECT count(*) FROM kb_documents", docs, sizeof(docs)) == 0);
      if (strcmp(docs, "0") != 0)
      {
         if (!refused)
            fprintf(stderr, "kb_content_scope_enable() accepted %s unattributed rows\n", docs);
         assert(refused);
         printf("  PASS: enabling refuses while content is unattributed\n");
      }
      else
      {
         printf("  SKIP: no content rows in this database to refuse over\n");
      }
      /* Whatever happened, leave the tables as they were found. */
      expect("SELECT count(*) FROM pg_class"
             " WHERE relname IN ('kb_documents','kb_file_index')"
             "   AND (relrowsecurity OR relforcerowsecurity)",
             "0");
   }

   /* 6. A tenant scope must not survive its transaction.
    *
    *    This is the property the whole content-scope design leans on and the one
    *    nobody would notice breaking: aimee.principal is a GUC on a POOLED
    *    connection, so if it outlived its scope the next request to borrow that
    *    connection would run as the previous user. Under RLS that is not a
    *    degraded answer, it is one tenant reading another's rows.
    *
    *    db2_tenant.c resets both GUCs deliberately (tenant_reset_gucs). Nothing
    *    pinned it, so this does: open a scope, close it, and look. */
   {
      /* A principal needs a team it is actually a member of, or set_tenant_context
         refuses -- which is itself the behaviour we want, so build the fixture. */
      char team_id[64] = "";
      assert(scalar("INSERT INTO kb_team(name) VALUES ('scope-leak-probe')"
                    " ON CONFLICT (name) DO UPDATE SET name=EXCLUDED.name RETURNING id",
                    team_id, sizeof(team_id)) == 0);

      /* identity_key is DERIVED (kb_identity_key), never a field: the canonical
         form is oidc:<iss>:<sub>. Ask for it rather than spelling it, so this
         test cannot drift from the derivation the membership table is keyed on. */
      kb_principal_t p;
      memset(&p, 0, sizeof(p));
      p.kind = KB_PRIN_OIDC;
      p.authenticated = 1;
      snprintf(p.issuer, sizeof(p.issuer), "%s", "https://probe.invalid");
      snprintf(p.subject, sizeof(p.subject), "%s", "leak");

      char key[640] = "";
      assert(kb_identity_key(&p, key, sizeof(key)) == 0);

      char sql[1024];
      snprintf(sql, sizeof(sql),
               "INSERT INTO kb_team_membership(identity_key, team, is_default)"
               " VALUES ('%s', %s, 1)"
               " ON CONFLICT (identity_key, team) DO UPDATE SET is_default=1 RETURNING id",
               key, team_id);
      char row[64] = "";
      assert(scalar(sql, row, sizeof(row)) == 0);

      if (db2_tenant_scope_begin(&p, (int64_t)atoll(team_id)) == 0)
      {
         char inside[640] = "";
         assert(scalar("SELECT coalesce(current_setting('aimee.principal', true),'<unset>')",
                       inside, sizeof(inside)) == 0);
         if (strcmp(inside, key) != 0)
            fprintf(stderr, "inside the scope the principal was \"%s\", expected \"%s\"\n", inside,
                    key);
         assert(strcmp(inside, key) == 0);
         assert(db2_tenant_scope_commit() == 0);

         /* And now, on the same pooled connection, it must be gone.
          *
          * RESET leaves the GUC as an EMPTY STRING rather than NULL, so the
          * assertion is about what it is not: not the previous identity, and not
          * anything a policy could match. kb_team_membership.identity_key has a
          * CHECK of 1..600 characters, so '' matches no row and the predicate
          * denies -- which is why empty is as safe as unset here, and why this
          * asserts the property rather than the spelling. */
         char after[640] = "";
         assert(scalar("SELECT coalesce(current_setting('aimee.principal', true),'')", after,
                       sizeof(after)) == 0);
         if (strcmp(after, key) == 0)
            fprintf(stderr, "the principal survived its scope on a pooled connection: \"%s\"\n",
                    after);
         assert(strcmp(after, key) != 0);
         assert(after[0] == '\0');

         char team_after[64] = "";
         assert(scalar("SELECT coalesce(current_setting('aimee.team', true),'')", team_after,
                       sizeof(team_after)) == 0);
         assert(team_after[0] == '\0');
         printf("  PASS: a tenant scope does not outlive its transaction\n");
      }
      else
      {
         /* Refusing to open the scope is a valid outcome for a database without
            the roles provisioned; say so rather than passing silently. */
         printf("  SKIP: could not open a tenant scope here (roles not provisioned)\n");
      }
   }

   db2_shutdown();
   printf("All tests passed.\n");
   return 0;
}
