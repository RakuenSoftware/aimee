/* agent_loop.c: self-correcting delegate loop.
 *
 * Each iteration runs the delegate, then asks it to self-assess task
 * completion (0-100). The loop continues until:
 *   - self-assessed completion >= threshold AND verify command passes, or
 *   - the iteration cap is reached.
 *
 * This is quality-based retry, separate from --retry (transient failures).
 */
#include "aimee.h"
#include "agent_exec.h"
#include <aimee/delegates/delegate_role.h>
#include "dstr.h"
#include "log.h"
#include "util.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Self-assessment suffix appended to each iteration's prompt. */
static const char SELF_ASSESS_SUFFIX[] =
    "\n\nAfter completing the task, append a JSON self-assessment as your "
    "final output:\n"
    "{\"completion\": <0-100>, \"gaps\": [\"remaining gap 1\", ...]}\n"
    "Where \"completion\" is your estimate of how much of the original task "
    "is done (100 = fully complete, 0 = not started).";

/* ------------------------------------------------------------------ init */

void agent_loop_init(agent_loop_t *loop, int max_iter, int threshold, const char *verify_cmd)
{
   if (!loop)
      return;
   memset(loop, 0, sizeof(*loop));
   loop->max_iterations = max_iter > 0 ? max_iter : AGENT_LOOP_MAX_ITER_DEFAULT;
   loop->completion_threshold =
       (threshold >= 0 && threshold <= 100) ? threshold : AGENT_LOOP_THRESHOLD_DEFAULT;
   if (verify_cmd && verify_cmd[0])
      snprintf(loop->verify_cmd, sizeof(loop->verify_cmd), "%s", verify_cmd);
}

void agent_loop_free(agent_loop_t *loop)
{
   if (!loop)
      return;
   free(loop->accumulated_context);
   loop->accumulated_context = NULL;
}

/* ------------------------------------------------------------------ parser */

/* Scan backwards through text for the last '{' that starts a JSON object
 * containing a "completion" key. This lets the delegate write prose before
 * the assessment JSON without confusing the parser. */
int agent_loop_parse_completion(const char *response, char *gaps_out, size_t gaps_len)
{
   if (!response)
      return -1;

   /* Scan from the end for the last { */
   const char *p = response + strlen(response);
   const char *found = NULL;
   while (p >= response)
   {
      if (*p == '{')
      {
         /* Quick check: does this look like it contains "completion"? */
         if (strstr(p, "\"completion\"") || strstr(p, "'completion'"))
         {
            found = p;
            break;
         }
      }
      p--;
   }

   if (!found)
      return -1;

   cJSON *obj = cJSON_Parse(found);
   if (!obj)
      return -1;

   cJSON *comp_item = cJSON_GetObjectItemCaseSensitive(obj, "completion");
   if (!comp_item || !cJSON_IsNumber(comp_item))
   {
      cJSON_Delete(obj);
      return -1;
   }

   int score = (int)comp_item->valuedouble;
   /* Clamp to [0, 100] */
   if (score < 0)
      score = 0;
   if (score > 100)
      score = 100;

   /* Extract gaps if requested */
   if (gaps_out && gaps_len > 0)
   {
      gaps_out[0] = '\0';
      cJSON *gaps_arr = cJSON_GetObjectItemCaseSensitive(obj, "gaps");
      if (gaps_arr && cJSON_IsArray(gaps_arr))
      {
         dstr_t gs;
         dstr_init(&gs);
         int first = 1;
         const cJSON *g;
         cJSON_ArrayForEach(g, gaps_arr)
         {
            if (!cJSON_IsString(g))
               continue;
            if (!first)
               dstr_append_str(&gs, "; ");
            dstr_append_str(&gs, g->valuestring);
            first = 0;
         }
         snprintf(gaps_out, gaps_len, "%s", dstr_cstr(&gs));
         dstr_free(&gs);
      }
   }

   cJSON_Delete(obj);
   return score;
}

/* ------------------------------------------------------------------ helpers */

/* Run a shell command and capture its output into out_buf (caller frees).
 * Returns the exit code. */
static int run_verify(const char *cmd, char **out_buf)
{
   const char *argv[] = {"/bin/sh", "-c", cmd, NULL};
   int rc = safe_exec_capture(argv, out_buf, AGENT_TOOL_OUTPUT_MAX);
   return rc;
}

/* Append a prior-iteration summary to the accumulated context buffer.
 * Truncates accumulated_context if it would exceed AGENT_LOOP_CONTEXT_MAX. */
static void accumulate_context(agent_loop_t *loop, int iter, int completion, const char *gaps,
                               const char *verify_out)
{
   dstr_t buf;
   dstr_init(&buf);

   if (loop->accumulated_context && loop->accumulated_context[0])
      dstr_append_str(&buf, loop->accumulated_context);

   dstr_appendf(&buf, "[Iteration %d: completion=%d%%", iter, completion);
   if (gaps && gaps[0])
      dstr_appendf(&buf, ", gaps: %s", gaps);
   if (verify_out && verify_out[0])
   {
      /* Keep verify output brief */
      char trunc[256];
      snprintf(trunc, sizeof(trunc), "%.255s", verify_out);
      dstr_appendf(&buf, ", verify output: %s", trunc);
   }
   dstr_append_str(&buf, "]\n");

   /* Trim to cap */
   const char *s = dstr_cstr(&buf);
   size_t total = strlen(s);
   if (total > AGENT_LOOP_CONTEXT_MAX)
   {
      /* Keep the tail (most recent context is most useful) */
      s = s + (total - AGENT_LOOP_CONTEXT_MAX);
   }

   free(loop->accumulated_context);
   loop->accumulated_context = strdup(s);
   dstr_free(&buf);
}

