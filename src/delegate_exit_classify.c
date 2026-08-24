/* delegate_exit_classify.c: what a delegate's exit means.
 *
 * Reading turn counts and write flags and deciding "this stalled without
 * writing" is a judgement about a run. It touches no database and answers no
 * query, so it is not storage, and it stayed in a DB1 source only because the
 * one function that records the judgement happens to write a row.
 *
 * The recording moved to the module; this did not. The module has no need of
 * it either: db1_delegate_learning_record now takes the failure mode as the
 * text it was always stored as, so the conversion happens once, here, on the
 * side that did the classifying.
 */
#include <stdio.h>
#include <string.h>

#include "db1_client/delegate_learning.h"

const char *dl_failure_mode_to_string(dl_failure_mode_t mode)
{
   switch (mode)
   {
   case DL_MODE_SUCCESS:
      return "success";
   case DL_MODE_STALL_NO_WRITES:
      return "stall/no-writes";
   case DL_MODE_STALL_SLOW_WRITES:
      return "stall/slow-writes";
   case DL_MODE_MAX_TURNS:
      return "max-turns";
   case DL_MODE_DRIFT_PREFLIGHT:
      return "drift/pre-flight";
   case DL_MODE_DRIFT_BRANCH:
      return "drift/branch";
   case DL_MODE_CANCELLED:
      return "cancelled";
   default:
      return "unknown";
   }
}

dl_failure_mode_t dl_string_to_failure_mode(const char *s)
{
   if (!s)
      return DL_MODE_SUCCESS;
   if (strcmp(s, "success") == 0)
      return DL_MODE_SUCCESS;
   if (strcmp(s, "stall/no-writes") == 0)
      return DL_MODE_STALL_NO_WRITES;
   if (strcmp(s, "stall/slow-writes") == 0)
      return DL_MODE_STALL_SLOW_WRITES;
   if (strcmp(s, "max-turns") == 0)
      return DL_MODE_MAX_TURNS;
   if (strcmp(s, "drift/pre-flight") == 0)
      return DL_MODE_DRIFT_PREFLIGHT;
   if (strcmp(s, "drift/branch") == 0)
      return DL_MODE_DRIFT_BRANCH;
   if (strcmp(s, "cancelled") == 0)
      return DL_MODE_CANCELLED;
   return DL_MODE_SUCCESS;
}

/* ── Classification logic ──────────────────────────────────────────────── */

void classify_delegate_exit(const dl_exit_metrics_t *m, dl_classification_t *out)
{
   memset(out, 0, sizeof(*out));

   if (!m->success)
   {
      /* Failure path — determine specific mode */
      if (m->write_enforce_fired && !m->had_writes)
      {
         /* Stall with no writes */
         out->failure_mode = DL_MODE_STALL_NO_WRITES;
         if (m->turns >= 14)
            out->confidence = 0.9;
         else
            out->confidence = 0.6;
         snprintf(out->lesson, sizeof(out->lesson),
                  "Delegate stalled with zero writes after %d turns in role '%s'. "
                  "Ensure the task is actionable and files are writable. "
                  "Break large tasks into smaller sub-delegations.",
                  m->turns, m->role ? m->role : "unknown");
         snprintf(out->evidence, sizeof(out->evidence),
                  "{\"turns\":%d,\"tool_calls\":%d,\"had_writes\":0,\"write_enforce\":1}", m->turns,
                  m->tool_calls);
      }
      else if (m->write_enforce_fired && m->had_writes)
      {
         /* Stall but eventually wrote */
         out->failure_mode = DL_MODE_STALL_SLOW_WRITES;
         out->confidence = 0.6;
         snprintf(out->lesson, sizeof(out->lesson),
                  "Delegate was slow to produce writes (%d turns) in role '%s'. "
                  "Consider tightening the prompt to require early writes.",
                  m->turns, m->role ? m->role : "unknown");
         snprintf(out->evidence, sizeof(out->evidence),
                  "{\"turns\":%d,\"tool_calls\":%d,\"had_writes\":1,\"write_enforce\":1}", m->turns,
                  m->tool_calls);
      }
      else if (m->max_turns_limit > 0 && m->turns >= m->max_turns_limit)
      {
         /* Max turns exhausted */
         out->failure_mode = DL_MODE_MAX_TURNS;
         if (!m->had_writes)
            out->confidence = 0.85;
         else
            out->confidence = 0.6;
         snprintf(out->lesson, sizeof(out->lesson),
                  "Delegate hit max-turns limit (%d/%d) in role '%s'. "
                  "Task may be too complex for the turn budget. "
                  "Split into smaller delegations or increase max_turns.",
                  m->turns, m->max_turns_limit, m->role ? m->role : "unknown");
         snprintf(out->evidence, sizeof(out->evidence),
                  "{\"turns\":%d,\"max_turns\":%d,\"had_writes\":%d}", m->turns, m->max_turns_limit,
                  m->had_writes);
      }
      else if (m->error && strstr(m->error, "drift") != NULL)
      {
         /* Drift detected */
         if (strstr(m->error, "pre-flight") != NULL || strstr(m->error, "preflight") != NULL)
         {
            out->failure_mode = DL_MODE_DRIFT_PREFLIGHT;
         }
         else
         {
            out->failure_mode = DL_MODE_DRIFT_BRANCH;
         }
         out->confidence = 0.6;
         snprintf(out->lesson, sizeof(out->lesson),
                  "Delegate drifted from expected state in role '%s'. "
                  "Ground the prompt with current file contents or diff evidence.",
                  m->role ? m->role : "unknown");
         snprintf(out->evidence, sizeof(out->evidence), "{\"error\":\"%s\",\"turns\":%d}", m->error,
                  m->turns);
      }
      else if (m->error && strstr(m->error, "cancel") != NULL)
      {
         out->failure_mode = DL_MODE_CANCELLED;
         out->confidence = 0.6;
         snprintf(out->lesson, sizeof(out->lesson),
                  "Delegate was cancelled in role '%s' after %d turns. "
                  "Ensure the task can complete within the timeout.",
                  m->role ? m->role : "unknown", m->turns);
         snprintf(out->evidence, sizeof(out->evidence), "{\"error\":\"%s\",\"turns\":%d}", m->error,
                  m->turns);
      }
      else
      {
         /* Generic failure — classify as max-turns if turns were high, else unknown */
         out->failure_mode = DL_MODE_MAX_TURNS;
         out->confidence = 0.6;
         snprintf(out->lesson, sizeof(out->lesson),
                  "Delegate failed in role '%s' after %d turns: %s", m->role ? m->role : "unknown",
                  m->turns, m->error ? m->error : "unknown error");
         snprintf(out->evidence, sizeof(out->evidence),
                  "{\"error\":\"%s\",\"turns\":%d,\"tool_calls\":%d}",
                  m->error ? m->error : "unknown", m->turns, m->tool_calls);
      }
   }
   else
   {
      /* Success */
      out->failure_mode = DL_MODE_SUCCESS;
      out->confidence = 0.6;
      snprintf(out->lesson, sizeof(out->lesson),
               "Delegate succeeded in role '%s' in %d turns with %d tool calls. "
               "This pattern worked — reuse similar prompt structure.",
               m->role ? m->role : "unknown", m->turns, m->tool_calls);
      snprintf(out->evidence, sizeof(out->evidence),
               "{\"turns\":%d,\"tool_calls\":%d,\"had_writes\":%d}", m->turns, m->tool_calls,
               m->had_writes);
   }
}

/* ── DB1 operations ────────────────────────────────────────────────────── */

/* Schema is in src/db1/schema.sql; applied at db1_init time. */
