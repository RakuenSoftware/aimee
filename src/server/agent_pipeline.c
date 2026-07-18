/* agent_pipeline.c: autonomous end-to-end pipeline state machine */
#include "aimee.h"
#include "db1.h"
#include "headers/agent_pipeline.h"
#include "headers/agent_coord.h"
#include "git_verify.h"
#include "headers/config.h"
#include "headers/index.h"
#include "kb_client.h"
#include "lifecycle.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static int count_words(const char *text)
{
   int in_word = 0;
   int words = 0;
   if (!text)
      return 0;
   while (*text)
   {
      if (isspace((unsigned char)*text))
      {
         in_word = 0;
      }
      else if (!in_word)
      {
         words++;
         in_word = 1;
      }
      text++;
   }
   return words;
}

static int task_has_anchor(const char *task)
{
   if (!task || !task[0])
      return 0;
   return strstr(task, "/") != NULL || strstr(task, ".c") != NULL || strstr(task, ".h") != NULL ||
          strstr(task, ".md") != NULL || strstr(task, ".json") != NULL ||
          strstr(task, "`") != NULL || strstr(task, "test") != NULL || strstr(task, "src") != NULL;
}

static int task_mentions_complexity(const char *task)
{
   static const char *words[] = {"refactor", "migrate",  "pipeline", "architecture",
                                 "parallel", "orchestr", "workflow", "integration",
                                 "database", "schema",   "review",   NULL};
   for (int i = 0; words[i]; i++)
      if (task && strstr(task, words[i]) != NULL)
         return 1;
   return 0;
}

void pipeline_classify_task(const char *task, const char *override, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   if (override && (strcmp(override, "trivial") == 0 || strcmp(override, "simple") == 0 ||
                    strcmp(override, "complex") == 0))
   {
      snprintf(out, out_len, "%s", override);
      return;
   }

   int words = count_words(task);
   if (words <= 6 && task_has_anchor(task) && !task_mentions_complexity(task))
      snprintf(out, out_len, "%s", "trivial");
   else if (words >= 20 || task_mentions_complexity(task) || (task && strstr(task, "\n")))
      snprintf(out, out_len, "%s", "complex");
   else
      snprintf(out, out_len, "%s", "simple");
}

static int task_needs_clarification(const char *task)
{
   int words = count_words(task);
   if (words <= 4)
      return 1;
   if (words <= 8 && !task_has_anchor(task))
      return 1;
   if (task && strstr(task, "something") != NULL)
      return 1;
   return 0;
}

static void summarize_spec(const char *spec, char *out, size_t out_len)
{
   size_t j = 0;
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (!spec)
      return;
   for (size_t i = 0; spec[i] && j + 1 < out_len; i++)
   {
      char c = spec[i];
      if (c == '\n' || c == '\r' || c == '\t')
         c = ' ';
      if (j > 0 && out[j - 1] == ' ' && c == ' ')
         continue;
      out[j++] = c;
   }
   out[j] = '\0';
}

static const char *effective_task_text(const db1_pipeline_t *p, char *buf, size_t buf_len)
{
   if (!p || !buf || buf_len == 0)
      return p ? p->task : "";
   if (p->clarify_session_id > 0)
   {
      clarify_session_t s;
      if (db1_clarify_get(p->clarify_session_id, &s) == 0 && s.status == CLARIFY_READY && s.spec[0])
      {
         summarize_spec(s.spec, buf, buf_len);
         return buf;
      }
   }
   summarize_spec(p->task, buf, buf_len);
   return buf;
}

static int is_path_char(int ch)
{
   return isalnum((unsigned char)ch) || ch == '_' || ch == '-' || ch == '.' || ch == '/' ||
          ch == '\\';
}

