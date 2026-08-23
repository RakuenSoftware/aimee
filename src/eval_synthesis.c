/* eval_synthesis.c: the storage half of failure -> regression eval task.
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */

#include "eval_synthesis.h"

#include "agent_jobs.h" /* db1_agent_job_list_recent */
#include "eval.h"       /* db1_eval_candidate_* */
#include "log.h"
#include "modules/db2/c/db2_learning.h"
#include "platform_path.h"

#include <aimee/learning/learning.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define EVAL_SYNTHESIS_MAX_PENDING 64

int eval_synthesis_observe(const learning_eval_failure_t *f, const char *suite,
                           const char *session_id)
{
   if (!f)
      return -1;

   char signature[LEARNING_EVAL_SIGNATURE_LEN];
   int rc = learning_eval_signature(f, signature, sizeof(signature));
   if (rc == -2)
      return -2; /* inadmissible text: store nothing */
   if (rc != 0)
      return -1;

   char task_name[LEARNING_EVAL_TASK_NAME_LEN];
   if (learning_eval_task_name(signature, task_name, sizeof(task_name)) != 0)
      return -1;

   char task_json[DB1_EVAL_CAND_TASK_JSON_LEN];
   rc = learning_eval_build_task(f, task_name, task_json, sizeof(task_json));
   if (rc == -2)
      return -2;
   if (rc != 0)
      return -1;

   return db1_eval_candidate_observe(signature, (suite && suite[0]) ? suite : "regressions",
                                     task_name, task_json, f->origin, f->origin_ref, session_id);
}

/* --- Scan: the failure ledgers become candidate observations --- */

#define EVAL_SYNTHESIS_SCAN_JOBS    64
#define EVAL_SYNTHESIS_SCAN_SIGNALS 64

/* Trim a free-text field to something a stored task can hold, cutting at the
 * last space so the result reads as a phrase rather than a severed word. */
static void eval_synthesis_summarize(const char *src, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (!src || !src[0])
      return;
   snprintf(out, out_len, "%s", src);
   if (strlen(src) < out_len)
      return;
   char *last = strrchr(out, ' ');
   if (last && last != out)
      *last = '\0';
}

static void eval_synthesis_scan_jobs(int window_days, const char *suite,
                                     eval_synthesis_scan_stats_t *stats)
{
   (void)window_days; /* the ledger is listed newest-first and bounded by count */

   db1_agent_job_t jobs[EVAL_SYNTHESIS_SCAN_JOBS];
   int n = db1_agent_job_list_recent(jobs, EVAL_SYNTHESIS_SCAN_JOBS, 1 /* include prompt */);
   if (n <= 0)
      return;

   for (int i = 0; i < n; i++)
   {
      /* 'cancelled' is an operator decision, not a defect; 'done', 'pending',
       * and 'running' are not failures. */
      if (strcmp(jobs[i].status, "failed") != 0)
         continue;
      stats->jobs_seen++;

      if (!jobs[i].prompt || !jobs[i].prompt[0])
      {
         stats->skipped++; /* nothing to replay */
         continue;
      }

      char prompt[LEARNING_EVAL_MAX_FIELD + 1];
      char mode[LEARNING_EVAL_MAX_FIELD + 1];
      char ref[64];
      eval_synthesis_summarize(jobs[i].prompt, prompt, sizeof(prompt));
      eval_synthesis_summarize(jobs[i].result ? jobs[i].result : "agent job failed", mode,
                               sizeof(mode));
      snprintf(ref, sizeof(ref), "agent_job:%d", jobs[i].id);

      learning_eval_failure_t f;
      memset(&f, 0, sizeof(f));
      f.origin = "agent_job";
      f.origin_ref = ref;
      f.role = jobs[i].role;
      f.prompt = prompt;
      f.failure_mode = mode;
      /* No check: the bar is that this prompt now succeeds at all. */

      /* The job id doubles as the session identity here: two failures of the
       * same shape from two different jobs are two independent observations,
       * which is exactly the reproduction test admission applies. */
      int rc = eval_synthesis_observe(&f, suite, ref);
      if (rc == 0)
         stats->observed++;
      else if (rc == -2)
         stats->rejected_text++;
      else
         stats->skipped++;
   }

   for (int i = 0; i < n; i++)
      db1_agent_job_free(&jobs[i]);
}

