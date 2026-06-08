/* db2/bandit.c: DB2 accessors for contextual bandit decision logs and arm stats.
 * See
 * docs/proposals/accepted/contextual-bandits-and-counterfactual-replay.md
 */

#include "bandit.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "aimee.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

int db2_bandit_decision_insert(const char *id, const char *decision_point, const char *arm_id,
                               const char *context_hash, double propensity, int is_exploration)
{
   if (!id || !decision_point || !arm_id)
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO bandit_decisions"
                        "  (id, decision_point, arm_id, context_hash, propensity, decided_at,"
                        "   is_exploration)"
                        "  VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)"
                        "  ON CONFLICT (id) DO NOTHING",
                        err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", id);
   aimee_pg_bind_text(st, "?2", decision_point);
   aimee_pg_bind_text(st, "?3", arm_id);
   aimee_pg_bind_text(st, "?4", context_hash ? context_hash : "");
   aimee_pg_bind_double(st, "?5", propensity);
   aimee_pg_bind_text(st, "?6", ts);
   aimee_pg_bind_double(st, "?7", is_exploration ? 1.0 : 0.0);
   aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return 0;
}

int db2_bandit_explore_stats(const char *decision_point, int window_seconds,
                             long long *n_explore_out, long long *n_total_out)
{
   if (n_explore_out)
      *n_explore_out = 0;
   if (n_total_out)
      *n_total_out = 0;
   if (!decision_point)
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* Filter on decided_at >= cutoff. ISO-8601 UTC strings sort
    * lexicographically. window_seconds <= 0 means "any time" — we leave cutoff
    * empty (every string compares >= ''). */
   char cutoff[32] = "";
   if (window_seconds > 0)
   {
      time_t t = time(NULL) - (time_t)window_seconds;
      struct tm gm;
      gmtime_r(&t, &gm);
      strftime(cutoff, sizeof(cutoff), "%Y-%m-%dT%H:%M:%SZ", &gm);
   }

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT COUNT(*),"
                                          "       SUM(CASE WHEN is_exploration THEN 1 ELSE 0 END)"
                                          "  FROM bandit_decisions"
                                          " WHERE decision_point = ?1 AND decided_at >= ?2",
                                          err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", decision_point);
   aimee_pg_bind_text(st, "?2", cutoff);

   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      long long n_total = (long long)aimee_pg_column_double(st, 0);
      long long n_explore = (long long)aimee_pg_column_double(st, 1);
      if (n_total_out)
         *n_total_out = n_total;
      if (n_explore_out)
         *n_explore_out = n_explore;
   }
   aimee_pg_finalize(st);
   return 0;
}

int db2_bandit_decision_close(const char *id, double reward)
{
   if (!id)
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "UPDATE bandit_decisions"
                                          "   SET reward = ?1, closed_at = ?2"
                                          " WHERE id = ?3",
                                          err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_double(st, "?1", reward);
   aimee_pg_bind_text(st, "?2", ts);
   aimee_pg_bind_text(st, "?3", id);
   aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return 0;
}

int db2_bandit_arm_stats_read(const char *decision_point, const char *arm_id,
                              db2_bandit_arm_stats_t *out)
{
   if (!decision_point || !arm_id || !out)
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT n_decisions, n_rewards, sum_reward, posterior_alpha, posterior_beta"
                        "  FROM bandit_arm_stats"
                        " WHERE decision_point = ?1 AND arm_id = ?2",
                        err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", decision_point);
   aimee_pg_bind_text(st, "?2", arm_id);

   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out->n_decisions = (long long)aimee_pg_column_double(st, 0);
      out->n_rewards = (long long)aimee_pg_column_double(st, 1);
      out->sum_reward = aimee_pg_column_double(st, 2);
      out->posterior_alpha = aimee_pg_column_double(st, 3);
      out->posterior_beta = aimee_pg_column_double(st, 4);
      found = 1;
   }
   else
   {
      /* Defaults: uninformative Beta(1,1) prior */
      out->n_decisions = 0;
      out->n_rewards = 0;
      out->sum_reward = 0.0;
      out->posterior_alpha = 1.0;
      out->posterior_beta = 1.0;
      found = 1;
   }
   aimee_pg_finalize(st);
   return found ? 0 : -1;
}

int db2_bandit_arm_stats_update(const char *decision_point, const char *arm_id, double reward_delta,
                                double posterior_alpha, double posterior_beta)
{
   if (!decision_point || !arm_id)
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "INSERT INTO bandit_arm_stats"
       "  (decision_point, arm_id, n_decisions, n_rewards, sum_reward, sum_reward_sq,"
       "   posterior_alpha, posterior_beta, updated_at)"
       "  VALUES (?1, ?2, 1, 1, ?3, ?3*?3, ?4, ?5, ?6)"
       "  ON CONFLICT (decision_point, arm_id) DO UPDATE"
       "    SET n_decisions = bandit_arm_stats.n_decisions + 1,"
       "        n_rewards   = bandit_arm_stats.n_rewards + 1,"
       "        sum_reward  = bandit_arm_stats.sum_reward + ?3,"
       "        sum_reward_sq = bandit_arm_stats.sum_reward_sq + ?3*?3,"
       "        posterior_alpha = ?4,"
       "        posterior_beta  = ?5,"
       "        updated_at      = ?6",
       err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", decision_point);
   aimee_pg_bind_text(st, "?2", arm_id);
   aimee_pg_bind_double(st, "?3", reward_delta);
   aimee_pg_bind_double(st, "?4", posterior_alpha);
   aimee_pg_bind_double(st, "?5", posterior_beta);
   aimee_pg_bind_text(st, "?6", ts);
   aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return 0;
}

