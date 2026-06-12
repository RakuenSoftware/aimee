/* agent_tasks.c: durable jobs and multi-step plan execution */
#include "aimee.h"
#include "agent_tasks.h"
#include "agent_exec.h"
#include "agent_tools.h"
#include "cJSON.h"
#include "agent_config.h"
#include "db1.h"
#include "liveness.h"
#include <unistd.h>

/* Internal helper: run a single tool command via the agent and capture the result.
 * Replaces the removed agent_run_tool_command() from an earlier refactor. */
static int agent_run_tool_command(const agent_t *agent, const char *type, const char *cmd,
                                  int timeout_ms, agent_result_t *out)
{
   (void)timeout_ms; /* passed through agent config */
   char sys[128];
   snprintf(sys, sizeof(sys), "Execute the %s command and report the result.", type);
   return agent_execute(agent, sys, cmd, 2048, 0.2, out);
}

static void plan_record_evidence(int plan_id, int step_id, const char *kind, const char *content,
                                 int passed, const char *strength)
{
   if (step_id <= 0 || !kind || !content || !strength)
      return;

   (void)db1_step_evidence_insert(plan_id, step_id, kind, content, passed, strength);
}

static int plan_evaluate_step(const plan_step_t *step, const char **strength_out, char *detail,
                              size_t detail_len)
{
   const char *strength = "weak";
   int passed = 0;

   if (!step || !detail || detail_len == 0)
      return 0;

   detail[0] = '\0';

   if (step->success_predicate[0])
   {
      strength = "strong";
      passed = (step->output[0] && strstr(step->output, step->success_predicate) != NULL);
      snprintf(detail, detail_len, "predicate \"%s\" %s", step->success_predicate,
               passed ? "matched recorded output" : "missing from recorded output");
   }
   else if (step->status == 2)
   {
      passed = 1;
      snprintf(detail, detail_len, "step completed without an explicit success predicate");
   }
   else if (step->status == 4)
   {
      passed = 0;
      snprintf(detail, detail_len, "step was rolled back");
   }
   else
   {
      passed = 0;
      snprintf(detail, detail_len, "step status=%d", step->status);
   }

   if (strength_out)
      *strength_out = strength;
   return passed;
}

static void plan_apply_step_verification(int plan_id, plan_step_t *step, int passed)
{
   if (!step || step->id <= 0)
      return;

   if (!passed && step->status != 4)
      step->status = 3;

   (void)db1_plan_step_set_status(step->id, (step->status == 2)   ? "done"
                                            : (step->status == 3) ? "failed"
                                                                  : "rolled_back");
   (void)db1_execution_plan_set_status(plan_id, passed ? "done" : "failed");
}

/* --- Plan IR (Feature 2) --- */

int agent_plan_rollback_step(plan_t *plan, int step_idx, int timeout_ms)
{
   if (!plan || step_idx < 0 || step_idx >= plan->step_count)
      return -1;
   plan_step_t *s = &plan->steps[step_idx];
   if (!s->rollback[0])
      return 0;

   agent_result_t rr;
   memset(&rr, 0, sizeof(rr));
   int rc = agent_run_tool_command(NULL, "rollback", s->rollback, timeout_ms, &rr);

   (void)db1_plan_step_set_status_output(s->id, (rc == 0) ? "rolled_back" : "failed",
                                         rr.response ? rr.response : rr.error);

   if (rr.response)
      free(rr.response);
   s->status = (rc == 0) ? 4 : 3;
   return rc;
}

int agent_plan_execute(plan_t *plan, const agent_t *agent, int timeout_ms)
{
   if (!plan)
      return -1;

   (void)db1_execution_plan_set_status(plan->id, "running");

   for (int i = 0; i < plan->step_count; i++)
   {
      plan_step_t *s = &plan->steps[i];
      if (!s->action[0])
         continue;

      (void)db1_plan_step_set_status(s->id, "running");
      s->status = 1;

      agent_result_t rr;
      memset(&rr, 0, sizeof(rr));
      int rc = agent_run_tool_command(agent, "execute", s->action, timeout_ms, &rr);

      int success = (rc == 0);
      if (success && s->success_predicate[0])
      {
         const char *hay = rr.response ? rr.response : "";
         if (strstr(hay, s->success_predicate) == NULL)
            success = 0;
      }

      {
         char evidence[4608];
         snprintf(evidence, sizeof(evidence), "%s", rr.response ? rr.response : rr.error);
         plan_record_evidence(plan->id, s->id,
                              s->success_predicate[0] ? "success_predicate" : "step_output",
                              evidence, success, s->success_predicate[0] ? "strong" : "weak");
      }

      (void)db1_plan_step_set_status_output(s->id, success ? "done" : "failed",
                                            rr.response ? rr.response : rr.error);

      s->status = success ? 2 : 3;
      if (rr.response)
      {
         snprintf(s->output, sizeof(s->output), "%s", rr.response);
         free(rr.response);
      }
      else if (rr.error[0])
      {
         snprintf(s->output, sizeof(s->output), "%s", rr.error);
      }

      if (!success)
      {
         for (int j = i; j >= 0; j--)
         {
            if (plan->steps[j].status == 2)
               agent_plan_rollback_step(plan, j, timeout_ms);
         }

         (void)db1_execution_plan_set_status(plan->id, "failed");
         snprintf(plan->status, sizeof(plan->status), "%s", "failed");
         return -1;
      }
   }

   (void)db1_execution_plan_set_status(plan->id, "done");
   snprintf(plan->status, sizeof(plan->status), "%s", "done");
   return 0;
}