static void eval_synthesis_scan_signals(int window_days, const char *suite,
                                        eval_synthesis_scan_stats_t *stats)
{
#if defined(AIMEE_DB2_DISABLED)
   /* Correction signals live in DB2. A build without it still scans the job
    * ledger; it simply has one fewer source. */
   (void)window_days;
   (void)suite;
   (void)stats;
#else
   db2_learning_negative_signal_t rows[EVAL_SYNTHESIS_SCAN_SIGNALS];
   int n = db2_learning_negative_signals_recent(window_days, rows, EVAL_SYNTHESIS_SCAN_SIGNALS);
   if (n <= 0)
      return;

   for (int i = 0; i < n; i++)
   {
      stats->signals_seen++;

      /* The description is what was being discussed; the title is the fallback
       * when a caller filled only that in. */
      const char *body = rows[i].description[0] ? rows[i].description : rows[i].title;
      if (!body || !body[0])
      {
         stats->skipped++; /* nothing to replay */
         continue;
      }

      char prompt[LEARNING_EVAL_MAX_FIELD + 1];
      char correction[LEARNING_EVAL_MAX_FIELD + 1];
      char mode[LEARNING_EVAL_MAX_FIELD + 1];
      char ref[64];
      eval_synthesis_summarize(body, prompt, sizeof(prompt));
      eval_synthesis_summarize(rows[i].correction_text, correction, sizeof(correction));
      snprintf(mode, sizeof(mode), "corrected: %s", rows[i].signal_type);
      snprintf(ref, sizeof(ref), "signal:%lld", (long long)rows[i].id);

      learning_eval_failure_t f;
      memset(&f, 0, sizeof(f));
      f.origin = "correction";
      f.origin_ref = ref;
      f.role = "execute";
      f.prompt = prompt;
      f.failure_mode = mode;
      f.check_type = "contains";
      f.check_value = correction;

      const char *session = rows[i].source_session[0] ? rows[i].source_session : ref;
      int rc = eval_synthesis_observe(&f, suite, session);
      if (rc == 0)
         stats->observed++;
      else if (rc == -2)
         stats->rejected_text++;
      else
         stats->skipped++;
   }
#endif
}

int eval_synthesis_scan_failures(int window_days, const char *suite,
                                 eval_synthesis_scan_stats_t *out)
{
   eval_synthesis_scan_stats_t local;
   eval_synthesis_scan_stats_t *stats = out ? out : &local;
   memset(stats, 0, sizeof(*stats));

   if (window_days <= 0)
      window_days = LEARNING_METRICS_DEFAULT_WINDOW_DAYS;

   eval_synthesis_scan_jobs(window_days, suite, stats);
   eval_synthesis_scan_signals(window_days, suite, stats);

   LOG_INFO("eval_synthesis",
            "scan: %d failed job(s), %d correction signal(s) -> %d observation(s)"
            " (%d refused for unsafe text, %d skipped)",
            stats->jobs_seen, stats->signals_seen, stats->observed, stats->rejected_text,
            stats->skipped);
   return stats->observed;
}

/* Write one task file. Returns 0 on success, -1 on any filesystem failure —
 * the caller leaves the candidate pending rather than marking it admitted. */
static int eval_synthesis_write_task(const char *suite_dir, const char *task_name,
                                     const char *task_json, char *path_out, size_t path_len)
{
   if (!suite_dir || !suite_dir[0] || !task_name || !task_name[0] || !task_json)
      return -1;
   if (platform_mkdir_p(suite_dir, 0755) != 0)
      return -1;

   int written = snprintf(path_out, path_len, "%s/%s.json", suite_dir, task_name);
   if (written < 0 || (size_t)written >= path_len)
      return -1;

   FILE *fp = fopen(path_out, "w");
   if (!fp)
      return -1;
   int ok = fputs(task_json, fp) >= 0 && fputc('\n', fp) != EOF;
   if (fclose(fp) != 0)
      ok = 0;
   return ok ? 0 : -1;
}

