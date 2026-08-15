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
 *  3. The user-read and maintenance-write policies exist but are INERT: RLS is
 *     not enabled on either table, so applying this schema changes what nobody
 *     can read. A policy that switched
 *     itself on would turn an upgrade into an outage for every row not yet
 *     attributed, so the off state is asserted rather than assumed.
 *  4. Reader readiness is declared without enabling RLS, and enabling still
 *     REFUSES while content is unattributed.
 *  5. Tenant and maintenance scopes do not outlive their transactions. Their
 *     GUCs live on a POOLED connection, so a leak there is one tenant reading
 *     another's rows rather than merely a stale value.
 *  6. The re-embed worker completes its post-embed payload read and vector write
 *     under a fresh exact-project scope, without crossing into a sibling project.
 *  7. Maintenance authority is named, exact-project, and transaction-local.
 *  8. Two users on two teams search the ordinary KB path with FORCE RLS enabled;
 *     each sees only their own project, and a caller-less search sees neither.
 */
#include "db2.h"
#include "db2/db2_internal.h"
#include "db2/db_postgres.h"
#include "db2/db2_tenant.h"
#include "db2/project.h"
#include "cJSON.h"
#include "kb.h"
#include "kb_identity.h"
#include "memory.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Single-value scalar query. Returns 0 and fills out on success. */
static int scalar(const char *sql, char *out, size_t cap);

/* Count only result objects whose project field exactly matches |project|.
 * Metadata is deliberately ignored so future diagnostic fields cannot turn a
 * harmless project-name echo into an isolation-test failure. */
static int search_project_count(const char *json, const char *project)
{
   cJSON *root = json ? cJSON_Parse(json) : NULL;
   cJSON *results = root ? cJSON_GetObjectItemCaseSensitive(root, "results") : NULL;
   if (!cJSON_IsArray(results))
   {
      cJSON_Delete(root);
      return -1;
   }
   int count = 0;
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, results)
   {
      cJSON *value = cJSON_GetObjectItemCaseSensitive(item, "project");
      if (cJSON_IsString(value) && strcmp(value->valuestring, project) == 0)
         count++;
   }
   cJSON_Delete(root);
   return count;
}

