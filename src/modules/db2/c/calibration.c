/* db2/calibration.c: Bayesian promotion-threshold calibration profiles.
 * See docs/proposals/done/bayesian-promotion-threshold-calibration.md */

#include "calibration.h"
#include "artifacts.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "aimee.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

static double calibration_clamp01(double v)
{
   if (v < 0.0)
      return 0.0;
   if (v > 1.0)
      return 1.0;
   return v;
}

int db2_calibration_threshold_from_profile_json(const char *payload_json, double static_threshold,
                                                double *threshold_out)
{
   if (!payload_json || !threshold_out)
      return -1;

   cJSON *root = cJSON_Parse(payload_json);
   if (!root)
      return -1;

   double threshold = calibration_clamp01(static_threshold);
   double conformal_floor = 0.0;

   cJSON *conf = cJSON_GetObjectItemCaseSensitive(root, "conformal");
   cJSON *rb = conf ? cJSON_GetObjectItemCaseSensitive(conf, "reject_below") : NULL;
   if (cJSON_IsNumber(rb))
      conformal_floor = calibration_clamp01(rb->valuedouble);

   cJSON *buckets = cJSON_GetObjectItemCaseSensitive(root, "buckets");
   if (cJSON_IsArray(buckets))
   {
      cJSON *bucket = NULL;
      double best = -1.0;
      cJSON_ArrayForEach(bucket, buckets)
      {
         cJSON *range = cJSON_GetObjectItemCaseSensitive(bucket, "range");
         cJSON *lo_j = cJSON_IsArray(range) ? cJSON_GetArrayItem(range, 0) : NULL;
         cJSON *bound_j = cJSON_GetObjectItemCaseSensitive(bucket, "lower_credible_bound");
         double bound = -1.0;
         if (cJSON_IsNumber(bound_j))
         {
            bound = bound_j->valuedouble;
         }
         else
         {
            cJSON *alpha_j = cJSON_GetObjectItemCaseSensitive(bucket, "alpha");
            cJSON *beta_j = cJSON_GetObjectItemCaseSensitive(bucket, "beta");
            if (cJSON_IsNumber(alpha_j) && cJSON_IsNumber(beta_j) &&
                alpha_j->valuedouble + beta_j->valuedouble > 0.0)
               bound = alpha_j->valuedouble / (alpha_j->valuedouble + beta_j->valuedouble);
         }

         if (cJSON_IsNumber(lo_j) && bound >= static_threshold)
         {
            double lo = calibration_clamp01(lo_j->valuedouble);
            if (best < 0.0 || lo < best)
               best = lo;
         }
      }
      if (best >= 0.0)
         threshold = best;
   }

   if (conformal_floor > threshold)
      threshold = conformal_floor;
   *threshold_out = calibration_clamp01(threshold);
   cJSON_Delete(root);
   return 0;
}

int db2_calibration_profile_write(const char *target_surface, const char *kind,
                                  const char *scope_kind, const char *scope_id,
                                  const char *feature_set_version, const char *payload_json,
                                  char *id_out, int id_out_len)
{
   if (!target_surface || !kind || !payload_json)
      return -1;

   char id[64];
   db2_artifact_gen_id(id, sizeof(id));

   /* Build a descriptor we can store in scope_id to allow retrieval by surface+kind */
   char ext_scope_id[256];
   if (scope_id && scope_id[0])
      snprintf(ext_scope_id, sizeof(ext_scope_id), "%s", scope_id);
   else
      ext_scope_id[0] = '\0';

   int rc =
       db2_artifact_write(id, "calibration_profile", "committed",
                          scope_kind ? scope_kind : "global", ext_scope_id, "", 1.0, payload_json);
   if (rc != 0)
      return -1;

   /* Stamp target_surface, kind, and feature_set_version via UPDATE so the
    * fixed-column write above stays clean. */
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "UPDATE artifacts"
                                          " SET target_surface = ?1,"
                                          "     model_version   = ?2,"
                                          "     prompt_version  = ?3,"
                                          "     committed_at    = ?4"
                                          " WHERE id = ?5",
                                          err, sizeof(err));
   if (!st)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));
   aimee_pg_bind_text(st, "?1", target_surface);
   aimee_pg_bind_text(st, "?2", kind);
   aimee_pg_bind_text(st, "?3", feature_set_version ? feature_set_version : "v1");
   aimee_pg_bind_text(st, "?4", ts);
   aimee_pg_bind_text(st, "?5", id);
   aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);

   if (id_out && id_out_len > 0)
      snprintf(id_out, (size_t)id_out_len, "%s", id);

   return 0;
}

