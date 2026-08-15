/* memory_scenes.c: DB2 scene-clustering domain helpers — Postgres via libpq. */

#include "memory_scenes.h"

#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MS_ERRBUF 256

int db2_memory_scenes_list_recent(db2_memory_scene_row_t *rows, int max)
{
   if (!rows || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT id, workspace_id, turn_count, created_at FROM memory_scenes"
                            " ORDER BY id DESC LIMIT 100";
   char err[MS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;

   int count = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && count < max)
   {
      db2_memory_scene_row_t *out = &rows[count++];
      memset(out, 0, sizeof(*out));
      out->id = aimee_pg_column_int64(st, 0);
      const char *ws = aimee_pg_column_text(st, 1);
      snprintf(out->workspace_id, sizeof(out->workspace_id), "%s", ws ? ws : "");
      out->turn_count = aimee_pg_column_int(st, 2);
      const char *ca = aimee_pg_column_text(st, 3);
      snprintf(out->created_at, sizeof(out->created_at), "%s", ca ? ca : "");
   }
   aimee_pg_finalize(st);
   return count;
}

int db2_memory_scene_members(int64_t scene_id, db2_memory_scene_member_t *rows, int max)
{
   if (!rows || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT msm.memory_id, m.key, msm.membership_strength"
                            " FROM memory_scene_members msm"
                            " JOIN memories m ON m.id = msm.memory_id"
                            " WHERE msm.scene_id = ?1"
                            " ORDER BY msm.membership_strength DESC";
   char err[MS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", scene_id);

   int count = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && count < max)
   {
      db2_memory_scene_member_t *out = &rows[count++];
      memset(out, 0, sizeof(*out));
      out->memory_id = aimee_pg_column_int64(st, 0);
      const char *key = aimee_pg_column_text(st, 1);
      snprintf(out->key, sizeof(out->key), "%s", key ? key : "");
      out->membership_strength = aimee_pg_column_double(st, 2);
   }
   aimee_pg_finalize(st);
   return count;
}

int db2_memory_scene_memberships_for_memory(int64_t memory_id, db2_memory_scene_membership_t *rows,
                                            int max)
{
   if (!rows || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql =
       "SELECT scene_id, membership_strength FROM memory_scene_members WHERE memory_id = ?1";
   char err[MS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      rows[count].scene_id = aimee_pg_column_int64(st, 0);
      rows[count].membership_strength = aimee_pg_column_double(st, 1);
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

int db2_memory_scene_member_exists(int64_t memory_id, int64_t scene_id)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql =
       "SELECT 1 FROM memory_scene_members WHERE memory_id = ?1 AND scene_id = ?2 LIMIT 1";
   char err[MS_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_int64(st, "?2", scene_id);
   int hit = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW) ? 1 : 0;
   aimee_pg_finalize(st);
   return hit;
}
