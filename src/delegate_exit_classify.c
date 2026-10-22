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
#include "cJSON.h"

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
   case DL_MODE_NO_PROGRESS:
      return "no-progress/retrieval-loop";
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
      return DL_MODE_UNKNOWN;
   if (strcmp(s, "success") == 0)
      return DL_MODE_SUCCESS;
   if (strcmp(s, "stall/no-writes") == 0)
      return DL_MODE_STALL_NO_WRITES;
   if (strcmp(s, "stall/slow-writes") == 0)
      return DL_MODE_STALL_SLOW_WRITES;
   if (strcmp(s, "max-turns") == 0)
      return DL_MODE_MAX_TURNS;
   if (strcmp(s, "no-progress/retrieval-loop") == 0)
      return DL_MODE_NO_PROGRESS;
   if (strcmp(s, "drift/pre-flight") == 0)
      return DL_MODE_DRIFT_PREFLIGHT;
   if (strcmp(s, "drift/branch") == 0)
      return DL_MODE_DRIFT_BRANCH;
   if (strcmp(s, "cancelled") == 0)
      return DL_MODE_CANCELLED;
   return DL_MODE_UNKNOWN;
}

/* ── Classification logic ──────────────────────────────────────────────── */

/* Error text is data. Never interpolate it into JSON or truncate an encoded
 * object into an apparently valid evidence record. The production error buffer
 * is 512 bytes; evidence capacity includes its worst-case JSON escaping. */
static void classify_error_evidence(dl_classification_t *out, const dl_exit_metrics_t *m, int tools,
                                    int no_writes)
{
   cJSON *object = cJSON_CreateObject();
   int ok = object && cJSON_AddStringToObject(object, "error", m->error ? m->error : "unknown") &&
            cJSON_AddNumberToObject(object, "turns", m->turns);
   if (ok && tools)
      ok = cJSON_AddNumberToObject(object, "tool_calls", m->tool_calls) != NULL;
   if (ok && m->max_turns_limit > 0)
      ok = cJSON_AddNumberToObject(object, "max_turns", m->max_turns_limit) != NULL;
   if (ok && m->write_enforce_fired)
      ok = cJSON_AddTrueToObject(object, "write_enforce") != NULL;
   if (ok && no_writes)
      ok = cJSON_AddStringToObject(object, "write_progress", "none_in_observed_guard_window") !=
           NULL;
   if (ok)
      ok = m->had_writes < 0
               ? cJSON_AddNullToObject(object, "had_writes") != NULL
               : cJSON_AddBoolToObject(object, "had_writes", m->had_writes > 0) != NULL;
   if (ok)
      ok = cJSON_AddStringToObject(object, "verification", "ungraded") != NULL;
   if (!ok || !cJSON_PrintPreallocated(object, out->evidence, sizeof out->evidence, 0))
      out->evidence[0] = '\0';
   cJSON_Delete(object);
}

void classify_delegate_exit(const dl_exit_metrics_t *m, dl_classification_t *out)
{
   memset(out, 0, sizeof(*out));

   if (!m->success)
   {
      /* Failure path — determine specific mode */
      if (m->error && strstr(m->error, "no-progress circuit breaker") != NULL)
      {
         out->failure_mode = DL_MODE_NO_PROGRESS;
         out->confidence = 0.95;
         snprintf(out->lesson, sizeof(out->lesson),
                  "A write-capable delegate repeatedly retrieved repository context without "
                  "editing. On a similar goal, start from a concrete defect hypothesis and "
                  "attempt the smallest justified edit or decisive test before broadening "
                  "exploration.");
         classify_error_evidence(out, m, 1, 1);
      }
      else if (m->write_enforce_fired && m->had_writes == 0)
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
         classify_error_evidence(out, m, 1, 0);
      }
      else if (m->write_enforce_fired && m->had_writes > 0)
      {
         /* Stall but eventually wrote */
         out->failure_mode = DL_MODE_STALL_SLOW_WRITES;
         out->confidence = 0.6;
         snprintf(out->lesson, sizeof(out->lesson),
                  "Delegate was slow to produce writes (%d turns) in role '%s'. "
                  "Consider tightening the prompt to require early writes.",
                  m->turns, m->role ? m->role : "unknown");
         classify_error_evidence(out, m, 1, 0);
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
         classify_error_evidence(out, m, 1, 0);
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
         classify_error_evidence(out, m, 0, 0);
      }
      else if (m->error && strstr(m->error, "cancel") != NULL)
      {
         out->failure_mode = DL_MODE_CANCELLED;
         out->confidence = 0.6;
         snprintf(out->lesson, sizeof(out->lesson),
                  "Delegate was cancelled in role '%s' after %d turns. "
                  "Ensure the task can complete within the timeout.",
                  m->role ? m->role : "unknown", m->turns);
         classify_error_evidence(out, m, 0, 0);
      }
      else
      {
         /* An opaque failure does not establish a turn-budget failure. */
         out->failure_mode = DL_MODE_UNKNOWN;
         out->confidence = 0.6;
         snprintf(out->lesson, sizeof(out->lesson),
                  "Delegate failed in role '%s' after %d turns: %s", m->role ? m->role : "unknown",
                  m->turns, m->error ? m->error : "unknown error");
         classify_error_evidence(out, m, 1, 0);
      }
   }
   else
   {
      /* Process completion is an observation, not an independent task grade. */
      out->failure_mode = DL_MODE_SUCCESS;
      out->confidence = 0.6;
      snprintf(out->lesson, sizeof(out->lesson),
               "Delegate process completed in role '%s' in %d turns with %d tool calls. "
               "Task correctness remains ungraded.",
               m->role ? m->role : "unknown", m->turns, m->tool_calls);
      cJSON *evidence = cJSON_CreateObject();
      int ok = evidence && cJSON_AddNumberToObject(evidence, "turns", m->turns) &&
               cJSON_AddNumberToObject(evidence, "tool_calls", m->tool_calls) &&
               cJSON_AddStringToObject(evidence, "verification", "ungraded");
      if (ok)
         ok = m->had_writes < 0
                  ? cJSON_AddNullToObject(evidence, "had_writes") != NULL
                  : cJSON_AddBoolToObject(evidence, "had_writes", m->had_writes > 0) != NULL;
      if (!ok || !cJSON_PrintPreallocated(evidence, out->evidence, sizeof out->evidence, 0))
         out->evidence[0] = 0;
      cJSON_Delete(evidence);
   }
}

/* ── DB1 operations ────────────────────────────────────────────────────── */

/* Schema is in src/db1/schema.sql; applied at db1_init time. */