/* Narrowest-scope fallback helper — tries exact scope, then scope_kind only,
 * then global ("" scope_kind). */
static int try_read(void *conn, const char *target_surface, const char *kind,
                    const char *scope_kind, const char *scope_id, char *buf, size_t len)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT id, payload FROM artifacts"
                                          " WHERE kind = 'calibration_profile'"
                                          "   AND target_surface = ?1"
                                          "   AND model_version  = ?2"
                                          "   AND scope_kind     = ?3"
                                          "   AND scope_id       = ?4"
                                          "   AND state          = 'committed'"
                                          " ORDER BY committed_at DESC"
                                          " LIMIT 1",
                                          err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", target_surface);
   aimee_pg_bind_text(st, "?2", kind);
   aimee_pg_bind_text(st, "?3", scope_kind ? scope_kind : "");
   aimee_pg_bind_text(st, "?4", scope_id ? scope_id : "");

   int found = 0;
   char found_id[64] = "";
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *id = aimee_pg_column_text(st, 0);
      const char *payload = aimee_pg_column_text(st, 1);
      if (payload)
      {
         snprintf(buf, len, "%s", payload);
         snprintf(found_id, sizeof(found_id), "%s", id ? id : "");
         found = 1;
      }
   }
   aimee_pg_finalize(st);
   if (found && found_id[0])
      db2_artifact_touch(found_id);
   return found ? 0 : -1;
}

int db2_calibration_profile_read(const char *target_surface, const char *kind,
                                 const char *scope_kind, const char *scope_id, char *buf,
                                 size_t len)
{
   void *conn = db2_conn();
   if (!conn || !target_surface || !kind || !buf || len == 0)
      return -1;

   /* 1. Exact scope */
   if (try_read(conn, target_surface, kind, scope_kind, scope_id, buf, len) == 0)
      return 0;

   /* 2. Scope-kind only (drop scope_id) */
   if (scope_id && scope_id[0] &&
       try_read(conn, target_surface, kind, scope_kind, "", buf, len) == 0)
      return 0;

   /* 3. Global fallback */
   if (scope_kind && scope_kind[0] &&
       try_read(conn, target_surface, kind, "global", "", buf, len) == 0)
      return 0;

   return -1;
}