/* Emit the single TEXT column of a prepared statement as a JSON array of
 * strings.  Identifiers (decision points / arm ids) are code-controlled and
 * contain no quotes, mirroring the no-escape convention of the export below. */
static int bandit_emit_string_array(aimee_pg_stmt_t *st, char *buf, size_t len)
{
   size_t pos = 0;
   buf[pos++] = '[';
   char err[256] = "";
   int first = 1;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *v = aimee_pg_column_text(st, 0);
      if (!v || !v[0])
         continue;
      if (pos + strlen(v) + 4 >= len - 2)
         break;
      if (!first && pos < len - 2)
         buf[pos++] = ',';
      first = 0;
      int written = snprintf(buf + pos, len - pos, "\"%s\"", v);
      if (written > 0 && (size_t)written < len - pos)
         pos += (size_t)written;
   }
   if (pos < len - 2)
   {
      buf[pos++] = ']';
      buf[pos] = '\0';
   }
   return 0;
}

int db2_bandit_decision_points_list(char *buf, size_t len)
{
   if (!buf || len < 3)
      return -1;
   buf[0] = '[';
   buf[1] = ']';
   buf[2] = '\0';

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT decision_point"
                                          "  FROM bandit_decisions"
                                          " GROUP BY decision_point"
                                          " ORDER BY MAX(decided_at) DESC"
                                          " LIMIT 64",
                                          err, sizeof(err));
   if (!st)
      return 0;
   bandit_emit_string_array(st, buf, len);
   aimee_pg_finalize(st);
   return 0;
}

int db2_bandit_arms_list(const char *decision_point, char *buf, size_t len)
{
   if (!decision_point || !buf || len < 3)
      return -1;
   buf[0] = '[';
   buf[1] = ']';
   buf[2] = '\0';

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT DISTINCT arm_id"
                                          "  FROM bandit_decisions"
                                          " WHERE decision_point = ?1"
                                          " ORDER BY arm_id",
                                          err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", decision_point);
   bandit_emit_string_array(st, buf, len);
   aimee_pg_finalize(st);
   return 0;
}

int db2_bandit_promotion_get(const char *decision_point, char *arm_out, size_t arm_out_len)
{
   if (arm_out && arm_out_len)
      arm_out[0] = '\0';
   if (!decision_point || !arm_out || arm_out_len == 0)
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT arm_id FROM bandit_promotions"
                                          " WHERE decision_point = ?1",
                                          err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", decision_point);
   int found = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *arm = aimee_pg_column_text(st, 0);
      if (arm && arm[0])
      {
         snprintf(arm_out, arm_out_len, "%s", arm);
         found = 0;
      }
   }
   aimee_pg_finalize(st);
   return found;
}

int db2_bandit_promotion_set(const char *decision_point, const char *arm_id,
                             const char *rollback_arm)
{
   if (!decision_point || !arm_id || !arm_id[0])
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO bandit_promotions (decision_point, arm_id, rollback_arm,"
                        "   promoted_at)"
                        "  VALUES (?1, ?2, ?3, ?4)"
                        "  ON CONFLICT (decision_point) DO UPDATE"
                        "    SET arm_id = ?2, rollback_arm = ?3, promoted_at = ?4",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", decision_point);
   aimee_pg_bind_text(st, "?2", arm_id);
   aimee_pg_bind_text(st, "?3", rollback_arm ? rollback_arm : "");
   aimee_pg_bind_text(st, "?4", ts);
   aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return 0;
}

int db2_bandit_decisions_export(const char *decision_point, int limit, char *buf, size_t len)
{
   if (!decision_point || !buf || len == 0)
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   if (limit <= 0 || limit > 10000)
      limit = 1000;

   char lim_str[16];
   snprintf(lim_str, sizeof(lim_str), "%d", limit);

   char err[256] = "";
   /* Build JSON array manually */
   size_t pos = 0;
   buf[pos++] = '[';

   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT id, arm_id, context_hash, propensity::text, reward::text, decided_at"
       "  FROM bandit_decisions"
       " WHERE decision_point = ?1 AND closed_at != '' AND reward IS NOT NULL"
       " ORDER BY decided_at DESC"
       " LIMIT ?2",
       err, sizeof(err));
   if (!st)
   {
      buf[0] = '[';
      buf[1] = ']';
      buf[2] = '\0';
      return 0;
   }

   aimee_pg_bind_text(st, "?1", decision_point);
   aimee_pg_bind_text(st, "?2", lim_str);

   int first = 1;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (pos >= len - 128)
         break;
      if (!first && pos < len - 2)
         buf[pos++] = ',';
      first = 0;

      const char *id_v = aimee_pg_column_text(st, 0);
      const char *arm_v = aimee_pg_column_text(st, 1);
      const char *ctx_v = aimee_pg_column_text(st, 2);
      const char *prop_v = aimee_pg_column_text(st, 3);
      const char *rew_v = aimee_pg_column_text(st, 4);
      const char *ts_v = aimee_pg_column_text(st, 5);

      int written = snprintf(buf + pos, len - pos,
                             "{\"id\":\"%s\",\"arm_id\":\"%s\",\"context_hash\":\"%s\","
                             "\"propensity\":%s,\"reward\":%s,\"decided_at\":\"%s\"}",
                             id_v ? id_v : "", arm_v ? arm_v : "", ctx_v ? ctx_v : "",
                             prop_v ? prop_v : "1.0", rew_v ? rew_v : "0.0", ts_v ? ts_v : "");
      if (written > 0 && (size_t)written < len - pos)
         pos += (size_t)written;
   }
   aimee_pg_finalize(st);

   if (pos < len - 2)
   {
      buf[pos++] = ']';
      buf[pos] = '\0';
   }
   return 0;
}
