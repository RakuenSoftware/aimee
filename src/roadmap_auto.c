/* roadmap_auto.c: deterministic spec-driven roadmap dispatch loop.
 *
 * The loop is the primary orchestrator: it selects the next ready leaf-task
 * unit from the DB1 runtime state, builds a delegate work packet from the DB2
 * plan_unit artifact, dispatches it via delegate_launch_coord_job, runs the
 * unit's verification commands as a gate, and persists the result. No LLM
 * component makes state-transition decisions.
 *
 * See src/headers/roadmap_auto.h and
 * docs/proposals/pending/spec-driven-roadmaps-and-autonomous-delegate-dispatch.md
 */

#include "aimee.h"
#include "headers/roadmap_auto.h"
#include "headers/roadmap_milestone.h"
#include "headers/roadmap_reassess.h"
#include "headers/roadmap_report.h"
#include "db1/roadmap_runtime.h"
#include "headers/roadmap.h"
#include "delegate_launch.h"
#include "headers/dstr.h"
#include "headers/util.h"
#include "headers/agent_config.h"
#include "headers/agent_exec.h"
#include "db2/artifacts.h"
#include "db2/db_postgres.h"
#include "db2/db2_internal.h"
#include "cJSON.h"

/* Maximum bytes captured from a verification command. */
#define ROADMAP_VERIFY_OUTPUT_MAX (4 * 1024)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void write_exit(char *buf, size_t len, const char *reason)
{
   if (buf && len)
      snprintf(buf, len, "%s", reason);
}

static long now_s(void)
{
   return (long)time(NULL);
}

static void cfg_defaults(const roadmap_auto_cfg_t *in, roadmap_auto_cfg_t *out)
{
   memset(out, 0, sizeof(*out));
   if (in)
      *out = *in;
   if (out->max_verify_retries <= 0)
      out->max_verify_retries = ROADMAP_AUTO_MAX_VERIFY_RETRIES;
   if (out->soft_timeout_s < 0)
      out->soft_timeout_s = 0;
}

/* Run a shell command and return its exit code; stdout+stderr captured and
 * discarded (the gate only needs the exit status). */
static int rdm_run_cmd(const char *cmd)
{
   if (!cmd || !cmd[0])
      return 0;
   const char *argv[] = {"/bin/sh", "-c", cmd, NULL};
   char *out = NULL;
   int rc = safe_exec_capture(argv, &out, ROADMAP_VERIFY_OUTPUT_MAX);
   free(out);
   return rc;
}

/* Load the plan_unit DB2 artifact payload for unit_id. Caller frees. */
static cJSON *load_unit_payload(const char *unit_id)
{
   if (!unit_id || !unit_id[0])
      return NULL;
   db2_artifact_row_t row;
   int cc = 0;
   if (db2_artifact_read(unit_id, &row, NULL, 0, &cc) != 0)
      return NULL;
   return cJSON_Parse(row.payload_json);
}

/* Build a minimal delegate_plan_v1 JSON for one leaf task unit.
 * The packet uses role="code", owned_files from the payload, and a prompt
 * derived from title + intent + acceptance_criteria. Caller frees. */