int db2_calibration_audit_stats(const char *target_surface, const char *kind,
                                const char *scope_kind, const char *scope_id, int window_rows,
                                db2_calibration_bucket_t *buckets, int max_buckets)
{
   if (!target_surface || !kind || !buckets || max_buckets < 1)
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* For each bucket b in [0..K-1], bucket b covers confidence range
    * [b/K, (b+1)/K).  We use integer division on applied_confidence * K
    * to assign rows to buckets. */
   if (max_buckets > DB2_CALIBRATION_BUCKETS)
      max_buckets = DB2_CALIBRATION_BUCKETS;

   /* Initialize buckets */
   for (int i = 0; i < max_buckets; i++)
   {
      buckets[i].range_lo = (double)i / max_buckets;
      buckets[i].range_hi = (double)(i + 1) / max_buckets;
      buckets[i].alpha = 0.0;
      buckets[i].beta = 0.0;
      buckets[i].sample_n = 0;
   }

   char sql[1400] = "";
   /* We build the SQL with scope conditions inline — safe because all values
    * come from trusted server-side config, not user input. */
   const char *scope_filter = "";
   if (scope_kind && scope_kind[0] && scope_id && scope_id[0])
      scope_filter = " AND ae.scope_kind = ?3 AND ae.scope_id = ?4";
   else if (scope_kind && scope_kind[0])
      scope_filter = " AND ae.scope_kind = ?3";

   if (window_rows > 0)
   {
      snprintf(sql, sizeof(sql),
               "WITH recent AS ("
               "  SELECT ae.applied_confidence, ae.verdict FROM audit_events ae"
               "  JOIN artifacts a ON a.id = ae.source_artifact_id"
               "  WHERE a.kind = ?1"
               "    AND ae.target_surface = ?2"
               "    AND ae.verdict <> ''"
               "%s"
               "  ORDER BY ae.applied_at DESC"
               "  LIMIT ?6"
               ")"
               "SELECT"
               "  CASE"
               "    WHEN applied_confidence >= 1.0 THEN ?5 - 1"
               "    WHEN applied_confidence < 0.0 THEN 0"
               "    ELSE CAST(applied_confidence * ?5 AS INT)"
               "  END AS bucket,"
               "  SUM(CASE WHEN verdict = 'accepted' THEN 1 ELSE 0 END) AS n_accepted,"
               "  SUM(CASE WHEN verdict = 'rejected' THEN 1 ELSE 0 END) AS n_rejected,"
               "  COUNT(*) AS n_total"
               " FROM recent"
               " GROUP BY bucket"
               " ORDER BY 1",
               scope_filter);
   }
   else
   {
      snprintf(sql, sizeof(sql),
               "SELECT"
               "  CASE"
               "    WHEN ae.applied_confidence >= 1.0 THEN ?5 - 1"
               "    WHEN ae.applied_confidence < 0.0 THEN 0"
               "    ELSE CAST(ae.applied_confidence * ?5 AS INT)"
               "  END AS bucket,"
               "  SUM(CASE WHEN ae.verdict = 'accepted' THEN 1 ELSE 0 END) AS n_accepted,"
               "  SUM(CASE WHEN ae.verdict = 'rejected' THEN 1 ELSE 0 END) AS n_rejected,"
               "  COUNT(*) AS n_total"
               " FROM audit_events ae"
               " JOIN artifacts a ON a.id = ae.source_artifact_id"
               " WHERE a.kind = ?1"
               "   AND ae.target_surface = ?2"
               "   AND ae.verdict <> ''"
               "%s"
               " GROUP BY bucket"
               " ORDER BY 1"
               " LIMIT ?6",
               scope_filter);
   }

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", kind);
   aimee_pg_bind_text(st, "?2", target_surface);
   if (scope_kind && scope_kind[0])
      aimee_pg_bind_text(st, "?3", scope_kind);
   if (scope_kind && scope_kind[0] && scope_id && scope_id[0])
      aimee_pg_bind_text(st, "?4", scope_id);
   aimee_pg_bind_int(st, "?5", max_buckets);
   aimee_pg_bind_int(st, "?6", window_rows > 0 ? window_rows : 10000);

   int filled = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      int bucket = aimee_pg_column_int(st, 0);
      int n_acc = aimee_pg_column_int(st, 1);
      int n_rej = aimee_pg_column_int(st, 2);
      int n_tot = aimee_pg_column_int(st, 3);

      if (bucket >= 0 && bucket < max_buckets)
      {
         buckets[bucket].alpha = (double)n_acc;
         buckets[bucket].beta = (double)n_rej;
         buckets[bucket].sample_n = n_tot;
         filled++;
      }
   }
   aimee_pg_finalize(st);
   return filled;
}