static int token_looks_like_path(const char *tok)
{
   static const char *exts[] = {".c",    ".h",   ".md",   ".json", ".txt", ".py",
                                ".js",   ".ts",  ".tsx",  ".jsx",  ".sh",  ".sql",
                                ".yaml", ".yml", ".toml", ".html", ".css", NULL};
   if (!tok || !tok[0] || strstr(tok, "://") != NULL || strchr(tok, '*') || strchr(tok, '?'))
      return 0;
   if (strchr(tok, '/') || strchr(tok, '\\'))
      return 1;
   for (int i = 0; exts[i]; i++)
      if (strlen(tok) > strlen(exts[i]) &&
          strcmp(tok + strlen(tok) - strlen(exts[i]), exts[i]) == 0)
         return 1;
   return 0;
}

static int path_ref_exists(const char *tok)
{
   char alt[MAX_PATH_LEN];
   if (!tok || !tok[0])
      return 0;
   if (access(tok, F_OK) == 0)
      return 1;
   if (strncmp(tok, "./", 2) != 0 && strncmp(tok, "../", 3) != 0)
   {
      snprintf(alt, sizeof(alt), "../%s", tok);
      if (access(alt, F_OK) == 0)
         return 1;
   }
   return 0;
}

static void trim_token(char *tok)
{
   size_t start = 0;
   size_t len = strlen(tok);
   while (start < len && (tok[start] == '"' || tok[start] == '\'' || tok[start] == '`' ||
                          tok[start] == '(' || tok[start] == '['))
      start++;
   while (len > start && (tok[len - 1] == '"' || tok[len - 1] == '\'' || tok[len - 1] == '`' ||
                          tok[len - 1] == ',' || tok[len - 1] == '.' || tok[len - 1] == ')' ||
                          tok[len - 1] == ']' || tok[len - 1] == ':'))
      len--;
   if (start > 0 || len < strlen(tok))
      memmove(tok, tok + start, len - start);
   tok[len - start] = '\0';
}

static int append_issue(char *buf, size_t buf_len, const char *fmt, const char *value)
{
   size_t used = strlen(buf);
   if (used >= buf_len)
      return 0;
   int written = snprintf(buf + used, buf_len - used, fmt, value);
   return written > 0 ? 1 : 0;
}

static int scan_text_for_missing_refs(const char *text, char *issues, size_t issues_len)
{
   char token[256];
   int errors = 0;
   char prev[64] = "";
   size_t tlen = 0;
   if (!text)
      return 0;

   for (size_t i = 0;; i++)
   {
      int ch = (unsigned char)text[i];
      if (is_path_char(ch))
      {
         if (tlen + 1 < sizeof(token))
            token[tlen++] = (char)ch;
      }
      else
      {
         if (tlen > 0)
         {
            token[tlen] = '\0';
            trim_token(token);
            if (token[0])
            {
               if ((strcmp(prev, "file") == 0 || token_looks_like_path(token)) &&
                   !path_ref_exists(token))
               {
                  errors += append_issue(issues, issues_len, "missing file reference: %s\n", token);
               }
               else if ((strcmp(prev, "symbol") == 0 || strcmp(prev, "function") == 0 ||
                         strcmp(prev, "method") == 0 || strcmp(prev, "class") == 0 ||
                         strcmp(prev, "struct") == 0 || strcmp(prev, "enum") == 0) &&
                        isalpha((unsigned char)token[0]))
               {
                  term_hit_t hits[1];
                  if (kb_client_index_find(token, hits, 1) <= 0)
                     errors +=
                         append_issue(issues, issues_len, "missing symbol reference: %s\n", token);
               }
               snprintf(prev, sizeof(prev), "%s", token);
            }
            tlen = 0;
         }
         if (ch == '\0')
            break;
      }
   }
   return errors;
}

static int validate_plan_references(int plan_id, char *msg, size_t msg_len)
{
   plan_t plan;
   int errors = 0;
   if (db1_execution_plan_get(plan_id, &plan) != 0)
   {
      if (msg && msg_len > 0)
         snprintf(msg, msg_len, "plan #%d could not be loaded for validation", plan_id);
      return -1;
   }

   if (msg && msg_len > 0)
      msg[0] = '\0';
   for (int i = 0; i < plan.step_count; i++)
   {
      plan_step_t *step = &plan.steps[i];
      errors += scan_text_for_missing_refs(step->action, msg, msg_len);
      errors += scan_text_for_missing_refs(step->precondition, msg, msg_len);
      errors += scan_text_for_missing_refs(step->success_predicate, msg, msg_len);
      errors += scan_text_for_missing_refs(step->rollback, msg, msg_len);
   }

   if (errors == 0)
   {
      if (msg && msg_len > 0)
         snprintf(msg, msg_len, "Plan #%d passed mechanical validation.", plan_id);
      return 0;
   }
   return -1;
}

