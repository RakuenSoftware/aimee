/* test_kb_audit_worm_pg.c: PROOF that the SQL WORM appender (schema.sql
 * kb_audit_worm_append) and the C WORM appender (db2/kb_audit_worm.c
 * db2_kb_audit_append) write BYTE-IDENTICAL rows into the SAME kb_audit_event hash
 * chain — so a chain interleaving C and SQL appends still verifies under the C
 * verifier. This is the load-bearing invariant of the P2a atomic-audit design: the
 * SECURITY DEFINER catalog mutations audit via kb_audit_worm_append() inside their own
 * txn, and that row MUST hash exactly as the C store would.
 *
 * REAL-PG ONLY: plpgsql SECURITY DEFINER functions cannot run on the SQLite shim, so
 * this test needs a live Postgres. It reads AIMEE_TEST_PG_URL and SKIPS CLEANLY (exit 0)
 * when it is unset — mirroring test_vault_pg.c, so `make unit-tests` on a box without
 * Postgres stays green. Validated for real on CT103.
 *
 * The mixed chain built here is:  [C append] -> [SQL append] -> [C append]
 * and the test asserts (a) db2_kb_audit_verify_chain() == 0 over the whole chain (which
 * only returns 0 if EVERY row, including the SQL one, recomputes to its stored hash under
 * the C canonicalization), and (b) an INDEPENDENT recomputation in C of the SQL row's
 * hash (audit_worm_row_hash over its stored fields + stored prev_hash) equals the row
 * hash the SQL function stored — direct byte-identity, not merely self-consistency. */
#include <aimee/audit/audit_worm_chain.h> /* audit_worm_row_hash, AUDIT_WORM_GENESIS_PREV */
#include <aimee/db2/host_contracts.h>
#include "modules/db2/c/db2.h"          /* db2_init / db2_shutdown */
#include "modules/db2/c/db2_internal.h" /* db2_conn */
#include "modules/db2/c/db_postgres.h"  /* aimee_pg_* */
#include "kb_audit_worm.h"              /* db2_kb_audit_append / verify / count */
#include "config_embedder_dims.h"        /* CONFIG_EMBEDDER_DIMS_DEFAULT */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Best-effort reset to a fresh chain so seq numbers are deterministic. Drops the WORM
 * triggers, empties the table, recreates the triggers. If the connecting role cannot do
 * this the test still holds: it measures a baseline count and verifies the whole chain. */
static void reset_chain(void *conn)
{
   char err[256];
   aimee_pg_exec(conn, "DROP TRIGGER IF EXISTS kb_audit_no_update ON kb_audit_event", err,
                 sizeof err);
   aimee_pg_exec(conn, "DROP TRIGGER IF EXISTS kb_audit_no_delete ON kb_audit_event", err,
                 sizeof err);
   aimee_pg_exec(conn, "DROP TRIGGER IF EXISTS kb_audit_no_truncate ON kb_audit_event", err,
                 sizeof err);
   aimee_pg_exec(conn, "DELETE FROM kb_audit_event", err, sizeof err);
   aimee_pg_exec(conn,
                 "CREATE TRIGGER kb_audit_no_update BEFORE UPDATE ON kb_audit_event"
                 " FOR EACH ROW EXECUTE FUNCTION kb_worm_block()",
                 err, sizeof err);
   aimee_pg_exec(conn,
                 "CREATE TRIGGER kb_audit_no_delete BEFORE DELETE ON kb_audit_event"
                 " FOR EACH ROW EXECUTE FUNCTION kb_worm_block()",
                 err, sizeof err);
   aimee_pg_exec(conn,
                 "CREATE TRIGGER kb_audit_no_truncate BEFORE TRUNCATE ON kb_audit_event"
                 " FOR EACH STATEMENT EXECUTE FUNCTION kb_worm_block()",
                 err, sizeof err);
}

/* Append one row via the SQL definer kb_audit_worm_append(...). Returns 0 on success. */
static int sql_append(void *conn, const char *role, const char *principal, const char *action,
                      const char *subject, const char *verdict, const char *detail)
{
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT kb_audit_worm_append(?1,?2,?3,?4,?5,?6)", err, sizeof err);
   if (!st)
   {
      fprintf(stderr, "sql_append prepare failed: %s\n", err);
      return -1;
   }
   aimee_pg_bind_text(st, "?1", role);
   aimee_pg_bind_text(st, "?2", principal);
   aimee_pg_bind_text(st, "?3", action);
   aimee_pg_bind_text(st, "?4", subject);
   aimee_pg_bind_text(st, "?5", verdict);
   aimee_pg_bind_text(st, "?6", detail);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof err);
   aimee_pg_finalize(st);
   if (rc == AIMEE_PG_ERR)
   {
      fprintf(stderr, "sql_append step failed: %s\n", err);
      return -1;
   }
   return 0;
}

