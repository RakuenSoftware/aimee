/* roundtable_pipeline_capture.c: server-worker-owned result capture. See
 * roundtable_pipeline_capture.h and the proposal's section 4. */

#include "roundtable_pipeline_capture.h"
#include "roundtable_pipeline.h"

#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/* FNV-1a 64-bit, hex. Stable content hash for result snapshots / pinning. */
static void rtp_hash_hex(const char *s, char *out, size_t out_cap)
{
   unsigned long long h = 1469598103934665603ULL;
   if (s)
   {
      for (const unsigned char *p = (const unsigned char *)s; *p; p++)
      {
         h ^= (unsigned long long)*p;
         h *= 1099511628211ULL;
      }
   }
   snprintf(out, out_cap, "%016llx", h);
}

int rtp_capture_parse(const char *result_json, int is_draft, rtp_envelope_t *out)
{
   if (!out)
      return 0;
   memset(out, 0, sizeof(*out));
   out->is_draft = is_draft ? 1 : 0;

   if (!result_json || !result_json[0])
   {
      out->present = 0; /* nothing captured */
      return 0;
   }
   out->present = 1;

   cJSON *r = cJSON_Parse(result_json);
   if (!r || !cJSON_IsObject(r))
   {
      out->parse_ok = 0;
      cJSON_Delete(r);
      return 0;
   }
   out->parse_ok = 1;

   if (cJSON_GetObjectItemCaseSensitive(r, "error"))
      out->has_error = 1;

   cJSON *art = cJSON_GetObjectItemCaseSensitive(r, "artifact");
   if (cJSON_IsString(art) && art->valuestring && art->valuestring[0])
      out->artifact_present = 1;

   cJSON *it;
#define RTP_GETBOOL(field, key)                                                                    \
   do                                                                                              \
   {                                                                                               \
      it = cJSON_GetObjectItemCaseSensitive(r, key);                                               \
      out->field = cJSON_IsTrue(it) ? 1 : 0;                                                       \
   } while (0)
#define RTP_GETINT(field, key)                                                                     \
   do                                                                                              \
   {                                                                                               \
      it = cJSON_GetObjectItemCaseSensitive(r, key);                                               \
      out->field = cJSON_IsNumber(it) ? (int)it->valuedouble : 0;                                  \
   } while (0)

   RTP_GETBOOL(converged, "converged");
   RTP_GETBOOL(degraded, "degraded");
   RTP_GETBOOL(truncated, "truncated");
   RTP_GETBOOL(items_truncated, "items_truncated");
   RTP_GETBOOL(cost_capped, "cost_capped");
   RTP_GETBOOL(deadline_hit, "deadline_hit");
   RTP_GETBOOL(cancelled, "cancelled");
   RTP_GETINT(items_round, "items_round");
   RTP_GETINT(artifact_round, "artifact_round");
   RTP_GETINT(best_round, "best_round");
   RTP_GETINT(rounds_run, "rounds_run");

   it = cJSON_GetObjectItemCaseSensitive(r, "cost_usd");
   if (cJSON_IsNumber(it))
   {
      out->cost_usd = it->valuedouble;
      out->cost_known = 1;
   }

#undef RTP_GETBOOL
#undef RTP_GETINT

   cJSON *items = cJSON_GetObjectItemCaseSensitive(r, "items");
   if (cJSON_IsArray(items))
   {
      cJSON *item = NULL;
      cJSON_ArrayForEach(item, items)
      {
         out->item_count++;
         cJSON *sev = cJSON_GetObjectItemCaseSensitive(item, "severity");
         const char *s = cJSON_IsString(sev) ? sev->valuestring : "";
         if (s && strcmp(s, "blocking") == 0)
            out->blocking_count++;
         else if (s && strcmp(s, "suggestion") == 0)
            out->suggestion_count++;
         else
            out->nit_count++;
      }
   }

   cJSON *answers = cJSON_GetObjectItemCaseSensitive(r, "answered_questions");
   if (cJSON_IsArray(answers))
   {
      cJSON *a = NULL;
      cJSON_ArrayForEach(a, answers)
      {
         cJSON *ans = cJSON_GetObjectItemCaseSensitive(a, "answered");
         if (cJSON_IsTrue(ans))
            out->answered_count++;
      }
   }

   cJSON *gaps = cJSON_GetObjectItemCaseSensitive(r, "coverage_gaps");
   if (cJSON_IsArray(gaps))
      out->coverage_gap_count = cJSON_GetArraySize(gaps);

   cJSON_Delete(r);
   return 0;
}

int rtp_seam_register_attempt(int pipeline_pass_id, const char *run_id)
{
   if (pipeline_pass_id <= 0)
      return 0; /* not a pipeline-owned run */
   if (!run_id || !run_id[0])
      return -1;

   rtp_pass_t pass;
   if (rtp_pass_get(pipeline_pass_id, &pass) != 0)
      return -1; /* unknown pass */

   /* the pass must belong to an active, non-terminal pipeline (#21). */
   rtp_run_t run;
   if (rtp_run_get(pass.pipeline_id, &run) != 0)
      return -1;
   if (strcmp(run.state, RTP_STATE_DONE) == 0 || strcmp(run.state, RTP_STATE_FAILED) == 0 ||
       strcmp(run.state, RTP_STATE_ABANDONED) == 0)
      return -1;

   int attempt_no = rtp_attempt_max_no(pipeline_pass_id) + 1;
   int att_id = 0;
   if (rtp_attempt_create(pipeline_pass_id, attempt_no, run_id, &att_id) != 0)
      return -1;
   /* the newest attempt is the current one; older attempts step down (#30). */
   rtp_attempt_supersede_others(pipeline_pass_id, att_id);
   return 1;
}