static int exec_sql(const char *sql)
{
   char err[256] = "";
   int rc = aimee_pg_exec(db2_conn(), sql, err, sizeof(err));
   if (rc != 0)
      fprintf(stderr, "exec failed: %s\n  sql: %s\n", err, sql);
   return rc;
}

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
          "4");
   printf("  PASS: the user-read and maintenance-write policies are defined\n");

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

   /* 5. The release declares reader readiness without enabling RLS. Enabling
    *    still refuses while any content is unattributed: turning it on over an
    *    orphan does not give a weaker control, it hides that row from everyone. */
   {
      expect("SELECT value FROM kb_meta WHERE key='content_scope_reader_ready'", "1");
      printf("  PASS: reader readiness is declared without enabling content scope\n");

      char orphan_id[64] = "";
      assert(scalar("INSERT INTO projects(name,root,scanned_at,kb_project)"
                    " VALUES ('scope-unattributed','/scope/unattributed','',NULL)"
                    " ON CONFLICT (name) DO UPDATE SET kb_project=NULL RETURNING id",
                    orphan_id, sizeof(orphan_id)) == 0);
      assert(exec_sql("DELETE FROM kb_documents"
                      " WHERE project='scope-unattributed'"
                      " AND file_path='scope-unattributed.md'") == 0);
      assert(exec_sql("DELETE FROM projects WHERE name='scope-unattributed'") == 0);
      assert(scalar("INSERT INTO kb_documents"
                    " (project,generation,file_path,file_hash,chunk_index,heading_path,"
                    "  line_start,line_end,content,token_count)"
                    " SELECT name,current_generation,'scope-unattributed.md','orphan-hash',0,"
                    "  'Unattributed',1,1,'must block content scope enable',5"
                    " FROM projects WHERE id="
                    " (SELECT id FROM projects WHERE name='scope-unattributed') RETURNING id",
                    orphan_id, sizeof(orphan_id)) == 0);

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
      if (!refused)
         fprintf(stderr, "kb_content_scope_enable() accepted an unattributed row\n");
      assert(refused);
      printf("  PASS: enabling refuses while content is unattributed\n");
      /* Whatever happened, leave the tables as they were found. */
      expect("SELECT count(*) FROM pg_class"
             " WHERE relname IN ('kb_documents','kb_file_index')"
             "   AND (relrowsecurity OR relforcerowsecurity)",
             "0");
      assert(exec_sql("DELETE FROM kb_documents"
                      " WHERE project='scope-unattributed'"
                      " AND file_path='scope-unattributed.md'") == 0);
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

   /* 7. Background work gets a named PROJECT scope, never a synthetic user.
    *    The SQL setter independently allowlists the worker and resolves the
    *    project to its attributed kb_project. Prove that the selected project is
    *    visible, a sibling is not, and every maintenance GUC is gone on commit. */
   {
      char team_id[64] = "";
      assert(scalar("INSERT INTO kb_team(name) VALUES ('scope-maintenance-team')"
                    " ON CONFLICT (name) DO UPDATE SET name=EXCLUDED.name RETURNING id",
                    team_id, sizeof(team_id)) == 0);

      char project_a[64] = "", project_b[64] = "", sql[2048];
      snprintf(sql, sizeof(sql),
               "INSERT INTO kb_project(parent,name,access_mode)"
               " VALUES (%s,'scope-maintenance-a','team-open')"
               " ON CONFLICT (parent,name) DO UPDATE SET access_mode='team-open' RETURNING id",
               team_id);
      assert(scalar(sql, project_a, sizeof(project_a)) == 0);
      snprintf(sql, sizeof(sql),
               "INSERT INTO kb_project(parent,name,access_mode)"
               " VALUES (%s,'scope-maintenance-b','team-open')"
               " ON CONFLICT (parent,name) DO UPDATE SET access_mode='team-open' RETURNING id",
               team_id);
      assert(scalar(sql, project_b, sizeof(project_b)) == 0);

      snprintf(sql, sizeof(sql),
               "INSERT INTO projects(name,root,scanned_at,kb_project)"
               " VALUES ('scope-maintenance-a','/scope/a','',NULL)"
               " ON CONFLICT (name) DO UPDATE SET kb_project=NULL RETURNING id");
      char ignored[64] = "";
      assert(scalar(sql, ignored, sizeof(ignored)) == 0);
      snprintf(sql, sizeof(sql),
               "INSERT INTO projects(name,root,scanned_at,kb_project)"
               " VALUES ('scope-maintenance-b','/scope/b','',NULL)"
               " ON CONFLICT (name) DO UPDATE SET kb_project=NULL RETURNING id");
      assert(scalar(sql, ignored, sizeof(ignored)) == 0);

      kb_principal_t owner = {.kind = KB_PRIN_OWNER, .authenticated = 1};
      assert(db2_tenant_scope_begin(&owner, 0) == 0);
      assert(db2_project_attribute_code("scope-maintenance-a", (int64_t)atoll(project_a)) == 0);
      assert(db2_project_attribute_code("scope-maintenance-b", (int64_t)atoll(project_b)) == 0);
      assert(db2_tenant_scope_commit() == 0);
      snprintf(sql, sizeof(sql),
               "SELECT CASE WHEN kb_project=%s THEN 'bound' ELSE 'wrong' END"
               " FROM projects WHERE name='scope-maintenance-a'",
               project_a);
      expect(sql, "bound");

      kb_principal_t member = {.kind = KB_PRIN_OIDC, .authenticated = 1};
      snprintf(member.issuer, sizeof(member.issuer), "%s", "https://member.invalid");
      snprintf(member.subject, sizeof(member.subject), "%s", "not-admin");
      char member_key[640] = "";
      assert(kb_identity_key(&member, member_key, sizeof(member_key)) == 0);
      snprintf(sql, sizeof(sql),
               "INSERT INTO kb_team_membership(identity_key,team,is_default)"
               " VALUES ('%s',%s,0) ON CONFLICT (identity_key,team)"
               " DO UPDATE SET is_default=EXCLUDED.is_default RETURNING id",
               member_key, team_id);
      assert(scalar(sql, ignored, sizeof(ignored)) == 0);
      assert(db2_tenant_scope_begin(&member, (int64_t)atoll(team_id)) == 0);
      assert(db2_project_attribute_code("scope-maintenance-a", (int64_t)atoll(project_b)) != 0);
      db2_tenant_scope_rollback();
      snprintf(sql, sizeof(sql),
               "SELECT CASE WHEN kb_project=%s THEN 'bound' ELSE 'wrong' END"
               " FROM projects WHERE name='scope-maintenance-a'",
               project_a);
      expect(sql, "bound");
      printf("  PASS: attribution is explicit by id and denied to a non-admin member\n");

      assert(db2_maintenance_scope_begin(DB2_MAINTENANCE_CURATOR, "scope-maintenance-a") == 0);
      expect("SELECT current_setting('aimee.maintenance_worker',true)", "curator");
      expect("SELECT current_setting('aimee.maintenance_project',true)", "scope-maintenance-a");
      snprintf(sql, sizeof(sql),
               "SELECT CASE WHEN kb_project_visible(%s) THEN 'allow' ELSE 'deny' END", project_a);
      expect(sql, "allow");
      snprintf(sql, sizeof(sql),
               "SELECT CASE WHEN kb_project_visible(%s) THEN 'allow' ELSE 'deny' END", project_b);
      expect(sql, "deny");
      assert(db2_maintenance_scope_commit() == 0);

      expect("SELECT coalesce(current_setting('aimee.maintenance_worker',true),'')", "");
      expect("SELECT coalesce(current_setting('aimee.maintenance_project',true),'')", "");
      expect("SELECT coalesce(current_setting('aimee.maintenance_kb_project',true),'')", "");
      assert(db2_maintenance_scope_begin((db2_maintenance_worker_t)999, "scope-maintenance-a") ==
             DB2_ERR_MAINTENANCE_INVALID);
      assert(
          db2_maintenance_scope_begin(DB2_MAINTENANCE_CURATOR, "scope-maintenance-unattributed") ==
          DB2_ERR_MAINTENANCE_INVALID);

      /* Worker wiring is inert before the operator enables content RLS, then
       * begins a fresh short transaction for every content phase. This lets the
       * wiring land without changing behavior before the final slice. */
      assert(db2_maintenance_job_enter(DB2_MAINTENANCE_CURATOR, "scope-maintenance-a") == 0);
      assert(db2_maintenance_scope_begin_current() == 0);
      db2_maintenance_job_leave();

      /* Seed selected and sibling unembedded chunks while the policies are inert. The
       * backfill discovers it under one maintenance transaction, drops the DB
       * lease for the embedder round-trip, then must open a fresh scope to
       * rebuild its payload from kb_documents and write kb_embeddings. */
      assert(exec_sql("DELETE FROM kb_embeddings WHERE point_id IN ("
                      "SELECT id FROM kb_documents"
                      " WHERE project='scope-maintenance-a'"
                      " AND file_path='scope-maintenance-reembed.md')") == 0);
      assert(exec_sql("DELETE FROM kb_documents"
                      " WHERE project='scope-maintenance-a'"
                      " AND file_path='scope-maintenance-reembed.md'") == 0);
      assert(exec_sql("DELETE FROM kb_embeddings WHERE point_id IN ("
                      "SELECT id FROM kb_documents"
                      " WHERE project='scope-maintenance-b'"
                      " AND file_path='scope-maintenance-sibling.md')") == 0);
      assert(exec_sql("DELETE FROM kb_documents"
                      " WHERE project='scope-maintenance-b'"
                      " AND file_path='scope-maintenance-sibling.md'") == 0);
      char reembed_doc[64] = "";
      assert(scalar("INSERT INTO kb_documents"
                    " (project,generation,file_path,file_hash,chunk_index,heading_path,"
                    "  line_start,line_end,content,token_count)"
                    " SELECT name,current_generation,'scope-maintenance-reembed.md',"
                    "  'scope-hash',0,'Background scope',1,1,"
                    "  'project scoped re-embedding proof',5"
                    " FROM projects WHERE name='scope-maintenance-a' RETURNING id",
                    reembed_doc, sizeof(reembed_doc)) == 0);
      char sibling_doc[64] = "";
      assert(scalar("INSERT INTO kb_documents"
                    " (project,generation,file_path,file_hash,chunk_index,heading_path,"
                    "  line_start,line_end,content,token_count)"
                    " SELECT name,current_generation,'scope-maintenance-sibling.md',"
                    "  'sibling-hash',0,'Sibling scope',1,1,"
                    "  'must remain outside the selected project',6"
                    " FROM projects WHERE name='scope-maintenance-b' RETURNING id",
                    sibling_doc, sizeof(sibling_doc)) == 0);

      assert(exec_sql("ALTER TABLE kb_documents ENABLE ROW LEVEL SECURITY") == 0);
      assert(exec_sql("ALTER TABLE kb_documents FORCE ROW LEVEL SECURITY") == 0);
      assert(exec_sql("ALTER TABLE kb_file_index ENABLE ROW LEVEL SECURITY") == 0);
      assert(exec_sql("ALTER TABLE kb_file_index FORCE ROW LEVEL SECURITY") == 0);
      assert(db2_maintenance_job_enter(DB2_MAINTENANCE_CURATOR, "scope-maintenance-a") == 0);
      assert(db2_maintenance_scope_begin_current() == 1);
      expect("SELECT current_setting('aimee.maintenance_project',true)", "scope-maintenance-a");
      assert(db2_maintenance_scope_commit() == 0);
      db2_maintenance_job_leave();

      assert(db2_maintenance_job_enter(DB2_MAINTENANCE_REEMBED, "scope-maintenance-a") == 0);
      assert(kb_doc_embed_backfill("scope-maintenance-a", MEMORY_EMBED_TEST_FIXTURE, 1) == 1);
      assert(db2_maintenance_scope_begin_current() == 1);
      snprintf(sql, sizeof(sql),
               "SELECT CASE WHEN e.project='scope-maintenance-a' AND p.kb_project=%s"
               " THEN 'bound' ELSE 'wrong' END"
               " FROM kb_embeddings e JOIN kb_documents d ON d.id=e.point_id"
               " JOIN projects p ON p.name=d.project WHERE e.point_id=%s",
               project_a, reembed_doc);
      expect(sql, "bound");
      assert(db2_maintenance_scope_commit() == 0);
      snprintf(sql, sizeof(sql), "SELECT count(*) FROM kb_embeddings WHERE point_id=%s",
               sibling_doc);
      expect(sql, "0");
      db2_maintenance_job_leave();
      printf("  PASS: re-embed reapplies exact-project scope after the embedder round-trip\n");
      printf("  PASS: re-embed cannot cross into an unselected sibling chunk\n");

      assert(exec_sql("ALTER TABLE kb_documents NO FORCE ROW LEVEL SECURITY") == 0);
      assert(exec_sql("ALTER TABLE kb_documents DISABLE ROW LEVEL SECURITY") == 0);
      assert(exec_sql("ALTER TABLE kb_file_index NO FORCE ROW LEVEL SECURITY") == 0);
      assert(exec_sql("ALTER TABLE kb_file_index DISABLE ROW LEVEL SECURITY") == 0);
      printf("  PASS: maintenance authority is named, project-bound, and transaction-local\n");
   }

   /* 8. The final reader proof uses the ordinary hybrid-search function, not a
    *    hand-written policy query. Two authenticated users belong to different
    *    teams whose projects contain the same unique term. Once the operator
    *    enables FORCE RLS, each search must materialize only its own row, and a
    *    caller-less search after pooled-scope cleanup must materialize neither. */
   {
      char team_a[64] = "", team_b[64] = "", project_a[64] = "", project_b[64] = "";
      char ignored[64] = "", sql[2048];
      assert(scalar("INSERT INTO kb_team(name) VALUES ('scope-reader-team-a')"
                    " ON CONFLICT (name) DO UPDATE SET name=EXCLUDED.name RETURNING id",
                    team_a, sizeof(team_a)) == 0);
      assert(scalar("INSERT INTO kb_team(name) VALUES ('scope-reader-team-b')"
                    " ON CONFLICT (name) DO UPDATE SET name=EXCLUDED.name RETURNING id",
                    team_b, sizeof(team_b)) == 0);

      snprintf(sql, sizeof(sql),
               "INSERT INTO kb_project(parent,name,access_mode)"
               " VALUES (%s,'scope-reader-a','team-open')"
               " ON CONFLICT (parent,name) DO UPDATE SET access_mode='team-open' RETURNING id",
               team_a);
      assert(scalar(sql, project_a, sizeof(project_a)) == 0);
      snprintf(sql, sizeof(sql),
               "INSERT INTO kb_project(parent,name,access_mode)"
               " VALUES (%s,'scope-reader-b','team-open')"
               " ON CONFLICT (parent,name) DO UPDATE SET access_mode='team-open' RETURNING id",
               team_b);
      assert(scalar(sql, project_b, sizeof(project_b)) == 0);

      snprintf(sql, sizeof(sql),
               "INSERT INTO projects(name,root,scanned_at,kb_project)"
               " VALUES ('scope-reader-a','/scope/reader-a','',%s)"
               " ON CONFLICT (name) DO UPDATE SET kb_project=EXCLUDED.kb_project,"
               " lifecycle_state='current' RETURNING id",
               project_a);
      assert(scalar(sql, ignored, sizeof(ignored)) == 0);
      snprintf(sql, sizeof(sql),
               "INSERT INTO projects(name,root,scanned_at,kb_project)"
               " VALUES ('scope-reader-b','/scope/reader-b','',%s)"
               " ON CONFLICT (name) DO UPDATE SET kb_project=EXCLUDED.kb_project,"
               " lifecycle_state='current' RETURNING id",
               project_b);
      assert(scalar(sql, ignored, sizeof(ignored)) == 0);

      kb_principal_t reader_a = {.kind = KB_PRIN_OIDC, .authenticated = 1};
      kb_principal_t reader_b = {.kind = KB_PRIN_OIDC, .authenticated = 1};
      snprintf(reader_a.issuer, sizeof(reader_a.issuer), "%s", "https://reader.invalid");
      snprintf(reader_a.subject, sizeof(reader_a.subject), "%s", "alice");
      snprintf(reader_b.issuer, sizeof(reader_b.issuer), "%s", "https://reader.invalid");
      snprintf(reader_b.subject, sizeof(reader_b.subject), "%s", "bob");
      char key_a[640] = "", key_b[640] = "";
      assert(kb_identity_key(&reader_a, key_a, sizeof(key_a)) == 0);
      assert(kb_identity_key(&reader_b, key_b, sizeof(key_b)) == 0);
      snprintf(sql, sizeof(sql),
               "INSERT INTO kb_team_membership(identity_key,team,is_default)"
               " VALUES ('%s',%s,1) ON CONFLICT (identity_key,team)"
               " DO UPDATE SET is_default=1 RETURNING id",
               key_a, team_a);
      assert(scalar(sql, ignored, sizeof(ignored)) == 0);
      snprintf(sql, sizeof(sql),
               "INSERT INTO kb_team_membership(identity_key,team,is_default)"
               " VALUES ('%s',%s,1) ON CONFLICT (identity_key,team)"
               " DO UPDATE SET is_default=1 RETURNING id",
               key_b, team_b);
      assert(scalar(sql, ignored, sizeof(ignored)) == 0);

      assert(exec_sql("DELETE FROM kb_documents"
                      " WHERE project IN ('scope-reader-a','scope-reader-b')"
                      " AND file_path IN ('reader-a.md','reader-b.md')") == 0);
      assert(scalar("INSERT INTO kb_documents"
                    " (project,generation,file_path,file_hash,chunk_index,heading_path,"
                    "  line_start,line_end,content,token_count)"
                    " SELECT name,current_generation,'reader-a.md','reader-a-hash',0,"
                    "  'Reader A',1,1,'readinessisolationtoken belongs to reader alpha',5"
                    " FROM projects WHERE name='scope-reader-a' RETURNING id",
                    ignored, sizeof(ignored)) == 0);
      assert(scalar("INSERT INTO kb_documents"
                    " (project,generation,file_path,file_hash,chunk_index,heading_path,"
                    "  line_start,line_end,content,token_count)"
                    " SELECT name,current_generation,'reader-b.md','reader-b-hash',0,"
                    "  'Reader B',1,1,'readinessisolationtoken belongs to reader beta',5"
                    " FROM projects WHERE name='scope-reader-b' RETURNING id",
                    ignored, sizeof(ignored)) == 0);

      expect("SELECT count(*) FROM kb_documents d"
             " WHERE NOT EXISTS (SELECT 1 FROM projects p"
             " WHERE p.name=d.project AND p.kb_project IS NOT NULL)",
             "0");
      expect("SELECT count(*) FROM kb_file_index f"
             " WHERE NOT EXISTS (SELECT 1 FROM projects p"
             " WHERE p.name=f.project AND p.kb_project IS NOT NULL)",
             "0");
      expect("SELECT kb_content_scope_enable()",
             "content scope enabled on kb_documents, kb_file_index");

      assert(db2_tenant_scope_begin(&reader_a, (int64_t)atoll(team_a)) == 0);
      char *result_a =
          kb_search_json_ex(NULL, "readinessisolationtoken", MEMORY_EMBED_TEST_FIXTURE, 10, "rrf");
      assert(result_a);
      int result_a_own = search_project_count(result_a, "scope-reader-a");
      int result_a_other = search_project_count(result_a, "scope-reader-b");
      if (result_a_own != 1 || result_a_other != 0)
         fprintf(stderr, "reader A search crossed content scope: %s\n", result_a);
      assert(result_a_own == 1);
      assert(result_a_other == 0);
      free(result_a);
      assert(db2_tenant_scope_commit() == 0);

      char *anonymous =
          kb_search_json_ex(NULL, "readinessisolationtoken", MEMORY_EMBED_TEST_FIXTURE, 10, "rrf");
      assert(anonymous);
      int anonymous_a = search_project_count(anonymous, "scope-reader-a");
      int anonymous_b = search_project_count(anonymous, "scope-reader-b");
      if (anonymous_a != 0 || anonymous_b != 0)
         fprintf(stderr, "caller-less search inherited content scope: %s\n", anonymous);
      assert(anonymous_a == 0);
      assert(anonymous_b == 0);
      free(anonymous);

      assert(db2_tenant_scope_begin(&reader_b, (int64_t)atoll(team_b)) == 0);
      char *result_b =
          kb_search_json_ex(NULL, "readinessisolationtoken", MEMORY_EMBED_TEST_FIXTURE, 10, "rrf");
      assert(result_b);
      int result_b_own = search_project_count(result_b, "scope-reader-b");
      int result_b_other = search_project_count(result_b, "scope-reader-a");
      if (result_b_own != 1 || result_b_other != 0)
         fprintf(stderr, "reader B search crossed content scope: %s\n", result_b);
      assert(result_b_own == 1);
      assert(result_b_other == 0);
      free(result_b);
      assert(db2_tenant_scope_commit() == 0);

      expect("SELECT kb_content_scope_disable()", "content scope disabled");
      printf("  PASS: two users on two teams search only their own project under FORCE RLS\n");
      printf("  PASS: caller-less search inherits no pooled content identity\n");

      assert(exec_sql("DELETE FROM kb_documents"
                      " WHERE project IN ('scope-reader-a','scope-reader-b')"
                      " AND file_path IN ('reader-a.md','reader-b.md')") == 0);
      assert(exec_sql("DELETE FROM projects WHERE name IN ('scope-reader-a','scope-reader-b')") ==
             0);
      snprintf(sql, sizeof(sql), "DELETE FROM kb_team_membership WHERE identity_key IN ('%s','%s')",
               key_a, key_b);
      assert(exec_sql(sql) == 0);
      assert(exec_sql("DELETE FROM kb_team"
                      " WHERE name IN ('scope-reader-team-a','scope-reader-team-b')") == 0);
   }

   db2_shutdown();
   printf("All tests passed.\n");
   return 0;
}