static cJSON *build_unit_plan(const char *roadmap_id, const char *unit_id, cJSON *payload)
{
   if (!payload)
      return NULL;

   const char *title = "";
   const char *intent = "";
   cJSON *tj = cJSON_GetObjectItemCaseSensitive(payload, "title");
   cJSON *ij = cJSON_GetObjectItemCaseSensitive(payload, "intent");
   if (cJSON_IsString(tj))
      title = tj->valuestring;
   if (cJSON_IsString(ij))
      intent = ij->valuestring;

   /* Build prompt from title + intent + acceptance_criteria. */
   dstr_t s;
   dstr_init(&s);
   dstr_appendf(&s, "Implement: %s\n\n", title);
   if (intent[0])
      dstr_appendf(&s, "Intent: %s\n\n", intent);
   cJSON *ac = cJSON_GetObjectItemCaseSensitive(payload, "acceptance_criteria");
   if (cJSON_IsArray(ac))
   {
      dstr_append_str(&s, "Acceptance criteria:\n");
      cJSON *item;
      cJSON_ArrayForEach(item, ac)
      {
         if (cJSON_IsString(item))
            dstr_appendf(&s, "- %s\n", item->valuestring);
      }
   }
   cJSON *vc = cJSON_GetObjectItemCaseSensitive(payload, "verification_commands");
   if (cJSON_IsArray(vc) && cJSON_GetArraySize(vc) > 0)
   {
      dstr_append_str(&s, "\nRun to verify: ");
      cJSON *item;
      int first = 1;
      cJSON_ArrayForEach(item, vc)
      {
         if (cJSON_IsString(item))
         {
            if (!first)
               dstr_append_str(&s, " && ");
            dstr_append_str(&s, item->valuestring);
            first = 0;
         }
      }
      dstr_append_str(&s, "\n");
   }
   dstr_appendf(&s, "\nRoadmap context: roadmap_id=%s unit_id=%s\n", roadmap_id, unit_id);

   /* Build plan JSON. */
   cJSON *plan = cJSON_CreateObject();
   cJSON_AddStringToObject(plan, "schema", "delegate_plan_v1");
   cJSON *packets = cJSON_AddArrayToObject(plan, "packets");

   /* Map tool_policy_mode onto the delegate role so the existing guardrails
    * enforce the write-scope contract without any new enforcement engine:
    *   planning → "review" (read-only; no writes allowed by role)
    *   docs     → "draft"  (writes only under docs/ and .aimee/)
    *   execution → "code"  (writes scoped to owned_files) */
   const char *role = "code";
   cJSON *tpj = cJSON_GetObjectItemCaseSensitive(payload, "tool_policy_mode");
   if (cJSON_IsString(tpj))
   {
      if (strcmp(tpj->valuestring, "planning") == 0)
         role = "review";
      else if (strcmp(tpj->valuestring, "docs") == 0)
         role = "draft";
   }

   cJSON *packet = cJSON_CreateObject();
   cJSON_AddStringToObject(packet, "role", role);
   cJSON_AddStringToObject(packet, "prompt", dstr_cstr(&s));
   cJSON *owned = cJSON_AddArrayToObject(packet, "owned_files");
   cJSON *of = cJSON_GetObjectItemCaseSensitive(payload, "owned_files");
   if (cJSON_IsArray(of))
   {
      cJSON *f;
      cJSON_ArrayForEach(f, of)
      {
         if (cJSON_IsString(f))
            cJSON_AddItemToArray(owned, cJSON_CreateString(f->valuestring));
      }
   }
   cJSON_AddItemToArray(packets, packet);
   cJSON_AddArrayToObject(plan, "missing_owned_files");
   cJSON_AddArrayToObject(plan, "new_owned_files");
   cJSON_AddNumberToObject(plan, "packet_count", 1);

   dstr_free(&s);
   return plan;
}

/* Run the unit's verification_commands. Returns 0 on pass, nonzero on fail. */
static int verify_unit(cJSON *payload)
{
   cJSON *vc = cJSON_GetObjectItemCaseSensitive(payload, "verification_commands");
   if (!cJSON_IsArray(vc) || cJSON_GetArraySize(vc) == 0)
      return 0; /* no commands = pass */
   cJSON *item;
   cJSON_ArrayForEach(item, vc)
   {
      if (cJSON_IsString(item) && item->valuestring[0])
      {
         int rc = rdm_run_cmd(item->valuestring);
         if (rc != 0)
            return rc;
      }
   }
   return 0;
}

/* Ensure all plan_unit rows for a roadmap exist in rdm_unit_dispatch.
 * Queries DB2 for plan_unit artifacts scoped to roadmap_id and upserts. */
