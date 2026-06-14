/* db2/demotion.c: retrieval attribution evidence and demotion profiles.
 * See docs/proposals/done/outcome-driven-demotion-and-poison-resilience.md */

#include "demotion.h"
#include "artifacts.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "aimee.h"

#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Build surfaced_ids JSON array string into buf. */
static void build_ids_array(const int64_t *ids, int n, char *buf, size_t len)
{
   size_t off = 0;
   buf[off++] = '[';
   for (int i = 0; i < n && off < len - 32; i++)
   {
      if (i > 0)
         buf[off++] = ',';
      off += (size_t)snprintf(buf + off, len - off, "%lld", (long long)ids[i]);
   }
   if (off < len - 1)
      buf[off++] = ']';
   buf[off] = '\0';
}

int db2_demotion_retrieval_event_write(const char *query_fingerprint, const char *role,
                                       const int64_t *surfaced_ids, int n_surfaced, char *id_out,
                                       int id_out_len)
{
   char id[64];
   db2_artifact_gen_id(id, sizeof(id));

   char ids_buf[4096];
   build_ids_array(surfaced_ids ? surfaced_ids : NULL, surfaced_ids ? n_surfaced : 0, ids_buf,
                   sizeof(ids_buf));

   char payload[8192];
   snprintf(payload, sizeof(payload),
            "{\"query_fingerprint\":\"%s\",\"role\":\"%s\",\"surfaced_ids\":%s}",
            query_fingerprint ? query_fingerprint : "", role ? role : "", ids_buf);

   int rc = db2_artifact_write(id, "retrieval_event", "proposed", "system", "", "", 1.0, payload);
   if (rc != 0)
      return -1;

   if (id_out && id_out_len > 0)
      snprintf(id_out, (size_t)id_out_len, "%s", id);
   return 0;
}

int db2_demotion_retrieval_event_write_turn(const char *turn_id, const char *query_fingerprint,
                                            const char *role, const int64_t *surfaced_ids,
                                            int n_surfaced, char *id_out, int id_out_len)
{
   char id[64];
   if (db2_demotion_retrieval_event_write(query_fingerprint, role, surfaced_ids, n_surfaced, id,
                                          sizeof(id)) != 0)
      return -1;

   /* Stamp the caller-visible turn_id (single follow-up UPDATE, like the
    * attribution writer stamps model_version). The partial unique index rejects a
    * duplicate turn_id — tolerable: the event row still exists un-stamped and the
    * first turn-stamped event remains authoritative (P1 single-writer; the
    * idempotent merge is P1.5). */
   if (turn_id && turn_id[0])
   {
      void *conn = db2_conn();
      if (conn)
      {
         char err[256] = "";
         aimee_pg_stmt_t *st = aimee_pg_prepare(
             conn, "UPDATE artifacts SET turn_id = ?1 WHERE id = ?2", err, sizeof(err));
         if (st)
         {
            aimee_pg_bind_text(st, "?1", turn_id);
            aimee_pg_bind_text(st, "?2", id);
            (void)aimee_pg_step(st, err, sizeof(err)); /* conflict -> left un-stamped */
            aimee_pg_finalize(st);
         }
      }
   }

   if (id_out && id_out_len > 0)
      snprintf(id_out, (size_t)id_out_len, "%s", id);
   return 0;
}

