/* db2/features.c: feature_rows for ranking and detection.
 * See
 * docs/proposals/accepted/statistical-decision-systems-for-ranking-calibration-and-experiments.md
 */

#include "feature_rows.h"
#include "artifacts.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "aimee.h"

#include <stdio.h>
#include <string.h>

int db2_feature_row_upsert(const char *subject_id, const char *subject_kind, const char *scope_kind,
                           const char *scope_id, const char *feature_set_version,
                           const char *features_json, const char *computed_at)
{
   if (!subject_id || !subject_kind || !feature_set_version || !features_json)
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char ts[32];
   if (!computed_at || !computed_at[0])
      now_utc(ts, sizeof(ts));
   else
      snprintf(ts, sizeof(ts), "%s", computed_at);

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "INSERT INTO feature_rows"
       "  (subject_id, subject_kind, scope_kind, scope_id, feature_set_version, features, "
       "computed_at)"
       "  VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)"
       "  ON CONFLICT (subject_id, subject_kind, feature_set_version)"
       "  DO UPDATE SET features = EXCLUDED.features, computed_at = EXCLUDED.computed_at,"
       "               scope_kind = EXCLUDED.scope_kind, scope_id = EXCLUDED.scope_id",
       err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", subject_id);
   aimee_pg_bind_text(st, "?2", subject_kind);
   aimee_pg_bind_text(st, "?3", scope_kind ? scope_kind : "");
   aimee_pg_bind_text(st, "?4", scope_id ? scope_id : "");
   aimee_pg_bind_text(st, "?5", feature_set_version);
   aimee_pg_bind_text(st, "?6", features_json);
   aimee_pg_bind_text(st, "?7", ts);
   aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return 0;
}

int db2_feature_row_read(const char *subject_id, const char *subject_kind,
                         const char *feature_set_version, char *buf, size_t len)
{
   if (!subject_id || !subject_kind || !feature_set_version || !buf || len == 0)
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT features FROM feature_rows"
                                          "  WHERE subject_id = ?1 AND subject_kind = ?2"
                                          "    AND feature_set_version = ?3",
                                          err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", subject_id);
   aimee_pg_bind_text(st, "?2", subject_kind);
   aimee_pg_bind_text(st, "?3", feature_set_version);

   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *payload = aimee_pg_column_text(st, 0);
      if (payload)
      {
         snprintf(buf, len, "%s", payload);
         found = 1;
      }
   }
   aimee_pg_finalize(st);
   return found ? 0 : -1;
}