static int sync_units_to_dispatch(const char *roadmap_id)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT id, payload FROM artifacts WHERE kind = ?1 AND scope_id = ?2 AND state = ?3",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", "plan_unit");
   aimee_pg_bind_text(st, "?2", roadmap_id);
   aimee_pg_bind_text(st, "?3", "committed");
   int n = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *uid = aimee_pg_column_text(st, 0);
      const char *pl = aimee_pg_column_text(st, 1);
      if (!uid || !pl)
         continue;
      cJSON *p = cJSON_Parse(pl);
      if (!p)
         continue;
      const char *level = "task";
      const char *policy = "execution";
      cJSON *lj = cJSON_GetObjectItemCaseSensitive(p, "level");
      cJSON *pj = cJSON_GetObjectItemCaseSensitive(p, "tool_policy_mode");
      if (cJSON_IsString(lj))
         level = lj->valuestring;
      if (cJSON_IsString(pj))
         policy = pj->valuestring;
      rdm_unit_ensure(roadmap_id, uid, level, policy);
      cJSON_Delete(p);
      n++;
   }
   aimee_pg_finalize(st);
   return n >= 0 ? 0 : -1;
}

/* ── stuck-loop detection ─────────────────────────────────────────────────── */

typedef struct
{
   char last_unit_ids[ROADMAP_AUTO_STUCK_WINDOW][64];
   int count;
} stuck_detector_t;

static void stuck_init(stuck_detector_t *d)
{
   memset(d, 0, sizeof(*d));
}

/* Returns 1 if the same unit has appeared >= half the window size consecutively. */
static int stuck_check(stuck_detector_t *d, const char *unit_id)
{
   int idx = d->count % ROADMAP_AUTO_STUCK_WINDOW;
   snprintf(d->last_unit_ids[idx], sizeof(d->last_unit_ids[0]), "%s", unit_id ? unit_id : "");
   d->count++;
   if (d->count < ROADMAP_AUTO_STUCK_WINDOW)
      return 0;
   int repeats = 0;
   const char *ref = d->last_unit_ids[idx];
   for (int i = 0; i < ROADMAP_AUTO_STUCK_WINDOW; i++)
   {
      if (strcmp(d->last_unit_ids[i], ref) == 0)
         repeats++;
   }
   return repeats >= ROADMAP_AUTO_STUCK_WINDOW / 2;
}

/* ── main loop ───────────────────────────────────────────────────────────── */