int rtp_seam_finalize(const char *run_id, int http_ok, int cancelled, const char *result_json)
{
   if (!run_id || !run_id[0])
      return 0;

   rtp_attempt_t att;
   if (rtp_attempt_get_by_run(run_id, &att) != 0)
      return 0; /* not a pipeline run — ordinary ensemble_review, untouched */

   rtp_pass_t pass;
   int have_pass = (rtp_pass_get(att.pass_id, &pass) == 0);
   int is_draft = have_pass && strcmp(pass.mode, RTP_MODE_DRAFT) == 0;

   rtp_envelope_t env;
   rtp_capture_parse(result_json, is_draft, &env);

   /* Record the terminal envelope first; validity is decided after it is durable
    * (#44). Capture status reflects what we actually got. */
   if (!env.present)
   {
      snprintf(att.capture_status, sizeof(att.capture_status), RTP_CAP_LOST);
      att.lost_result = 1;
   }
   else if (cancelled)
      snprintf(att.capture_status, sizeof(att.capture_status), RTP_CAP_CANCELLED);
   else if (!http_ok || env.has_error || !env.parse_ok)
      snprintf(att.capture_status, sizeof(att.capture_status), RTP_CAP_FAILED);
   else
      snprintf(att.capture_status, sizeof(att.capture_status), RTP_CAP_CAPTURED);

   snprintf(att.terminal_status, sizeof(att.terminal_status), "%s",
            cancelled ? "cancelled" : (http_ok ? "completed" : "failed"));
   snprintf(att.parse_status, sizeof(att.parse_status), "%s", env.parse_ok ? "ok" : "malformed");
   att.truncated = env.truncated;
   att.items_truncated = env.items_truncated;
   att.degraded = env.degraded;
   att.cost_capped = env.cost_capped;
   att.deadline_hit = env.deadline_hit;
   att.cancelled = env.cancelled || cancelled;
   att.envelope_valid = roundtable_terminal_envelope_valid(&env);
   att.cost_usd = env.cost_usd;
   att.cost_known = env.cost_known;
   rtp_hash_hex(result_json, att.result_hash, sizeof(att.result_hash));
   if (result_json)
      snprintf(att.result_snapshot, sizeof(att.result_snapshot), "%s", result_json);
   /* a coarse terminal timestamp marker; the row also keeps submitted_at. */
   snprintf(att.terminal_at, sizeof(att.terminal_at), "captured");
   rtp_attempt_update(&att);

   /* Cumulative cost roll-up (#46/#19): every terminal attempt with a known cost
    * accumulates into the phase and pipeline totals — including lost/superseded
    * attempts, which are booked as overhead. The per-phase cap therefore never
    * resets across fail-return re-entries. */
   if (have_pass && env.cost_known && env.cost_usd > 0.0)
   {
      rtp_run_t run2;
      if (rtp_run_get(pass.pipeline_id, &run2) == 0)
      {
         /* Attribute to a phase by an explicit whitelist, not a catch-all else,
          * so a future phase cannot silently misbill to the proposal bucket
          * (#7). v1 has exactly two phases; an unknown phase still counts toward
          * the whole-pipeline total but is not misattributed to a per-phase cap. */
         if (strcmp(pass.phase, RTP_PHASE_IMPL) == 0)
            run2.impl_phase_cost_usd += env.cost_usd;
         else if (strcmp(pass.phase, RTP_PHASE_PROPOSAL) == 0)
            run2.proposal_phase_cost_usd += env.cost_usd;
         run2.total_cost_usd += env.cost_usd;
         rtp_run_update(&run2);
      }
   }

   /* Only the current (non-superseded) attempt of a live pass updates the
    * aggregate and can satisfy the done-bar; a late/superseded terminal stays
    * audit history (#30). */
   if (!have_pass || !att.is_current)
      return 1;
   if (strcmp(pass.status, RTP_PASS_SUPERSEDED) == 0 || strcmp(pass.status, RTP_PASS_FAILED) == 0)
      return 1;

   pass.converged = env.converged;
   pass.envelope_valid = att.envelope_valid;
   pass.blocking_count = env.blocking_count;
   pass.suggestion_count = env.suggestion_count;
   pass.nit_count = env.nit_count;
   pass.answered_count = env.answered_count; /* accepted brief questions answered (#3) */
   pass.open_questions = 0;                  /* refined by the driver from accepted-question set */
   pass.coverage_gaps = env.coverage_gap_count;
   pass.items_round = env.items_round;
   pass.artifact_round = env.artifact_round;
   pass.best_round = env.best_round;
   pass.rounds_run = env.rounds_run;
   pass.cost_usd = env.cost_usd;
   snprintf(pass.result_hash, sizeof(pass.result_hash), "%s", att.result_hash);
   if (att.lost_result)
      snprintf(pass.status, sizeof(pass.status), RTP_PASS_OPEN); /* awaits re-run */
   else
      snprintf(pass.status, sizeof(pass.status), RTP_PASS_CAPTURED);
   rtp_pass_update(&pass);
   return 1;
}