int db2_demotion_retrieval_event_by_turn(const char *turn_id, char *id_out, int id_out_len,
                                         char *payload_out, int payload_out_len)
{
   if (id_out && id_out_len > 0)
      id_out[0] = '\0';
   if (payload_out && payload_out_len > 0)
      payload_out[0] = '\0';
   if (!turn_id || !turn_id[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT id, payload FROM artifacts"
                        " WHERE kind = 'retrieval_event' AND turn_id = ?1 LIMIT 1",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", turn_id);
   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      found = 1;
      if (id_out && id_out_len > 0)
         snprintf(id_out, (size_t)id_out_len, "%s", aimee_pg_column_text(st, 0));
      if (payload_out && payload_out_len > 0)
         snprintf(payload_out, (size_t)payload_out_len, "%s", aimee_pg_column_text(st, 1));
   }
   aimee_pg_finalize(st);
   return found;
}

int db2_demotion_retrieval_attribution_write(const char *retrieval_event_id,
                                             int64_t surfaced_row_id, const char *verdict,
                                             double weight)
{
   if (!retrieval_event_id || !verdict)
      return -1;

   char id[64];
   db2_artifact_gen_id(id, sizeof(id));

   /* scope_id = string(surfaced_row_id) for fast lookup by row. */
   char scope_id_buf[32];
   snprintf(scope_id_buf, sizeof(scope_id_buf), "%lld", (long long)surfaced_row_id);

   char payload[512];
   snprintf(payload, sizeof(payload),
            "{\"retrieval_event_id\":\"%s\",\"surfaced_row_id\":%lld,\"verdict\":\"%s\","
            "\"weight\":%.6f}",
            retrieval_event_id, (long long)surfaced_row_id, verdict, weight);

   int rc = db2_artifact_write(id, "retrieval_attribution", "proposed", "memory", scope_id_buf, "",
                               1.0, payload);
   if (rc != 0)
      return -1;

   /* Stamp model_version = retrieval_event_id for FK-style linking queries. */
   void *conn = db2_conn();
   if (!conn)
      return 0; /* written, but can't stamp event link — tolerable */

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE artifacts SET model_version = ?1 WHERE id = ?2", err, sizeof(err));
   if (st)
   {
      aimee_pg_bind_text(st, "?1", retrieval_event_id);
      aimee_pg_bind_text(st, "?2", id);
      aimee_pg_step(st, err, sizeof(err));
      aimee_pg_finalize(st);
   }
   return 0;
}

/* Parse a "YYYY-MM-DD HH:MM:SS" UTC timestamp string and return seconds since
 * epoch.  Returns 0 on parse failure (treated as ancient; no decay impact). */
static time_t parse_utc_ts(const char *ts)
{
   if (!ts || !*ts)
      return 0;
   struct tm t;
   memset(&t, 0, sizeof(t));
   int n = sscanf(ts, "%d-%d-%d %d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min,
                  &t.tm_sec);
   if (n < 3)
      return 0;
   t.tm_year -= 1900;
   t.tm_mon -= 1;
   t.tm_isdst = 0;
#ifdef _WIN32
   return _mkgmtime(&t);
#else
   return timegm(&t);
#endif
}

/* Verdict contribution: accepted → +w, negative verdicts → -w, irrelevant → 0. */
static double verdict_sign(const char *verdict)
{
   if (!verdict)
      return 0.0;
   if (strcmp(verdict, DEMOTION_VERDICT_ACCEPTED) == 0)
      return 1.0;
   if (strcmp(verdict, DEMOTION_VERDICT_CORRECTED) == 0 ||
       strcmp(verdict, DEMOTION_VERDICT_CONTRADICTED) == 0 ||
       strcmp(verdict, DEMOTION_VERDICT_ROLLED_BACK) == 0)
      return -1.0;
   return 0.0; /* irrelevant or unknown */
}

double db2_demotion_score(int64_t row_id, int window_size, double half_life_days, int n_min)
{
   if (window_size <= 0)
      window_size = 64;
   if (half_life_days <= 0.0)
      half_life_days = 30.0;
   if (n_min <= 0)
      n_min = 5;

   void *conn = db2_conn();
   if (!conn)
      return NAN;

   char scope_id_buf[32];
   snprintf(scope_id_buf, sizeof(scope_id_buf), "%lld", (long long)row_id);

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT id, payload, created_at FROM artifacts"
                                          " WHERE kind = 'retrieval_attribution' AND scope_id = ?1"
                                          " ORDER BY created_at DESC"
                                          " LIMIT ?2",
                                          err, sizeof(err));
   if (!st)
      return NAN;

   aimee_pg_bind_text(st, "?1", scope_id_buf);
   aimee_pg_bind_int(st, "?2", window_size);

   time_t now = time(NULL);
   double score = 0.0;
   int n_valid = 0;
   double ln2 = 0.693147180559945;
   char(*touched_ids)[64] = calloc((size_t)window_size, sizeof(*touched_ids));
   int touched_n = 0;

   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *artifact_id = aimee_pg_column_text(st, 0);
      const char *payload_str = aimee_pg_column_text(st, 1);
      const char *created_at = aimee_pg_column_text(st, 2);
      if (touched_ids && artifact_id && artifact_id[0] && touched_n < window_size)
         snprintf(touched_ids[touched_n++], sizeof(touched_ids[0]), "%s", artifact_id);

      if (!payload_str)
         continue;

      cJSON *p = cJSON_Parse(payload_str);
      if (!p)
         continue;

      cJSON *jv = cJSON_GetObjectItemCaseSensitive(p, "verdict");
      cJSON *jw = cJSON_GetObjectItemCaseSensitive(p, "weight");

      if (cJSON_IsString(jv))
      {
         double w = cJSON_IsNumber(jw) ? jw->valuedouble : 1.0;
         double sign = verdict_sign(jv->valuestring);

         /* Time decay: exp(-ln2 * age_days / half_life_days). */
         double age_days = 0.0;
         if (created_at)
         {
            time_t ts = parse_utc_ts(created_at);
            if (ts > 0 && now > ts)
               age_days = difftime(now, ts) / 86400.0;
         }
         double decay = exp(-ln2 * age_days / half_life_days);

         score += sign * w * decay;
         n_valid++;
      }
      cJSON_Delete(p);
   }
   aimee_pg_finalize(st);
   /* Scoring consumes retrieval-attribution artifacts; stamp those reads after
    * the SELECT is finalized so temporal maintenance sees active evidence. */
   for (int i = 0; i < touched_n; i++)
      db2_artifact_touch(touched_ids[i]);
   free(touched_ids);

   if (n_valid < n_min)
      return NAN;
   return score;
}