/* ------------------------------------------------------------------ */
/* Phase handlers — return 1 if phase complete, 0 if still pending     */
/* ------------------------------------------------------------------ */

static int phase_classify(db1_pipeline_t *p, char *msg, size_t msg_len)
{
   if (task_needs_clarification(p->task))
   {
      clarify_session_t s;
      if (p->clarify_session_id <= 0)
      {
         int sid = db1_clarify_start(p->task, &s);
         if (sid > 0)
            p->clarify_session_id = sid;
      }
      else if (db1_clarify_get(p->clarify_session_id, &s) != 0)
      {
         memset(&s, 0, sizeof(s));
      }

      if (msg && msg_len > 0)
      {
         const char *question = (s.qa_count > 0) ? s.qa[s.qa_count - 1].question : "";
         snprintf(msg, msg_len,
                  "Task classified as %s and marked ambiguous. Clarification session #%d started.\n"
                  "Current question: %s",
                  p->request_classification[0] ? p->request_classification : "simple",
                  p->clarify_session_id, question[0] ? question : "(no pending question)");
      }
   }
   else if (msg && msg_len > 0)
   {
      snprintf(msg, msg_len, "Task classified as %s. Plan depth set to %s.",
               p->request_classification[0] ? p->request_classification : "simple",
               p->plan_depth[0] ? p->plan_depth : "simple");
   }

   return 1;
}

static int phase_clarify(db1_pipeline_t *p, char *msg, size_t msg_len)
{
   clarify_session_t s;
   if (p->clarify_session_id <= 0)
   {
      if (msg && msg_len > 0)
         snprintf(msg, msg_len, "No clarification session is linked. Advancing to plan phase.");
      return 1;
   }
   if (db1_clarify_get(p->clarify_session_id, &s) != 0)
   {
      if (msg && msg_len > 0)
         snprintf(msg, msg_len, "Clarification session #%d could not be loaded.",
                  p->clarify_session_id);
      return 0;
   }
   if (s.status == CLARIFY_READY && s.spec[0])
   {
      char summary[512];
      summarize_spec(s.spec, summary, sizeof(summary));
      if (msg && msg_len > 0)
         snprintf(msg, msg_len,
                  "Clarification session #%d is ready (score %.2f). Using clarified spec for "
                  "planning: %s",
                  s.id, s.score, summary);
      return 1;
   }

   if (msg && msg_len > 0)
   {
      const char *question = "";
      const char *dimension = "";
      for (int i = s.qa_count - 1; i >= 0; i--)
      {
         if (!s.qa[i].answered)
         {
            question = s.qa[i].question;
            dimension = s.qa[i].dimension;
            break;
         }
      }
      snprintf(msg, msg_len,
               "ACTION REQUIRED — Clarify phase:\n"
               "  Session #%d score: %.2f\n"
               "  Pending question [%s]: %s\n"
               "  Answer via: aimee clarify answer %d \"...\"\n"
               "  or MCP: clarify_answer { session_id: %d, answer: \"...\" }",
               s.id, s.score, dimension[0] ? dimension : "unknown",
               question[0] ? question : "(no pending question)", s.id, s.id);
   }
   return 0;
}