/* Build the prompt for the next iteration, incorporating original task and
 * accumulated context from prior runs. Caller frees the returned string. */
static char *build_iter_prompt(const char *original, const char *accumulated_ctx, int iter,
                               int max_iter)
{
   dstr_t p;
   dstr_init(&p);

   dstr_append_str(&p, original);

   if (accumulated_ctx && accumulated_ctx[0])
   {
      dstr_appendf(&p,
                   "\n\n[Continuation: iteration %d of %d. Prior progress:\n%s\n"
                   "Pick up where you left off, address the remaining gaps.]",
                   iter, max_iter, accumulated_ctx);
   }

   /* Append self-assessment request */
   dstr_append_str(&p, SELF_ASSESS_SUFFIX);

   return dstr_steal(&p);
}

/* ------------------------------------------------------------------ main loop */

int agent_loop_run(agent_config_t *cfg, const char *role, const char *system_prompt,
                   const char *original_prompt, int max_tokens, agent_loop_t *loop,
                   agent_result_t *out)
{
   if (!cfg || !role || !original_prompt || !loop || !out)
      return -1;

   memset(out, 0, sizeof(*out));
   loop->current_iteration = 0;
   loop->last_completion = 0;

   int final_rc = -1;

   for (int iter = 1; iter <= loop->max_iterations; iter++)
   {
      loop->current_iteration = iter;

      /* Build iteration prompt */
      char *iter_prompt =
          build_iter_prompt(original_prompt, loop->accumulated_context, iter, loop->max_iterations);
      if (!iter_prompt)
      {
         aimee_log(LOG_ERROR, "agent_loop", "OOM building iteration prompt");
         return -1;
      }

      LOG_INFO("agent_loop", "iteration %d/%d starting", iter, loop->max_iterations);

      /* Free previous response before overwriting */
      free(out->response);
      memset(out, 0, sizeof(*out));

      int rc = agent_run(cfg, role, system_prompt, iter_prompt, max_tokens, out);
      free(iter_prompt);

      if (rc != 0)
      {
         LOG_WARN("agent_loop", "iteration %d failed: %s", iter, out->error);
         /* Hard failure — propagate */
         final_rc = rc;
         break;
      }

      /* Turn 1 protocol: write roles must produce file changes on the first
       * iteration. Zero tool calls means the delegate produced text-only output
       * (likely a discovery or planning response). Warn and inject a correction
       * into the accumulated context so the next iteration acts. */
      if (iter == 1 && out->tool_calls == 0 && delegate_role_is_write(role))
      {
         LOG_WARN("agent_loop",
                  "Turn 1 protocol violation: write role '%s' produced no tool calls. "
                  "Delegate should write files on the first turn, not plan or explore.",
                  role);
         if (!loop->accumulated_context)
         {
            loop->accumulated_context = malloc(512);
            if (loop->accumulated_context)
               snprintf(loop->accumulated_context, 512,
                        "Turn 1 produced no file writes. Write the required files immediately "
                        "in turn 2 — do not read or plan further. Use Write or Edit tools now.");
         }
      }

      /* Parse completion from response */
      char gaps[512] = "";
      int completion = -1;
      if (out->response)
         completion = agent_loop_parse_completion(out->response, gaps, sizeof(gaps));

      if (completion < 0)
      {
         /* No self-assessment found — treat as complete to avoid infinite loops */
         LOG_INFO("agent_loop", "iteration %d: no completion score found, treating as done", iter);
         completion = 100;
      }

      loop->last_completion = completion;
      LOG_INFO("agent_loop", "iteration %d: completion=%d%% threshold=%d%%", iter, completion,
               loop->completion_threshold);

      /* Run verify command if provided */
      int verify_passed = 1;
      char *verify_out = NULL;
      if (loop->verify_cmd[0])
      {
         int verify_rc = run_verify(loop->verify_cmd, &verify_out);
         verify_passed = (verify_rc == 0);
         if (!verify_passed)
            LOG_INFO("agent_loop", "iteration %d: verify failed (exit %d)", iter, verify_rc);
         else
            LOG_INFO("agent_loop", "iteration %d: verify passed", iter);
      }

      /* Accumulate context for potential next iteration */
      accumulate_context(loop, iter, completion, gaps,
                         (!verify_passed && verify_out) ? verify_out : NULL);
      free(verify_out);

      /* Check exit conditions */
      if (completion >= loop->completion_threshold && verify_passed)
      {
         LOG_INFO("agent_loop", "done after %d iteration(s): completion=%d%% verify=%s", iter,
                  completion, loop->verify_cmd[0] ? "passed" : "n/a");
         final_rc = 0;
         break;
      }

      if (iter == loop->max_iterations)
      {
         LOG_WARN("agent_loop", "max iterations (%d) reached; last completion=%d%% threshold=%d%%",
                  loop->max_iterations, completion, loop->completion_threshold);
         /* Return 0 with the last response — caller can inspect loop->last_completion */
         final_rc = 0;
         break;
      }
   }

   return final_rc;
}
