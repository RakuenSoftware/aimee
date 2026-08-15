/* db2/kb_releases.c: aimee-kb corpus staging — doc_releases accessors (DB2).
 *
 * See docs/proposals/pending/aimee-kb-ingest-api-and-corpus-staging.md */

#include "kb_releases.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "kb_runtime_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int64_t db2_kb_release_create(const char *name)
{
   void *conn = db2_conn();
   if (!conn || !name)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "INSERT INTO doc_releases (name) VALUES (?1)"
                                          " RETURNING id",
                                          err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", name);

   int64_t id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

int db2_kb_release_read(int64_t id, db2_kb_release_t *out)
{
   void *conn = db2_conn();
   if (!conn || !out)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT id, name, state, promoted_at, retired_at,"
                                          " created_at FROM doc_releases WHERE id = ?1",
                                          err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_int64(st, "?1", id);

   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      return -1;
   }

   memset(out, 0, sizeof(*out));
   out->id = aimee_pg_column_int64(st, 0);
   const char *v;
   v = aimee_pg_column_text(st, 1);
   if (v)
      snprintf(out->name, sizeof(out->name), "%s", v);
   v = aimee_pg_column_text(st, 2);
   if (v)
      snprintf(out->state, sizeof(out->state), "%s", v);
   v = aimee_pg_column_text(st, 3);
   if (v)
      snprintf(out->promoted_at, sizeof(out->promoted_at), "%s", v);
   v = aimee_pg_column_text(st, 4);
   if (v)
      snprintf(out->retired_at, sizeof(out->retired_at), "%s", v);
   v = aimee_pg_column_text(st, 5);
   if (v)
      snprintf(out->created_at, sizeof(out->created_at), "%s", v);

   aimee_pg_finalize(st);
   return 0;
}

int db2_kb_release_set_state(int64_t id, const char *state, const char *ts_field)
{
   void *conn = db2_conn();
   if (!conn || !state)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st;

   if (ts_field && strcmp(ts_field, "promoted_at") == 0)
   {
      char ts[32];
      now_utc(ts, sizeof(ts));
      st = aimee_pg_prepare(conn,
                            "UPDATE doc_releases SET state = ?1, promoted_at = ?2 WHERE id = ?3",
                            err, sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_text(st, "?1", state);
      aimee_pg_bind_text(st, "?2", ts);
      aimee_pg_bind_int64(st, "?3", id);
   }
   else if (ts_field && strcmp(ts_field, "retired_at") == 0)
   {
      char ts[32];
      now_utc(ts, sizeof(ts));
      st = aimee_pg_prepare(conn,
                            "UPDATE doc_releases SET state = ?1, retired_at = ?2 WHERE id = ?3",
                            err, sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_text(st, "?1", state);
      aimee_pg_bind_text(st, "?2", ts);
      aimee_pg_bind_int64(st, "?3", id);
   }
   else
   {
      st = aimee_pg_prepare(conn, "UPDATE doc_releases SET state = ?1 WHERE id = ?2", err,
                            sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_text(st, "?1", state);
      aimee_pg_bind_int64(st, "?2", id);
   }

   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_kb_release_promote(int64_t id)
{
   int64_t current_active = db2_kb_release_get_active();
   if (current_active > 0)
   {
      if (db2_kb_release_set_state(current_active, "retired", "retired_at") != 0)
         return -1;
   }

   if (db2_kb_release_set_state(id, "active", "promoted_at") != 0)
      return -1;

   char id_str[32];
   snprintf(id_str, sizeof(id_str), "%lld", (long long)id);
   if (db2_kb_runtime_state_set("active_release_id", id_str) != 0)
      return -1;

   return 0;
}

int db2_kb_release_rollback(int64_t target_id)
{
   if (target_id > 0)
      return db2_kb_release_promote(target_id);

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT id FROM doc_releases WHERE state = 'retired'"
                                          " ORDER BY retired_at DESC LIMIT 1",
                                          err, sizeof(err));
   if (!st)
      return -1;

   int64_t found_id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      found_id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);

   if (found_id <= 0)
      return -1;

   return db2_kb_release_promote(found_id);
}

int64_t db2_kb_release_get_active(void)
{
   char val[32] = "";
   if (db2_kb_runtime_state_get("active_release_id", val, sizeof(val)) != 0 || !val[0])
      return 0;
   int64_t id = (int64_t)atoll(val);
   return id > 0 ? id : 0;
}

int db2_kb_release_add_doc(int64_t release_id, int64_t doc_id)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "INSERT INTO release_docs (release_id, doc_id)"
                                          " VALUES (?1, ?2) ON CONFLICT DO NOTHING",
                                          err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_int64(st, "?1", release_id);
   aimee_pg_bind_int64(st, "?2", doc_id);

   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}