int eval_synthesis_admit_pending(const char *suite_dir, const char *admitted_by,
                                 int min_occurrences)
{
   if (!suite_dir || !suite_dir[0])
      return -1;

   /* One gate check for the whole pass: a loop feeding on its own output does
    * not get to widen its own yardstick. */
   learning_endogeneity_t endo;
   learning_gate_state_t gate = learning_gate_check(&endo);
   if (gate != LEARNING_GATE_OPEN)
   {
      LOG_INFO("eval_synthesis",
               "admission held, endogeneity gate %s (exogenous %.2f of %lld committed)",
               gate == LEARNING_GATE_CLOSED_ENDOGENOUS ? "closed" : "unavailable",
               endo.exogenous_ratio, (long long)endo.committed_total);
      return 0;
   }

   db1_eval_candidate_t rows[EVAL_SYNTHESIS_MAX_PENDING];
   int n = db1_eval_candidate_list("candidate", rows, EVAL_SYNTHESIS_MAX_PENDING);
   if (n <= 0)
      return n < 0 ? -1 : 0;

   int admitted = 0;
   for (int i = 0; i < n; i++)
   {
      if (!learning_eval_admission_ready(rows[i].occurrences, rows[i].distinct_sessions,
                                         min_occurrences, 1))
         continue;

      char path[DB1_EVAL_CAND_PATH_LEN];
      if (eval_synthesis_write_task(suite_dir, rows[i].task_name, rows[i].task_json, path,
                                    sizeof(path)) != 0)
      {
         LOG_WARN("eval_synthesis", "could not materialise %s under %s; leaving it pending",
                  rows[i].task_name, suite_dir);
         continue;
      }
      if (db1_eval_candidate_mark_admitted(
              rows[i].id, (admitted_by && admitted_by[0]) ? admitted_by : "auto", path) != 0)
      {
         LOG_WARN("eval_synthesis", "wrote %s but could not mark it admitted", path);
         continue;
      }
      admitted++;
   }
   return admitted;
}

/* --- Retirement: a check that stops catching anything stops being run --- */

#define EVAL_SYNTHESIS_MAX_RESULTS 200

/* The most recent recorded outcome for `task_name` in `suite`.
 * Returns 1 = passed, 0 = failed, -1 = no result recorded yet. */
static int eval_synthesis_latest_outcome(const char *suite, const char *task_name)
{
   db1_eval_display_row_t rows[EVAL_SYNTHESIS_MAX_RESULTS];
   int n =
       db1_eval_results_list((suite && suite[0]) ? suite : NULL, rows, EVAL_SYNTHESIS_MAX_RESULTS);
   /* db1_eval_results_list returns newest first, so the first match wins. */
   for (int i = 0; i < n; i++)
      if (strcmp(rows[i].task_name, task_name) == 0)
         return rows[i].success ? 1 : 0;
   return -1;
}

int eval_synthesis_retire(const char *suite_dir, int retire_windows)
{
   if (!suite_dir || !suite_dir[0])
      return -1;
   if (retire_windows <= 0)
      retire_windows = EVAL_SYNTHESIS_RETIRE_WINDOWS;

   db1_eval_candidate_t rows[EVAL_SYNTHESIS_MAX_PENDING];
   int n = db1_eval_candidate_list("admitted", rows, EVAL_SYNTHESIS_MAX_PENDING);
   if (n <= 0)
      return n < 0 ? -1 : 0;

   int retired = 0;
   for (int i = 0; i < n; i++)
   {
      int outcome = eval_synthesis_latest_outcome(rows[i].suite, rows[i].task_name);
      if (outcome < 0)
         continue; /* never run yet: no evidence either way */

      if (outcome == 0)
      {
         /* It caught something. Reset the clock — this check is earning its
          * place in every gate run. */
         if (rows[i].passing_windows != 0)
            (void)db1_eval_candidate_set_passing_windows(rows[i].id, 0);
         continue;
      }

      int windows = rows[i].passing_windows + 1;
      if (windows < retire_windows)
      {
         (void)db1_eval_candidate_set_passing_windows(rows[i].id, windows);
         continue;
      }

      /* Quiet for long enough: archive the row and take the file back out of
       * the hot suite. A missing file is not an error — an operator may have
       * removed it already. */
      if (rows[i].admitted_path[0] && unlink(rows[i].admitted_path) != 0)
         LOG_WARN("eval_synthesis", "retiring %s: could not remove %s", rows[i].task_name,
                  rows[i].admitted_path);
      if (db1_eval_candidate_mark_archived(rows[i].id) != 0)
      {
         LOG_WARN("eval_synthesis", "could not archive %s", rows[i].task_name);
         continue;
      }
      (void)db1_eval_candidate_set_passing_windows(rows[i].id, windows);
      retired++;
   }
   return retired;
}
