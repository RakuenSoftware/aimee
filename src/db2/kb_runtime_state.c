/* kb_runtime_state.c: DB2-backed runtime state for aimee-kb.
 *
 * Postgres-only: drives the kb_runtime_state table over libpq via the
 * shared db2 connection. Returns -1 / 0 for "no handle" so callers fail
 * soft when DB2 isn't initialised. */

#include "kb_runtime_state.h"

#include "config.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define KBRS_ERRBUF 256

int db2_kb_runtime_state_set(const char *key, const char *value)
{
   if (!key)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBRS_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO kb_runtime_state (state_key, state_value) VALUES (?1, ?2) "
                        "ON CONFLICT (state_key) DO UPDATE SET state_value = EXCLUDED.state_value",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", key);
   aimee_pg_bind_text(st, "?2", value ? value : "");
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int db2_kb_runtime_state_get(const char *key, char *out, size_t out_len)
{
   if (!key || !out || out_len == 0)
      return -1;
   out[0] = '\0';
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBRS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT state_value FROM kb_runtime_state WHERE state_key = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", key);

   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *v = aimee_pg_column_text(st, 0);
      snprintf(out, out_len, "%s", v ? v : "");
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_kb_runtime_state_delete(const char *key)
{
   if (!key)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBRS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, "DELETE FROM kb_runtime_state WHERE state_key = ?1",
                                          err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", key);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int db2_kb_runtime_state_set_now(const char *key)
{
   if (!key)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBRS_ERRBUF] = "";
   /* pg_now_text() stores the DB2 canonical UTC text format. */
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "INSERT INTO kb_runtime_state (state_key, state_value) VALUES (?1, pg_now_text()) "
       "ON CONFLICT (state_key) DO UPDATE SET state_value = EXCLUDED.state_value",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", key);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int db2_kb_runtime_state_vector_rebuild_lock_held(void)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char err[KBRS_ERRBUF] = "";
   /* 1800s = 30 minutes. state_value is the UTC TEXT timestamp written
    * by pg_now_text(); CURRENT_TIMESTAMP is forced to UTC for the
    * comparison. */
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT 1 FROM kb_runtime_state"
       " WHERE state_key = 'vector_rebuild_lock'"
       "   AND state_value::timestamp > (CURRENT_TIMESTAMP AT TIME ZONE 'UTC') - interval '1800 "
       "seconds'",
       err, sizeof(err));
   if (!st)
      return 0;
   int held = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW) ? 1 : 0;
   aimee_pg_finalize(st);
   return held;
}

int db2_kb_runtime_state_vector_rebuild_lock_try_acquire(void)
{
   if (db2_kb_runtime_state_vector_rebuild_lock_held())
      return 0;
   return db2_kb_runtime_state_set_now("vector_rebuild_lock") == 0 ? 1 : 0;
}

void db2_kb_runtime_state_vector_rebuild_lock_release(void)
{
   (void)db2_kb_runtime_state_delete("vector_rebuild_lock");
}

/* ── Project-purge generation fence (webchat-project-lifecycle slice 2) ──
 *
 * Two kb_runtime_state rows per fenced project key:
 *   project_purging:<key>    = "<generation> <purge_id>"  (space-separated text)
 *   project_purging_ts:<key> = pg_now_text()              (heartbeat timestamp)
 *
 * The value and the heartbeat live in separate rows because state_value is
 * TEXT and the TTL idiom (state_value::timestamp > now - interval) needs a
 * castable timestamp — a composite value would not cast. */

#define KBRS_FENCE_PREFIX    "project_purging:"
#define KBRS_FENCE_TS_PREFIX "project_purging_ts:"
#define KBRS_FENCE_TTL_DFLT  900

static int kbrs_fence_ttl_s(void)
{
   config_t cfg;
   if (config_load(&cfg) == 0 && cfg.kb_purge_fence_ttl_s > 0)
      return cfg.kb_purge_fence_ttl_s;
   return KBRS_FENCE_TTL_DFLT;
}

static void kbrs_fence_keys(const char *project, char *key, size_t key_cap, char *ts_key,
                            size_t ts_cap)
{
   snprintf(key, key_cap, KBRS_FENCE_PREFIX "%s", project ? project : "");
   snprintf(ts_key, ts_cap, KBRS_FENCE_TS_PREFIX "%s", project ? project : "");
}

/* 1 iff the heartbeat row for ts_key is younger than `secs`. A missing or
 * unparseable heartbeat row counts as NOT within. (Writer-side fail-closed
 * handling of "identity row without a heartbeat row" lives in
 * db2_kb_purge_fence_active, not here — liveness callers want stale=0.) */
static int kbrs_fence_ts_within(const char *ts_key, int secs)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   if (secs < 1)
      secs = 1;

   char sql[256];
   snprintf(sql, sizeof(sql),
            "SELECT 1 FROM kb_runtime_state"
            " WHERE state_key = ?1"
            "   AND state_value::timestamp > (CURRENT_TIMESTAMP AT TIME ZONE 'UTC')"
            " - interval '%d seconds'",
            secs);
   char err[KBRS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", ts_key);
   int within = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW) ? 1 : 0;
   aimee_pg_finalize(st);
   return within;
}