int roadmap_auto_run(const char *roadmap_id, const roadmap_auto_cfg_t *cfg_in, char *exit_reason,
                     size_t exit_reason_len)
{
   if (!roadmap_id || !roadmap_id[0])
      return -1;

   roadmap_auto_cfg_t cfg;
   cfg_defaults(cfg_in, &cfg);

   /* Ensure the roadmap dispatch row exists in DB1. */
   rdm_dispatch_upsert(roadmap_id, "balanced", cfg.require_slice_discussion,
                       cfg.budget_ceiling_tokens);
   rdm_dispatch_set_status(roadmap_id, "running", NULL);

   /* Sync plan_unit artifacts → rdm_unit_dispatch rows. */
   if (sync_units_to_dispatch(roadmap_id) != 0)
   {
      write_exit(exit_reason, exit_reason_len, ROADMAP_AUTO_EXIT_ERROR);
      rdm_dispatch_set_status(roadmap_id, "error", "failed to sync units from DB2");
      return -1;
   }

   long start_s = now_s();
   int iterations = 0;
   stuck_detector_t stuck;
   stuck_init(&stuck);

   for (;;)
   {
      /* 1. Check for external pause/stop. */
      rdm_dispatch_t drow;
      if (rdm_dispatch_get(roadmap_id, &drow) == 0)
      {
         if (strcmp(drow.status, "paused") == 0)
         {
            write_exit(exit_reason, exit_reason_len, ROADMAP_AUTO_EXIT_PAUSED);
            return 0;
         }
         if (strcmp(drow.status, "stopped") == 0)
         {
            write_exit(exit_reason, exit_reason_len, ROADMAP_AUTO_EXIT_STOPPED);
            return 0;
         }
      }

      /* 2. Safety guards. */
      if (cfg.soft_timeout_s > 0 && now_s() - start_s >= cfg.soft_timeout_s)
      {
         rdm_dispatch_set_status(roadmap_id, "paused", ROADMAP_AUTO_EXIT_TIMEOUT);
         write_exit(exit_reason, exit_reason_len, ROADMAP_AUTO_EXIT_TIMEOUT);
         return 0;
      }
      if (cfg.max_iterations > 0 && iterations >= cfg.max_iterations)
      {
         rdm_dispatch_set_status(roadmap_id, "paused", "max_iterations");
         write_exit(exit_reason, exit_reason_len, ROADMAP_AUTO_EXIT_PAUSED);
         return 0;
      }
      iterations++;

      /* 3. Select next ready leaf task. */
      char unit_id[64] = "";
      int sel = rdm_unit_select_next(roadmap_id, unit_id, sizeof(unit_id));
      if (sel < 0)
      {
         rdm_dispatch_set_status(roadmap_id, "error", "unit select failed");
         write_exit(exit_reason, exit_reason_len, ROADMAP_AUTO_EXIT_ERROR);
         return -1;
      }
      if (sel == 1)
      {
         /* No more pending tasks — run reassessment if enabled, then write
          * the report and mark the roadmap done. */
         fprintf(stderr, "aimee: roadmap auto: no pending units for %s\n", roadmap_id);
         roadmap_projections_write(roadmap_id);
         roadmap_report_write(roadmap_id);

         if (cfg.run_reassessment)
         {
            fprintf(stderr, "aimee: roadmap auto: running reassessment for %s\n", roadmap_id);
            agent_config_t acfg;
            char *verdict = NULL;
            int reassess_rc = -1;
            if (agent_load_config(&acfg) == 0 && acfg.agent_count > 0)
            {
               agent_http_init();
               reassess_rc = roadmap_reassess_run(roadmap_id, &acfg, &verdict);
               agent_http_cleanup();
            }
            if (reassess_rc == 1 && verdict && verdict[0])
            {
               /* Gaps found: log and continue the loop so new tasks are processed. */
               fprintf(stderr, "aimee: roadmap auto: reassessment found gaps — continuing: %s\n",
                       verdict);
               free(verdict);
               continue;
            }
            free(verdict);
         }

         rdm_dispatch_set_status(roadmap_id, "done", ROADMAP_AUTO_EXIT_ALL_COMPLETE);
         write_exit(exit_reason, exit_reason_len, ROADMAP_AUTO_EXIT_ALL_COMPLETE);
         fprintf(stderr, "aimee: roadmap auto: complete for %s\n", roadmap_id);
         return 0;
      }

      /* 4. Stuck-loop detection. */
      if (stuck_check(&stuck, unit_id))
      {
         rdm_dispatch_set_status(roadmap_id, "paused", ROADMAP_AUTO_EXIT_STUCK);
         write_exit(exit_reason, exit_reason_len, ROADMAP_AUTO_EXIT_STUCK);
         fprintf(stderr, "aimee: roadmap auto: stuck on unit %s — pausing\n", unit_id);
         return 0;
      }

      /* 5. Load the DB2 plan_unit artifact. */
      cJSON *payload = load_unit_payload(unit_id);
      if (!payload)
      {
         rdm_unit_set_state(roadmap_id, unit_id, "blocked");
         fprintf(stderr, "aimee: roadmap auto: could not load payload for unit %s\n", unit_id);
         continue;
      }

      /* 6. Claim the unit. */
      rdm_unit_claim(roadmap_id, unit_id, "roadmap_auto", cfg.cwd ? cfg.cwd : "");
      rdm_dispatch_set_phase(roadmap_id, "execute");
      fprintf(stderr, "aimee: roadmap auto: dispatching unit %s\n", unit_id);

      /* 7. Build and dispatch the delegate work packet. */
      cJSON *plan = build_unit_plan(roadmap_id, unit_id, payload);
      int dispatch_ok = 0;
      if (plan)
      {
         delegate_launch_result_t launch_res;
         char launch_err[256] = "";
         int lrc = delegate_launch_coord_job(plan, 1, cfg.cwd ? cfg.cwd : ".", &launch_res,
                                             launch_err, sizeof(launch_err));
         cJSON_Delete(plan);
         if (lrc == 0)
         {
            dispatch_ok = 1;
            rdm_unit_set_coord_job(roadmap_id, unit_id, launch_res.job_id);
         }
         else
         {
            fprintf(stderr, "aimee: roadmap auto: dispatch failed for %s: %s\n", unit_id,
                    launch_err);
         }
      }

      if (!dispatch_ok)
      {
         rdm_unit_finish(roadmap_id, unit_id, "blocked", "", "dispatch failed");
         cJSON_Delete(payload);
         continue;
      }

      /* 8. Verification gate with auto-fix retries. */
      int verify_pass = 0;
      for (int attempt = 0; attempt < cfg.max_verify_retries; attempt++)
      {
         rdm_unit_increment_verify_attempts(roadmap_id, unit_id);
         int vrc = verify_unit(payload);
         if (vrc == 0)
         {
            verify_pass = 1;
            break;
         }
         fprintf(stderr, "aimee: roadmap auto: verify failed for %s (attempt %d/%d)\n", unit_id,
                 attempt + 1, cfg.max_verify_retries);
         if (attempt + 1 < cfg.max_verify_retries)
         {
            /* Re-dispatch the same unit for auto-fix. */
            cJSON *fix_plan = build_unit_plan(roadmap_id, unit_id, payload);
            if (fix_plan)
            {
               delegate_launch_result_t fix_res;
               char fix_err[64] = "";
               delegate_launch_coord_job(fix_plan, 1, cfg.cwd ? cfg.cwd : ".", &fix_res, fix_err,
                                         sizeof(fix_err));
               cJSON_Delete(fix_plan);
            }
         }
      }

      if (!verify_pass)
      {
         fprintf(stderr, "aimee: roadmap auto: unit %s moved to needs_review\n", unit_id);
         rdm_unit_finish(roadmap_id, unit_id, "needs_review", "",
                         "verification exhausted after retries");
         /* The loop could continue with other independent units; for the
          * single-track baseline we pause so a human can review. */
         cJSON_Delete(payload);
         rdm_dispatch_set_status(roadmap_id, "paused", ROADMAP_AUTO_EXIT_VERIFY_EXHAUSTED);
         write_exit(exit_reason, exit_reason_len, ROADMAP_AUTO_EXIT_VERIFY_EXHAUSTED);
         return 0;
      }

      /* 9. Persist result and refresh projections. */
      rdm_unit_finish(roadmap_id, unit_id, "done", "verified", "");
      roadmap_projections_write(roadmap_id);
      rdm_unit_heartbeat(roadmap_id, unit_id);
      fprintf(stderr, "aimee: roadmap auto: unit %s done\n", unit_id);

      /* 10. Milestone gate: check if the parent slice — and then its parent
       * milestone — are now fully done. If so, run a review-delegate
       * validation. A 'pass' verdict lets the milestone close; 'fail' or
       * 'partial' reopens the milestone by creating a new gap-task and
       * continues the loop. */
      cJSON *par_j = cJSON_GetObjectItemCaseSensitive(payload, "parent_id");
      const char *slice_id = cJSON_IsString(par_j) ? par_j->valuestring : "";
      if (slice_id[0])
      {
         /* Check if all tasks in this slice are done. */
         int all_tasks = roadmap_milestone_all_done(roadmap_id, slice_id, "task");
         if (all_tasks == 1)
         {
            /* Check if the slice's milestone has all slices done. */
            cJSON *spayload_raw = load_unit_payload(slice_id);
            char milestone_id[64] = "";
            if (spayload_raw)
            {
               cJSON *mpj = cJSON_GetObjectItemCaseSensitive(spayload_raw, "parent_id");
               if (cJSON_IsString(mpj))
                  snprintf(milestone_id, sizeof(milestone_id), "%s", mpj->valuestring);
               cJSON_Delete(spayload_raw);
            }
            int all_slices =
                milestone_id[0] ? roadmap_milestone_all_done(roadmap_id, milestone_id, "slice") : 0;
            if (all_slices == 1)
            {
               fprintf(stderr, "aimee: roadmap auto: milestone %s complete — running gate\n",
                       milestone_id);
               agent_config_t acfg;
               char *verdict = NULL;
               if (agent_load_config(&acfg) == 0 && acfg.agent_count > 0)
               {
                  agent_http_init();
                  verdict = roadmap_milestone_validate(roadmap_id, milestone_id, &acfg);
                  agent_http_cleanup();
               }
               if (verdict && strcmp(verdict, "pass") == 0)
               {
                  fprintf(stderr, "aimee: roadmap auto: milestone %s gate: pass\n", milestone_id);
                  rdm_unit_set_state(roadmap_id, milestone_id, "done");
               }
               else
               {
                  const char *v = verdict ? verdict : "unknown";
                  fprintf(stderr, "aimee: roadmap auto: milestone %s gate: %s — reopening\n",
                          milestone_id, v);
                  /* Reopen: reset the milestone to pending so the loop re-examines it. */
                  rdm_unit_set_state(roadmap_id, milestone_id, "pending");
               }
               free(verdict);
            }
         }
      }
      cJSON_Delete(payload);
   }
}

