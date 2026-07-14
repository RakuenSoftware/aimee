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
 * unparseable heartbeat row counts as NOT within — an expired (or partially
 * written) fence is treated as absent so it can never block writers forever. */
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

int db2_kb_purge_fence_write(const char *project, const char *generation, const char *purge_id)
{
   if (!project || !project[0] || !generation || !generation[0] || !purge_id || !purge_id[0])
      return -1;
   char key[320], ts_key[320], value[256];
   kbrs_fence_keys(project, key, sizeof(key), ts_key, sizeof(ts_key));
   snprintf(value, sizeof(value), "%s %s", generation, purge_id);
   if (db2_kb_runtime_state_set(key, value) != 0)
      return -1;
   return db2_kb_runtime_state_set_now(ts_key);
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
   return kbrs_fence_ts_within(ts_key, kbrs_fence_ttl_s());
}

/* Match rule shared by heartbeat/finalize/cancel: BOTH generation and purge_id
 * must equal the stored fence; a displaced owner mismatches and no-ops. */
static int kbrs_fence_matches(const char *project, const char *generation, const char *purge_id)
{
   char gen[128] = "", pid[128] = "";
   if (db2_kb_purge_fence_read(project, gen, sizeof(gen), pid, sizeof(pid), NULL) != 1)
      return 0;
   return generation && purge_id && strcmp(gen, generation) == 0 && strcmp(pid, purge_id) == 0;
}

int db2_kb_purge_fence_heartbeat(const char *project, const char *generation, const char *purge_id)
{
   if (!project || !project[0])
      return -1;
   if (!kbrs_fence_matches(project, generation, purge_id))
      return 0;
   char key[320], ts_key[320];
   kbrs_fence_keys(project, key, sizeof(key), ts_key, sizeof(ts_key));
   return db2_kb_runtime_state_set_now(ts_key) == 0 ? 1 : -1;
}

int db2_kb_purge_fence_clear(const char *project, const char *generation, const char *purge_id)
{
   if (!project || !project[0])
      return -1;
   if (!kbrs_fence_matches(project, generation, purge_id))
      return 0;
   char key[320], ts_key[320];
   kbrs_fence_keys(project, key, sizeof(key), ts_key, sizeof(ts_key));
   int rc = db2_kb_runtime_state_delete(key);
   int rc_ts = db2_kb_runtime_state_delete(ts_key);
   return (rc == 0 && rc_ts == 0) ? 1 : -1;
}