static int phase_plan(db1_pipeline_t *p, char *msg, size_t msg_len)
{
   /* Phase is complete when an execution_plan has been linked */
   if (p->plan_id > 0)
   {
      /* Verify the plan record actually exists */
      if (db1_execution_plan_exists(p->plan_id))
      {
         char validation[2048] = "";
         if (validate_plan_references(p->plan_id, validation, sizeof(validation)) != 0)
         {
            if (msg && msg_len > 0)
               snprintf(msg, msg_len,
                        "Plan #%d is linked but failed mechanical validation.\n%s"
                        "Fix the linked plan or create a new one before advancing.",
                        p->plan_id, validation);
            return 0;
         }
         if (msg && msg_len > 0)
            snprintf(msg, msg_len, "Plan #%d linked and validated. Advancing to execute phase.\n%s",
                     p->plan_id, validation);
         return 1;
      }
   }

   /* No plan yet — instruct agent to create one */
   if (msg && msg_len > 0)
   {
      char task[768];
      const char *effective = effective_task_text(p, task, sizeof(task));
      snprintf(msg, msg_len,
               "ACTION REQUIRED — Plan phase:\n"
               "  1. Use the 'plan' MCP tool to create an execution plan for:\n"
               "     \"%s\"\n"
               "     plan depth: %s\n"
               "  2. Once the plan is created, call:\n"
               "     aimee autopilot link-plan %d <plan_id>\n"
               "     or: autopilot { action:\"link-plan\", pipeline_id:%d, plan_id:<id> }\n"
               "  3. Then call advance again to continue.",
               effective, p->plan_depth[0] ? p->plan_depth : p->request_classification, p->id,
               p->id);
   }
   return 0;
}

static int phase_execute(db1_pipeline_t *p, char *msg, size_t msg_len)
{
   /* If plan has 0 steps, skip straight to QA */
   if (p->plan_id > 0)
   {
      int steps = db1_execution_plan_count_steps(p->plan_id);
      if (steps == 0)
      {
         if (msg && msg_len > 0)
            snprintf(msg, msg_len, "Plan #%d has no steps — skipping execute phase to QA.",
                     p->plan_id);
         return 1;
      }
   }

   /* If a coord job is linked, check its status */
   if (p->job_id > 0)
   {
      db1_coord_job_t job;
      if (db1_coord_job_get(p->job_id, &job) == 0)
      {
         if (strcmp(job.status, "complete") == 0 || strcmp(job.status, "done") == 0)
         {
            if (msg && msg_len > 0)
               snprintf(msg, msg_len, "Job #%d complete. Advancing to QA phase.", p->job_id);
            return 1;
         }
         if (strcmp(job.status, "failed") == 0 || strcmp(job.status, "error") == 0)
         {
            if (msg && msg_len > 0)
               snprintf(msg, msg_len,
                        "Job #%d failed (%s). Review delegate output, fix issues, "
                        "then resume this pipeline.",
                        p->job_id, job.status);
            return 0;
         }
         /* Still running */
         if (msg && msg_len > 0)
            snprintf(msg, msg_len, "Job #%d in progress (%s). Call advance again when ready.",
                     p->job_id, job.status);
         return 0;
      }
   }

   /* No job yet — instruct agent to start one */
   if (msg && msg_len > 0)
      snprintf(msg, msg_len,
               "ACTION REQUIRED — Execute phase:\n"
               "  1. Start a coordinated job for plan #%d:\n"
               "     job_start { plan_id: %d }\n"
               "  2. Once the job is created, call:\n"
               "     aimee autopilot link-job %d <job_id>\n"
               "     or: autopilot { action:\"link-job\", pipeline_id:%d, job_id:<id> }\n"
               "  3. Then call advance again to continue.",
               p->plan_id, p->plan_id, p->id, p->id);
   return 0;
}

static int phase_qa(db1_pipeline_t *p, char *msg, size_t msg_len)
{
   /* Run verify check (uses process CWD / verify config) */
   char verify_msg[2048] = "";
   int rc = verify_check(NULL, NULL, verify_msg, sizeof(verify_msg));
   if (rc == 0)
   {
      if (msg && msg_len > 0)
         snprintf(msg, msg_len, "QA passed. Advancing to validate phase.");
      return 1;
   }

   if (msg && msg_len > 0)
      snprintf(msg, msg_len,
               "QA checks failed for pipeline #%d.\n"
               "Verify output:\n%s\n"
               "Fix the issues, then call advance again.",
               p->id, verify_msg);
   return 0;
}