int agent_plan_verify(plan_t *plan, plan_verify_summary_t *summary)
{
   if (!plan)
      return -1;

   if (summary)
      memset(summary, 0, sizeof(*summary));

   int overall_ok = 1;
   for (int i = 0; i < plan->step_count; i++)
   {
      plan_step_t *step = &plan->steps[i];
      const char *strength = "weak";
      char detail[1024];
      int passed = plan_evaluate_step(step, &strength, detail, sizeof(detail));

      plan_record_evidence(plan->id, step->id, "plan_verify", detail, passed, strength);

      if (summary)
      {
         summary->total_steps++;
         if (!passed)
            summary->failed++;
         else if (strcmp(strength, "strong") == 0)
            summary->strong_passed++;
         else
            summary->weak_passed++;
      }

      if (!passed)
      {
         overall_ok = 0;
         plan_apply_step_verification(plan->id, step, 0);
      }
   }

   if (overall_ok)
   {
      (void)db1_execution_plan_set_status(plan->id, "done");
      snprintf(plan->status, sizeof(plan->status), "%s", "done");
      return 0;
   }

   snprintf(plan->status, sizeof(plan->status), "%s", "failed");
   return -1;
}

int plan_assign_waves(plan_t *plan)
{
   if (!plan)
      return -1;

   /* Initialise all wave indices to -1 (unassigned). */
   for (int i = 0; i < plan->step_count; i++)
      plan->steps[i].wave = -1;

   /* Iterative relaxation: each pass assigns wave = 1 + max(dep waves).
    * Steps with no deps get wave 0 on the first pass.
    * We need at most step_count passes to resolve a linear chain; one extra
    * pass detects cycles (nothing changes only when all are resolved). */
   int changed = 1;
   int passes = 0;
   int max_passes = plan->step_count + 1;
   while (changed && passes < max_passes)
   {
      changed = 0;
      passes++;
      for (int i = 0; i < plan->step_count; i++)
      {
         plan_step_t *s = &plan->steps[i];

         /* Compute the maximum wave of all dependencies. */
         int max_dep_wave = -1;
         int all_resolved = 1;
         for (int d = 0; d < s->dep_count; d++)
         {
            int dep_seq = s->depends_on[d];
            if (dep_seq < 0 || dep_seq >= plan->step_count)
               continue; /* ignore out-of-range refs */
            if (plan->steps[dep_seq].wave < 0)
            {
               all_resolved = 0;
               break;
            }
            if (plan->steps[dep_seq].wave > max_dep_wave)
               max_dep_wave = plan->steps[dep_seq].wave;
         }

         if (!all_resolved)
            continue;

         int new_wave = max_dep_wave + 1;
         if (s->wave != new_wave)
         {
            s->wave = new_wave;
            changed = 1;
         }
      }
   }

   /* Any step still at -1 is in a dependency cycle. */
   for (int i = 0; i < plan->step_count; i++)
      if (plan->steps[i].wave < 0)
         return -1;

   return 0;
}