int db2_calibration_conformal_window(const char *target_surface, const char *kind,
                                     const char *scope_kind, const char *scope_id, int window_rows,
                                     db2_calibration_conformal_row_t *rows, int max_rows)
{
   if (!target_surface || !kind || !rows || max_rows < 1)
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   if (max_rows > DB2_CALIBRATION_CONFORMAL_MAX)
      max_rows = DB2_CALIBRATION_CONFORMAL_MAX;
   int limit = window_rows > 0 ? window_rows : max_rows;
   if (limit > max_rows)
      limit = max_rows;

   const char *scope_filter = "";
   if (scope_kind && scope_kind[0] && scope_id && scope_id[0])
      scope_filter = " AND ae.scope_kind = ?3 AND ae.scope_id = ?4";
   else if (scope_kind && scope_kind[0])
      scope_filter = " AND ae.scope_kind = ?3";

   char sql[1024];
   snprintf(sql, sizeof(sql),
            "SELECT ae.applied_confidence, ae.verdict"
            " FROM audit_events ae"
            " JOIN artifacts a ON a.id = ae.source_artifact_id"
            " WHERE a.kind = ?1"
            "   AND ae.target_surface = ?2"
            "   AND ae.verdict IN ('accepted','rejected')"
            "%s"
            " ORDER BY ae.applied_at DESC"
            " LIMIT ?5",
            scope_filter);

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", kind);
   aimee_pg_bind_text(st, "?2", target_surface);
   if (scope_kind && scope_kind[0])
      aimee_pg_bind_text(st, "?3", scope_kind);
   if (scope_kind && scope_kind[0] && scope_id && scope_id[0])
      aimee_pg_bind_text(st, "?4", scope_id);
   aimee_pg_bind_int(st, "?5", limit);

   int n = 0;
   while (n < max_rows && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      rows[n].applied_confidence = aimee_pg_column_double(st, 0);
      const char *verdict = aimee_pg_column_text(st, 1);
      snprintf(rows[n].verdict, sizeof(rows[n].verdict), "%s", verdict ? verdict : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_calibration_surfaces_with_data(int min_rows)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT COUNT(*) FROM ("
                        "  SELECT ae.target_surface, a.kind,"
                        "         ae.scope_kind, ae.scope_id"
                        "  FROM audit_events ae"
                        "  JOIN artifacts a ON a.id = ae.source_artifact_id"
                        "  WHERE ae.verdict <> ''"
                        "  GROUP BY ae.target_surface, a.kind, ae.scope_kind, ae.scope_id"
                        "  HAVING COUNT(*) >= ?1"
                        ") AS t",
                        err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_int(st, "?1", min_rows > 0 ? min_rows : 1);

   int count = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      count = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return count;
}

int db2_calibration_surface_list(int min_rows, db2_calibration_surface_t *out, int max_out)
{
   void *conn = db2_conn();
   if (!conn || !out || max_out <= 0)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT ae.target_surface, a.kind,"
                        "       COALESCE(ae.scope_kind, ''), COALESCE(ae.scope_id, ''),"
                        "       COUNT(*) AS n"
                        "  FROM audit_events ae"
                        "  JOIN artifacts a ON a.id = ae.source_artifact_id"
                        " WHERE ae.verdict <> ''"
                        " GROUP BY ae.target_surface, a.kind, ae.scope_kind, ae.scope_id"
                        " HAVING COUNT(*) >= ?1"
                        " ORDER BY n DESC, ae.target_surface, a.kind"
                        " LIMIT ?2",
                        err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_int(st, "?1", min_rows > 0 ? min_rows : 1);
   aimee_pg_bind_int(st, "?2", max_out);

   int n = 0;
   while (n < max_out && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *surface = aimee_pg_column_text(st, 0);
      const char *kind = aimee_pg_column_text(st, 1);
      const char *scope_kind = aimee_pg_column_text(st, 2);
      const char *scope_id = aimee_pg_column_text(st, 3);
      snprintf(out[n].target_surface, sizeof(out[n].target_surface), "%s", surface ? surface : "");
      snprintf(out[n].kind, sizeof(out[n].kind), "%s", kind ? kind : "");
      snprintf(out[n].scope_kind, sizeof(out[n].scope_kind), "%s", scope_kind ? scope_kind : "");
      snprintf(out[n].scope_id, sizeof(out[n].scope_id), "%s", scope_id ? scope_id : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}