/* Independently recompute, in C, the row_hash for the row at `seq` from its STORED fields
 * + STORED prev_hash, and assert it equals the STORED row_hash. Proves the row (written
 * by whichever engine) hashes byte-identically under the shared C canonicalization. */
static void assert_row_hashes_in_c(void *conn, long long seq)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT actor_role, actor_principal, action, subject, verdict, key_id, detail,"
       " prev_hash, row_hash FROM kb_audit_event WHERE seq = ?1",
       err, sizeof err);
   assert(st);
   aimee_pg_bind_int64(st, "?1", seq);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof err);
   assert(rc == AIMEE_PG_ROW);
   const char *role = aimee_pg_column_text(st, 0);
   const char *principal = aimee_pg_column_text(st, 1);
   const char *action = aimee_pg_column_text(st, 2);
   const char *subject = aimee_pg_column_text(st, 3);
   const char *verdict = aimee_pg_column_text(st, 4);
   const char *key_id = aimee_pg_column_text(st, 5);
   const char *detail = aimee_pg_column_text(st, 6);
   const char *prev = aimee_pg_column_text(st, 7);
   const char *stored = aimee_pg_column_text(st, 8);
   char rh[65];
   audit_worm_row_hash(seq, role ? role : "", principal ? principal : "", action ? action : "",
                       subject ? subject : "", verdict ? verdict : "", key_id ? key_id : "",
                       detail ? detail : "", prev ? prev : "", rh);
   assert(stored && strcmp(rh, stored) == 0);
   aimee_pg_finalize(st);
   printf("  PASS: SQL row seq %lld hashes byte-identically in C (%s)\n", seq, rh);
}

static long long chain_max_seq(void *conn)
{
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT COALESCE(max(seq),0) FROM kb_audit_event", err, sizeof err);
   assert(st);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof err);
   long long v = (rc == AIMEE_PG_ROW) ? aimee_pg_column_int64(st, 0) : -1;
   aimee_pg_finalize(st);
   return v;
}

static void run(void)
{
   void *conn = db2_conn();
   assert(conn);
   reset_chain(conn);

   long before = db2_kb_audit_count();
   assert(before >= 0);

   /* (a) C append, (b) SQL append via kb_audit_worm_append, (c) C append. */
   assert(db2_kb_audit_append("primary", "u", "tool.read", "v1-1", "allow", "{}") == 0);
   assert(sql_append(conn, "primary", "u", "p2a.test", "s", "ok", "{}") == 0);
   assert(db2_kb_audit_append("delegate", "mimo", "kb.query", "q1", "ok", "{}") == 0);

   assert(db2_kb_audit_count() == before + 3);
   printf("  PASS: mixed chain built [C, SQL, C] (+3 rows)\n");

   /* (d) The C verifier accepts the WHOLE chain — only possible if the SQL row's stored
    * row_hash matches what the C canonicalization recomputes for it. */
   char err[256];
   assert(db2_kb_audit_verify_chain(err, sizeof err) == 0);
   printf("  PASS: db2_kb_audit_verify_chain == 0 over the mixed chain\n");

   /* Direct byte-identity: the middle row (the SQL append) is max-1. */
   long long max_seq = chain_max_seq(conn);
   assert_row_hashes_in_c(conn, max_seq - 1);
}

int main(void)
{
   const char *url = getenv("AIMEE_TEST_PG_URL");
   if (!url || !url[0])
   {
      printf("kb_audit_worm_pg: SKIP (AIMEE_TEST_PG_URL unset; real Postgres required)\n");
      return 0;
   }

   char home[256];
   snprintf(home, sizeof(home), "/tmp/aimee-kb-audit-worm-pg-%d", (int)getpid());
   char mk[320];
   snprintf(mk, sizeof(mk), "rm -rf %s && mkdir -p %s", home, home);
   assert(system(mk) == 0);
   setenv("AIMEE_HOME", home, 1);

   /* Real-PG tests bypass the production entry points, so they must perform the
    * same explicit width injection as cmd_core/kb_main before db2_init(). */
   db2_set_embedding_dim_default(CONFIG_EMBEDDER_DIMS_DEFAULT);
   db2_set_embedding_dim(CONFIG_EMBEDDER_DIMS_DEFAULT);

   if (db2_init(url) != 0)
   {
      fprintf(stderr, "kb_audit_worm_pg: db2_init failed for %s\n", url);
      return 1;
   }

   aimee_db2_register_audit_hash_provider(audit_worm_row_hash);
   run();

   db2_shutdown();
   char rm[320];
   snprintf(rm, sizeof(rm), "rm -rf %s", home);
   (void)system(rm);
   printf("kb_audit_worm_pg: all tests passed\n");
   return 0;
}