int agent_execute_with_plan(const agent_t *agent, const agent_network_t *network,
                            const char *system_prompt, const char *user_prompt, int max_tokens,
                            double temperature, agent_result_t *out)
{
   if (!agent || !user_prompt || !out)
      return -1;

   memset(out, 0, sizeof(*out));

   /* Ask model to emit JSON plan */
   char plan_prompt[8192];
   snprintf(plan_prompt, sizeof(plan_prompt),
            "Create a concise execution plan as a JSON array. Each item must have: "
            "action, precondition, success_predicate, rollback. Task: %s\n"
            "Respond ONLY with JSON.",
            user_prompt);

   agent_result_t plan_res;
   memset(&plan_res, 0, sizeof(plan_res));
   int rc = agent_execute(agent, system_prompt, plan_prompt, max_tokens, temperature, &plan_res);
   if (rc != 0 || !plan_res.response)
   {
      snprintf(out->error, sizeof(out->error), "plan generation failed: %s", plan_res.error);
      if (plan_res.response)
         free(plan_res.response);
      return -1;
   }

   cJSON *json = cJSON_Parse(plan_res.response);
   if (!json || !cJSON_IsArray(json))
   {
      cJSON_Delete(json);
      /* Fallback to direct execution */
      free(plan_res.response);
      return agent_execute(agent, system_prompt, user_prompt, max_tokens, temperature, out);
   }

   int plan_id = db1_execution_plan_create(agent->name, user_prompt, json);
   cJSON_Delete(json);
   free(plan_res.response);
   if (plan_id < 0)
   {
      return agent_execute(agent, system_prompt, user_prompt, max_tokens, temperature, out);
   }

   plan_t plan;
   if (db1_execution_plan_get(plan_id, &plan) != 0)
      return -1;

   rc = agent_plan_execute(&plan, agent, agent->timeout_ms > 0 ? agent->timeout_ms : 60000);
   if (rc != 0)
   {
      snprintf(out->error, sizeof(out->error), "plan execution failed");
      return -1;
   }

   /* Summarize outputs */
   size_t cap = 8192;
   char *buf = malloc(cap);
   if (!buf)
   {
      snprintf(out->error, sizeof(out->error), "OOM");
      return -1;
   }
   buf[0] = '\0';
   for (int i = 0; i < plan.step_count; i++)
   {
      const char *status = (plan.steps[i].status == 2)   ? "done"
                           : (plan.steps[i].status == 4) ? "rolled_back"
                           : (plan.steps[i].status == 3) ? "failed"
                                                         : "pending";
      char line[4608];
      snprintf(line, sizeof(line), "Step %d [%s]: %s\n%s\n\n", i + 1, status, plan.steps[i].action,
               plan.steps[i].output);
      size_t need = strlen(buf) + strlen(line) + 1;
      if (need > cap)
      {
         cap = need * 2;
         char *tmp = realloc(buf, cap);
         if (!tmp)
         {
            free(buf);
            snprintf(out->error, sizeof(out->error), "OOM");
            return -1;
         }
         buf = tmp;
      }
      strcat(buf, line);
   }
   out->response = buf;
   return 0;
}

/* agent_jobs.c: durable jobs, result cache, one-shot hints, durable job context */

/* --- Durable job context globals ---
 *
 * Only the job id is published. DB1 helpers (db1_agent_job_update,
 * db1_agent_job_heartbeat) reach DB1 through db1_conn() internally,
 * so callers do not pass a DB connection in. */

static __thread int g_durable_job_id = 0;

void agent_set_durable_job(int job_id)
{
   g_durable_job_id = job_id;
}

int agent_get_durable_job_id(void)
{
   return g_durable_job_id;
}

int agent_job_resume(agent_config_t *cfg, int job_id, agent_result_t *out)
{
   if (!cfg || !out || job_id <= 0)
      return -1;

   memset(out, 0, sizeof(*out));

   /* job.prompt/result are heap-owned after a successful get; every exit below
    * goes through `out:` so they are freed exactly once. On a miss the get
    * zero-inits the struct, so the free is still safe. */
   db1_agent_job_t job;
   int rc = -1;
   if (db1_agent_job_get(job_id, &job) != 0)
   {
      snprintf(out->error, sizeof(out->error), "job %d not found", job_id);
      goto out;
   }

   if (strcmp(job.status, "done") == 0)
   {
      snprintf(out->error, sizeof(out->error), "job %d already completed", job_id);
      goto out;
   }
   if (strcmp(job.status, "cancelled") == 0)
   {
      snprintf(out->error, sizeof(out->error), "job %d is cancelled", job_id);
      goto out;
   }
   if (strcmp(job.status, "running") == 0 && job.heartbeat_at[0])
   {
      if (!db1_agent_job_heartbeat_is_stale(job.heartbeat_at, 10))
      {
         snprintf(out->error, sizeof(out->error), "job %d is still active", job_id);
         goto out;
      }
   }

   char pid[32];
   snprintf(pid, sizeof(pid), "%d", (int)getpid());
   if (strcmp(job.status, "pending") != 0)
      db1_agent_job_update(job_id, "pending", job.cursor_turn, job.result);
   if (db1_agent_job_take_lease(job_id, pid) != 0)
   {
      snprintf(out->error, sizeof(out->error), "failed to take lease for job %d", job_id);
      goto out;
   }
   agent_set_durable_job(job_id);

   rc = agent_run(cfg, job.role, NULL, job.prompt, AGENT_DEFAULT_MAX_TOKENS, out);
   agent_set_durable_job(0);
   int cursor_turn = out->turns > job.cursor_turn ? out->turns : job.cursor_turn;
   if (rc == 0 && liveness_reject_degenerate_response(
                      &out->response, out->error, sizeof(out->error),
                      "agent job returned raw tool-call markup or another degenerate response"))
   {
      rc = -1;
      out->success = 0;
   }

   db1_agent_job_update(job_id, (rc == 0) ? "done" : "failed", cursor_turn,
                        out->response ? out->response : out->error);

out:
   db1_agent_job_free(&job);
   return rc;
}

/* Agent result cache storage moved to src/db1/caches.c (db1_agent_cache_*).
 * One-shot hint lookup moved to db2/agent_hints.c
 * (db2_agent_hint_find_and_consume); callers go through that API
 * directly. */