/* Project-keyed transaction-scoped advisory lock, shared by the fence-publish
 * transaction (purge route) and every writer's commit-point check: taking it
 * on both sides serializes "publish fence" against "check fence + commit", so
 * a writer that read no-fence cannot commit after the fence lands. Must be
 * called INSIDE an open transaction (the lock is xact-scoped). Under the
 * sqlite test shim both functions are registered as no-ops — sqlite's
 * single-writer serialization covers the same guarantee. */
int db2_kb_purge_txn_guard(const char *project)
{
   if (!project || !project[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBRS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT pg_advisory_xact_lock(hashtext('aimee_purge:' || ?1))", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_ROW || rc == AIMEE_PG_DONE) ? 0 : -1;
}

/* Write both fence rows, TS ROW FIRST, inside the (possibly already open)
 * caller transaction. Ordering matters: a torn write can then only leave an
 * orphan ts row (harmless — inactive without the identity row), never an
 * identity row without a heartbeat. */
static int kbrs_fence_write_rows(const char *project, const char *generation, const char *purge_id)
{
   char key[320], ts_key[320], value[256];
   kbrs_fence_keys(project, key, sizeof(key), ts_key, sizeof(ts_key));
   snprintf(value, sizeof(value), "%s %s", generation, purge_id);
   if (db2_kb_runtime_state_set_now(ts_key) != 0)
      return -1;
   return db2_kb_runtime_state_set(key, value);
}

int db2_kb_purge_fence_write(const char *project, const char *generation, const char *purge_id)
{
   if (!project || !project[0] || !generation || !generation[0] || !purge_id || !purge_id[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[KBRS_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;
   if (kbrs_fence_write_rows(project, generation, purge_id) != 0 ||
       aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
   {
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   return 0;
}

int db2_kb_purge_fence_acquire(const char *project, const char *generation, const char *purge_id,
                               int takeover, char *cur_gen, size_t gen_cap, char *cur_pid,
                               size_t pid_cap, int *replaced_out)
{
   if (cur_gen && gen_cap)
      cur_gen[0] = '\0';
   if (cur_pid && pid_cap)
      cur_pid[0] = '\0';
   if (replaced_out)
      *replaced_out = 0;
   if (!project || !project[0] || !generation || !generation[0] || !purge_id || !purge_id[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char key[320], ts_key[320];
   kbrs_fence_keys(project, key, sizeof(key), ts_key, sizeof(ts_key));

   char err[KBRS_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;
   /* Guard FIRST: serializes this publish against every writer's
    * guard-then-check commit point. */
   if (db2_kb_purge_txn_guard(project) != 0)
   {
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }

   /* Read-decide-write on the identity row under FOR UPDATE so two
    * concurrent purge-project calls serialize on the same decision. */
   char have_gen[128] = "", have_pid[128] = "";
   int have = 0;
   {
      aimee_pg_stmt_t *st = aimee_pg_prepare(
          conn, "SELECT state_value FROM kb_runtime_state WHERE state_key = ?1 FOR UPDATE", err,
          sizeof(err));
      if (!st)
      {
         aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
         return -1;
      }
      aimee_pg_bind_text(st, "?1", key);
      if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         char value[256] = "";
         const char *v = aimee_pg_column_text(st, 0);
         snprintf(value, sizeof(value), "%s", v ? v : "");
         char *sp = strchr(value, ' ');
         if (sp)
            *sp = '\0';
         snprintf(have_gen, sizeof(have_gen), "%s", value);
         snprintf(have_pid, sizeof(have_pid), "%s", sp ? sp + 1 : "");
         have = 1;
      }
      aimee_pg_finalize(st);
   }
   if (cur_gen && gen_cap)
      snprintf(cur_gen, gen_cap, "%s", have_gen);
   if (cur_pid && pid_cap)
      snprintf(cur_pid, pid_cap, "%s", have_pid);

   int same = have && strcmp(have_gen, generation) == 0 && strcmp(have_pid, purge_id) == 0;
   /* LIVE = heartbeat younger than TTL/3 (2x the expected TTL/6 heartbeat
    * interval). A live foreign fence is refused without takeover. */
   if (have && !same && !takeover && kbrs_fence_ts_within(ts_key, kbrs_fence_ttl_s() / 3))
   {
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return 0;
   }
   if (replaced_out)
      *replaced_out = (have && !same);

   if (kbrs_fence_write_rows(project, generation, purge_id) != 0 ||
       aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
   {
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      if (replaced_out)
         *replaced_out = 0;
      return -1;
   }
   return 1;
}

int db2_kb_purge_fence_read(const char *project, char *gen_out, size_t gen_cap, char *pid_out,
                            size_t pid_cap, int *live_out)
{
   if (gen_out && gen_cap)
      gen_out[0] = '\0';
   if (pid_out && pid_cap)
      pid_out[0] = '\0';
   if (live_out)
      *live_out = 0;
   if (!project || !project[0])
      return -1;

   char key[320], ts_key[320], value[256];
   kbrs_fence_keys(project, key, sizeof(key), ts_key, sizeof(ts_key));
   if (db2_kb_runtime_state_get(key, value, sizeof(value)) != 0)
      return 0;

   char *sp = strchr(value, ' ');
   if (sp)
      *sp = '\0';
   if (gen_out && gen_cap)
      snprintf(gen_out, gen_cap, "%s", value);
   if (pid_out && pid_cap)
      snprintf(pid_out, pid_cap, "%s", sp ? sp + 1 : "");

   /* Liveness bound for the takeover decision: the owning delete op heartbeats
    * at least every TTL/6 seconds, so "younger than 2x the heartbeat interval"
    * is TTL/3 (default 300s of a 900s TTL). */
   if (live_out)
      *live_out = kbrs_fence_ts_within(ts_key, kbrs_fence_ttl_s() / 3);
   return 1;
}

int db2_kb_purge_fence_active(const char *project)
{
   if (!project || !project[0])
      return 0;
   char key[320], ts_key[320], value[256];
   kbrs_fence_keys(project, key, sizeof(key), ts_key, sizeof(ts_key));
   if (db2_kb_runtime_state_get(key, value, sizeof(value)) != 0)
      return 0;
   /* Identity row present but no heartbeat row: FAIL CLOSED (active). The
    * publish path writes the ts row first inside one transaction, so this
    * state cannot arise from a torn write — only from manual surgery — and
    * treating it as active keeps "a partially written fence is never
    * inactive". An OLD heartbeat (row present, past TTL) is still expiry. */
   char ts[64] = "";
   if (db2_kb_runtime_state_get(ts_key, ts, sizeof(ts)) != 0)
      return 1;
   return kbrs_fence_ts_within(ts_key, kbrs_fence_ttl_s());
}

/* Lock the identity row FOR UPDATE inside the caller's open transaction and
 * compare it against the expected "generation purge_id" value in C.
 * Returns 1 exact match (row stays locked until commit/rollback), 0
 * mismatch/absent, -1 error. */
static int kbrs_fence_match_locked(void *conn, const char *key, const char *expected)
{
   char err[KBRS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT state_value FROM kb_runtime_state WHERE state_key = ?1 FOR UPDATE", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", key);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int match = 0;
   if (rc == AIMEE_PG_ROW)
   {
      const char *v = aimee_pg_column_text(st, 0);
      match = (v && strcmp(v, expected) == 0) ? 1 : 0;
   }
   aimee_pg_finalize(st);
   if (rc == AIMEE_PG_ERR)
      return -1;
   return match;
}

/* Shared serialized match-then-mutate for heartbeat (clear=0) and finalize/
 * cancel (clear=1). A bare conditional UPDATE/DELETE with an EXISTS predicate
 * is NOT safe here: under READ COMMITTED row re-evaluation (EvalPlanQual) the
 * statement can block on a concurrent takeover's transaction and then mutate
 * the rows the takeover just rewrote, while its EXISTS subplan still saw the
 * old snapshot. So mirror db2_kb_purge_fence_acquire: one transaction that
 * takes the project advisory lock FIRST, locks the identity row FOR UPDATE,
 * compares in C, and only then mutates. Returns 1 mutated, 0 mismatch/absent,
 * -1 error. */
static int kbrs_fence_mutate_matched(const char *project, const char *generation,
                                     const char *purge_id, int clear)
{
   if (!project || !project[0] || !generation || !purge_id)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char key[320], ts_key[320], expected[256];
   kbrs_fence_keys(project, key, sizeof(key), ts_key, sizeof(ts_key));
   snprintf(expected, sizeof(expected), "%s %s", generation, purge_id);

   char err[KBRS_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;
   if (db2_kb_purge_txn_guard(project) != 0)
   {
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   int match = kbrs_fence_match_locked(conn, key, expected);
   if (match != 1)
   {
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return match; /* 0 mismatch/absent, -1 error */
   }

   int rc;
   if (clear)
   {
      rc = db2_kb_runtime_state_delete(key);
      if (rc == 0)
         rc = db2_kb_runtime_state_delete(ts_key);
   }
   else
   {
      rc = db2_kb_runtime_state_set_now(ts_key);
   }
   if (rc != 0 || aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
   {
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   return 1;
}

int db2_kb_purge_fence_heartbeat(const char *project, const char *generation, const char *purge_id)
{
   return kbrs_fence_mutate_matched(project, generation, purge_id, 0);
}

int db2_kb_purge_fence_clear(const char *project, const char *generation, const char *purge_id)
{
   return kbrs_fence_mutate_matched(project, generation, purge_id, 1);
}