int db2_demotion_profile_write(const char *memory_class, const char *scope_kind,
                               const char *scope_id, const char *payload_json, char *id_out,
                               int id_out_len)
{
   if (!memory_class || !payload_json)
      return -1;

   char id[64];
   db2_artifact_gen_id(id, sizeof(id));

   int rc =
       db2_artifact_write(id, "demotion_profile", "committed", scope_kind ? scope_kind : "global",
                          scope_id ? scope_id : "", "", 1.0, payload_json);
   if (rc != 0)
      return -1;

   /* Stamp target_surface = memory_class and committed_at. */
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "UPDATE artifacts"
                                          " SET target_surface = ?1,"
                                          "     committed_at   = ?2"
                                          " WHERE id = ?3",
                                          err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", memory_class);
   aimee_pg_bind_text(st, "?2", ts);
   aimee_pg_bind_text(st, "?3", id);
   aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);

   if (id_out && id_out_len > 0)
      snprintf(id_out, (size_t)id_out_len, "%s", id);
   return 0;
}

static int demotion_try_read(void *conn, const char *memory_class, const char *scope_kind,
                             const char *scope_id, char *buf, size_t len)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT id, payload FROM artifacts"
                                          " WHERE kind = 'demotion_profile'"
                                          "   AND target_surface = ?1"
                                          "   AND scope_kind     = ?2"
                                          "   AND scope_id       = ?3"
                                          "   AND state          = 'committed'"
                                          " ORDER BY committed_at DESC"
                                          " LIMIT 1",
                                          err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", memory_class);
   aimee_pg_bind_text(st, "?2", scope_kind ? scope_kind : "");
   aimee_pg_bind_text(st, "?3", scope_id ? scope_id : "");

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

int db2_demotion_profile_read(const char *memory_class, const char *scope_kind,
                              const char *scope_id, char *buf, size_t len)
{
   void *conn = db2_conn();
   if (!conn || !memory_class || !buf || len == 0)
      return -1;

   /* 1. Exact scope. */
   if (demotion_try_read(conn, memory_class, scope_kind, scope_id, buf, len) == 0)
      return 0;

   /* 2. Scope-kind only. */
   if (scope_id && scope_id[0] &&
       demotion_try_read(conn, memory_class, scope_kind, "", buf, len) == 0)
      return 0;

   /* 3. Global fallback. */
   if (scope_kind && scope_kind[0] &&
       demotion_try_read(conn, memory_class, "global", "", buf, len) == 0)
      return 0;

   return -1;
}

int db2_demotion_candidates(int n_min, db2_demotion_candidate_t *out, int max)
{
   if (!out || max < 1)
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT scope_id, COUNT(*) AS n"
                                          " FROM artifacts"
                                          " WHERE kind = 'retrieval_attribution'"
                                          " GROUP BY scope_id"
                                          " HAVING COUNT(*) >= ?1"
                                          " LIMIT ?2",
                                          err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_int(st, "?1", n_min > 0 ? n_min : 1);
   aimee_pg_bind_int(st, "?2", max);

   int filled = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && filled < max)
   {
      const char *sid = aimee_pg_column_text(st, 0);
      int n = aimee_pg_column_int(st, 1);
      if (sid && *sid)
      {
         out[filled].row_id = (int64_t)atoll(sid);
         out[filled].attribution_n = n;
         filled++;
      }
   }
   aimee_pg_finalize(st);
   return filled;
}