/* ── control API ─────────────────────────────────────────────────────────── */

int roadmap_auto_pause(const char *roadmap_id)
{
   if (!roadmap_id || !roadmap_id[0])
      return -1;
   return rdm_dispatch_set_status(roadmap_id, "paused", ROADMAP_AUTO_EXIT_PAUSED);
}

int roadmap_auto_resume(const char *roadmap_id, const roadmap_auto_cfg_t *cfg, char *exit_reason,
                        size_t exit_reason_len)
{
   return roadmap_auto_run(roadmap_id, cfg, exit_reason, exit_reason_len);
}

int roadmap_auto_stop(const char *roadmap_id)
{
   if (!roadmap_id || !roadmap_id[0])
      return -1;
   return rdm_dispatch_set_status(roadmap_id, "stopped", ROADMAP_AUTO_EXIT_STOPPED);
}

int roadmap_auto_status(const char *roadmap_id, int json_output)
{
   if (!roadmap_id || !roadmap_id[0])
      return -1;
   rdm_dispatch_t d;
   if (rdm_dispatch_get(roadmap_id, &d) != 0)
      return -1;
   if (json_output)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "roadmap_id", d.roadmap_id);
      cJSON_AddStringToObject(o, "status", d.status);
      cJSON_AddStringToObject(o, "phase", d.phase);
      cJSON_AddStringToObject(o, "exit_reason", d.exit_reason);
      char *s = cJSON_PrintUnformatted(o);
      if (s)
         printf("%s\n", s);
      free(s);
      cJSON_Delete(o);
   }
   else
   {
      printf("roadmap %s: status=%s phase=%s%s%s\n", d.roadmap_id, d.status, d.phase,
             d.exit_reason[0] ? " exit=" : "", d.exit_reason);
   }
   return 0;
}