static int phase_validate(db1_pipeline_t *p, char *msg, size_t msg_len)
{
   /* Second verify pass */
   char verify_msg[2048] = "";
   int rc = verify_check(NULL, NULL, verify_msg, sizeof(verify_msg));
   if (rc == 0)
   {
      if (msg && msg_len > 0)
         snprintf(msg, msg_len,
                  "Validate passed. Pipeline complete!\n"
                  "Reminder: run 'aimee slop' before creating the PR.");
      return 1;
   }

   if (msg && msg_len > 0)
      snprintf(msg, msg_len,
               "Validate checks still failing for pipeline #%d.\n"
               "Verify output:\n%s\n"
               "Fix remaining issues, then call advance again.",
               p->id, verify_msg);
   return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/* Phase order array for display */
static const char *PHASES[] = {
    PIPE_PHASE_CLASSIFY, PIPE_PHASE_CLARIFY,  PIPE_PHASE_PLAN, PIPE_PHASE_EXECUTE,
    PIPE_PHASE_QA,       PIPE_PHASE_VALIDATE, PIPE_PHASE_DONE, NULL};

static int phase_index(const char *phase)
{
   for (int i = 0; PHASES[i]; i++)
      if (strcmp(phase, PHASES[i]) == 0)
         return i;
   return 0;
}

int pipeline_advance(int pipeline_id, char *msg, size_t msg_len)
{
   db1_pipeline_t p;
   if (db1_pipeline_get(pipeline_id, &p) != 0)
   {
      if (msg && msg_len > 0)
         snprintf(msg, msg_len, "Pipeline #%d not found.", pipeline_id);
      return PIPELINE_ADV_ERROR;
   }

   /* Terminal states */
   if (strcmp(p.status, PIPE_STATUS_COMPLETE) == 0 || strcmp(p.status, PIPE_STATUS_FAILED) == 0 ||
       strcmp(p.status, PIPE_STATUS_CANCELLED) == 0)
   {
      if (msg && msg_len > 0)
         snprintf(msg, msg_len, "Pipeline #%d is in terminal state: %s", pipeline_id, p.status);
      return PIPELINE_ADV_ERROR;
   }

   if (strcmp(p.status, PIPE_STATUS_PAUSED) == 0)
   {
      if (msg && msg_len > 0)
         snprintf(msg, msg_len, "Pipeline #%d is paused.", pipeline_id);
      return PIPELINE_ADV_PAUSED;
   }

   /* Dispatch to phase handler */
   int phase_done = 0;
   int max_attempts = PIPELINE_MAX_ATTEMPTS_PLAN;

   if (strcmp(p.current_phase, PIPE_PHASE_CLASSIFY) == 0)
   {
      max_attempts = 1;
      phase_done = phase_classify(&p, msg, msg_len);
   }
   else if (strcmp(p.current_phase, PIPE_PHASE_CLARIFY) == 0)
   {
      max_attempts = PIPELINE_MAX_ATTEMPTS_CLARIFY;
      phase_done = phase_clarify(&p, msg, msg_len);
   }
   else if (strcmp(p.current_phase, PIPE_PHASE_PLAN) == 0)
   {
      max_attempts = PIPELINE_MAX_ATTEMPTS_PLAN;
      phase_done = phase_plan(&p, msg, msg_len);
   }
   else if (strcmp(p.current_phase, PIPE_PHASE_EXECUTE) == 0)
   {
      max_attempts = PIPELINE_MAX_ATTEMPTS_EXECUTE;
      phase_done = phase_execute(&p, msg, msg_len);
   }
   else if (strcmp(p.current_phase, PIPE_PHASE_QA) == 0)
   {
      max_attempts = PIPELINE_MAX_ATTEMPTS_QA;
      phase_done = phase_qa(&p, msg, msg_len);
   }
   else if (strcmp(p.current_phase, PIPE_PHASE_VALIDATE) == 0)
   {
      max_attempts = PIPELINE_MAX_ATTEMPTS_VALIDATE;
      phase_done = phase_validate(&p, msg, msg_len);
   }
   else if (strcmp(p.current_phase, PIPE_PHASE_DONE) == 0)
   {
      db1_pipeline_update(pipeline_id, PIPE_STATUS_COMPLETE, PIPE_PHASE_DONE, 0, p.plan_id,
                          p.job_id, p.request_classification, p.plan_depth, p.clarify_session_id);
      if (msg && msg_len > 0)
         snprintf(msg, msg_len, "Pipeline #%d is complete.", pipeline_id);
      return PIPELINE_ADV_DONE;
   }

   if (phase_done)
   {
      /* Transition to next phase */
      int cur = phase_index(p.current_phase);
      const char *next = PHASES[cur + 1] ? PHASES[cur + 1] : PIPE_PHASE_DONE;
      if (strcmp(p.current_phase, PIPE_PHASE_CLASSIFY) == 0 && p.clarify_session_id <= 0)
         next = PIPE_PHASE_PLAN;
      const char *new_status =
          strcmp(next, PIPE_PHASE_DONE) == 0 ? PIPE_STATUS_COMPLETE : PIPE_STATUS_ACTIVE;
      db1_pipeline_update(pipeline_id, new_status, next, 0, p.plan_id, p.job_id,
                          p.request_classification, p.plan_depth, p.clarify_session_id);
      return (strcmp(next, PIPE_PHASE_DONE) == 0) ? PIPELINE_ADV_DONE : PIPELINE_ADV_PROGRESSED;
   }

   /* Phase not yet complete — increment attempt counter */
   int new_attempts = p.phase_attempts + 1;
   if (new_attempts >= max_attempts)
   {
      /* Circuit breaker tripped */
      db1_pipeline_update(pipeline_id, PIPE_STATUS_PAUSED, p.current_phase, new_attempts, p.plan_id,
                          p.job_id, p.request_classification, p.plan_depth, p.clarify_session_id);
      if (msg && msg_len > 0)
      {
         char existing[PIPELINE_MAX_MSG_LEN];
         snprintf(existing, sizeof(existing), "%s", msg && msg[0] ? msg : "");
         snprintf(msg, msg_len,
                  "%s\n\nCircuit breaker: pipeline #%d paused after %d attempts in %s phase.",
                  existing, pipeline_id, new_attempts, p.current_phase);
      }
      return PIPELINE_ADV_PAUSED;
   }

   db1_pipeline_update(pipeline_id, PIPE_STATUS_ACTIVE, p.current_phase, new_attempts, p.plan_id,
                       p.job_id, p.request_classification, p.plan_depth, p.clarify_session_id);
   return PIPELINE_ADV_PROGRESSED;
}

int pipeline_status_report(int pipeline_id, char *buf, size_t buf_len)
{
   db1_pipeline_t p;
   if (db1_pipeline_get(pipeline_id, &p) != 0)
      return -1;

   int cur = phase_index(p.current_phase);
   char phases_line[256] = "";
   size_t used = 0;
   for (int i = 0; PHASES[i]; i++)
   {
      if (i > 0 && used < sizeof(phases_line) - 4)
      {
         phases_line[used++] = ' ';
         phases_line[used++] = '-';
         phases_line[used++] = '>';
         phases_line[used++] = ' ';
      }
      const char *marker;
      if (i < cur)
         marker = "[done]";
      else if (i == cur)
         marker = "[-->]";
      else
         marker = "[    ]";
      size_t rem = sizeof(phases_line) - used - 1;
      int n = snprintf(phases_line + used, rem, "%s %s", marker, PHASES[i]);
      if (n > 0)
         used += (size_t)n < rem ? (size_t)n : rem - 1;
   }
   phases_line[used] = '\0';

   int n = snprintf(buf, buf_len,
                    "pipeline #%d  status:%s  phase:%s  class:%s  plan_depth:%s  attempts:%d/%s\n"
                    "task: %s\n"
                    "progress: %s\n"
                    "plan_id:%d  job_id:%d  clarify_session_id:%d  created:%s",
                    p.id, p.status, p.current_phase,
                    p.request_classification[0] ? p.request_classification : "simple",
                    p.plan_depth[0] ? p.plan_depth : "simple", p.phase_attempts,
                    strcmp(p.current_phase, PIPE_PHASE_CLASSIFY) == 0  ? "1"
                    : strcmp(p.current_phase, PIPE_PHASE_CLARIFY) == 0 ? "8"
                    : strcmp(p.current_phase, PIPE_PHASE_PLAN) == 0    ? "3"
                    : strcmp(p.current_phase, PIPE_PHASE_EXECUTE) == 0 ? "5"
                    : strcmp(p.current_phase, PIPE_PHASE_QA) == 0      ? "5"
                                                                       : "3",
                    p.task, phases_line, p.plan_id, p.job_id, p.clarify_session_id, p.created_at);
   return n;
}

int pipeline_list(char *buf, size_t buf_len, int limit)
{
   if (limit <= 0)
      limit = 50;

   db1_pipeline_t rows[50];
   if (limit > (int)(sizeof(rows) / sizeof(rows[0])))
      limit = (int)(sizeof(rows) / sizeof(rows[0]));
   int count = db1_pipeline_list_active(rows, limit);
   if (count <= 0)
      return count < 0 ? 0 : count;

   size_t used = 0;
   for (int i = 0; i < count; i++)
   {
      int id = rows[i].id;
      const char *task = rows[i].task;
      const char *status = rows[i].status;
      const char *phase = rows[i].current_phase;
      const char *classification = rows[i].request_classification;
      const char *updated = rows[i].updated_at;
      int n = snprintf(buf + used, buf_len - used, "#%d  %-8s  %-8s  %-7s  %s  %.60s\n", id,
                       status ? status : "?", phase ? phase : "?",
                       classification ? classification : "?", updated ? updated : "?",
                       task ? task : "");
      if (n > 0)
         used += (size_t)n < buf_len - used ? (size_t)n : buf_len - used - 1;
   }
   return count;
}

/* ------------------------------------------------------------------ */
/* MCP handler                                                         */
/* ------------------------------------------------------------------ */

char *handle_autopilot(cJSON *args)
{
   const char *action = "";
   cJSON *ja = cJSON_GetObjectItem(args, "action");
   if (ja && cJSON_IsString(ja))
      action = ja->valuestring;

   char out[PIPELINE_MAX_MSG_LEN];

   if (strcmp(action, "start") == 0)
   {
      const char *task = "";
      const char *plan_depth = NULL;
      cJSON *jt = cJSON_GetObjectItem(args, "task");
      if (jt && cJSON_IsString(jt))
         task = jt->valuestring;
      cJSON *jpd = cJSON_GetObjectItem(args, "plan_depth");
      if (jpd && cJSON_IsString(jpd))
         plan_depth = jpd->valuestring;
      if (!task[0])
         return strdup("{\"error\":\"task required for start\"}");

      char classification[16] = "";
      pipeline_classify_task(task, plan_depth, classification, sizeof(classification));
      int pipeline_id = 0;
      if (db1_pipeline_create(task, classification, classification, &pipeline_id) != 0)
         return strdup("{\"error\":\"could not create pipeline\"}");

      char msg[PIPELINE_MAX_MSG_LEN] = "";
      pipeline_advance(pipeline_id, msg, sizeof(msg));
      char report[PIPELINE_MAX_MSG_LEN];
      pipeline_status_report(pipeline_id, report, sizeof(report));
      snprintf(out, sizeof(out), "pipeline #%d created\n\n%s\n\n%s", pipeline_id, report, msg);
   }
   else if (strcmp(action, "advance") == 0)
   {
      cJSON *jid = cJSON_GetObjectItem(args, "pipeline_id");
      if (!jid || !cJSON_IsNumber(jid))
         return strdup("{\"error\":\"pipeline_id required\"}");
      int pid = (int)jid->valuedouble;
      char msg[PIPELINE_MAX_MSG_LEN] = "";
      int rc = pipeline_advance(pid, msg, sizeof(msg));
      char report[PIPELINE_MAX_MSG_LEN];
      pipeline_status_report(pid, report, sizeof(report));
      const char *label = (rc == PIPELINE_ADV_DONE)     ? "complete"
                          : (rc == PIPELINE_ADV_PAUSED) ? "paused"
                          : (rc == PIPELINE_ADV_ERROR)  ? "error"
                                                        : "progressed";
      snprintf(out, sizeof(out), "pipeline #%d: %s\n\n%s\n\n%s", pid, label, report, msg);
   }
   else if (strcmp(action, "status") == 0)
   {
      cJSON *jid = cJSON_GetObjectItem(args, "pipeline_id");
      if (!jid || !cJSON_IsNumber(jid))
         return strdup("{\"error\":\"pipeline_id required\"}");
      int pid = (int)jid->valuedouble;
      int n = pipeline_status_report(pid, out, sizeof(out));
      if (n < 0)
         snprintf(out, sizeof(out), "pipeline #%d not found", pid);
   }
   else if (strcmp(action, "list") == 0)
   {
      int n = pipeline_list(out, sizeof(out), 0);
      if (n == 0)
         snprintf(out, sizeof(out), "no active pipelines");
   }
   else if (strcmp(action, "cancel") == 0)
   {
      cJSON *jid = cJSON_GetObjectItem(args, "pipeline_id");
      if (!jid || !cJSON_IsNumber(jid))
         return strdup("{\"error\":\"pipeline_id required\"}");
      int pid = (int)jid->valuedouble;
      db1_pipeline_t p;
      int rc = (db1_pipeline_get(pid, &p) == 0) ? db1_pipeline_cancel(pid) : -1;
      snprintf(out, sizeof(out),
               rc == 0 ? "pipeline #%d cancelled" : "error cancelling pipeline #%d", pid);
   }
   else if (strcmp(action, "resume") == 0)
   {
      cJSON *jid = cJSON_GetObjectItem(args, "pipeline_id");
      if (!jid || !cJSON_IsNumber(jid))
         return strdup("{\"error\":\"pipeline_id required\"}");
      int pid = (int)jid->valuedouble;
      db1_pipeline_t p;
      int rc = -1;
      if (db1_pipeline_get(pid, &p) == 0 && strcmp(p.status, PIPE_STATUS_PAUSED) == 0)
         rc = db1_pipeline_update(pid, PIPE_STATUS_ACTIVE, p.current_phase, 0, p.plan_id, p.job_id,
                                  p.request_classification, p.plan_depth, p.clarify_session_id);
      snprintf(out, sizeof(out),
               rc == 0 ? "pipeline #%d resumed — call advance to continue"
                       : "error: pipeline #%d not paused or not found",
               pid);
   }
   else if (strcmp(action, "link-plan") == 0)
   {
      cJSON *jid = cJSON_GetObjectItem(args, "pipeline_id");
      cJSON *jpid = cJSON_GetObjectItem(args, "plan_id");
      if (!jid || !cJSON_IsNumber(jid) || !jpid || !cJSON_IsNumber(jpid))
         return strdup("{\"error\":\"pipeline_id and plan_id required\"}");
      int pid = (int)jid->valuedouble;
      int plan_id = (int)jpid->valuedouble;
      int rc = db1_pipeline_link_plan(pid, plan_id);
      snprintf(out, sizeof(out), rc == 0 ? "plan #%d linked to pipeline #%d" : "error linking plan",
               plan_id, pid);
   }
   else if (strcmp(action, "link-job") == 0)
   {
      cJSON *jid = cJSON_GetObjectItem(args, "pipeline_id");
      cJSON *jjid = cJSON_GetObjectItem(args, "job_id");
      if (!jid || !cJSON_IsNumber(jid) || !jjid || !cJSON_IsNumber(jjid))
         return strdup("{\"error\":\"pipeline_id and job_id required\"}");
      int pid = (int)jid->valuedouble;
      int job_id = (int)jjid->valuedouble;
      int rc = db1_pipeline_link_job(pid, job_id);
      snprintf(out, sizeof(out), rc == 0 ? "job #%d linked to pipeline #%d" : "error linking job",
               job_id, pid);
   }
   else
   {
      snprintf(out, sizeof(out),
               "autopilot actions: start, advance, status, list, cancel, resume, "
               "link-plan, link-job");
   }

   /* Wrap in simple JSON text result */
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "result", out);
   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   return json;
}
