/* memory_prospective.c: "when X, surface Y" triggered-recall records —
 * the pre-turn matcher that lets reminders armed in one session fire on
 * a later one.  Extracted from memory_advanced.c so that file stays
 * under the 2000-line lint cap; nothing here depends on memory_advanced
 * internals, so the split is purely structural.
 *
 * Storage primitives live in db2/prospective_memories.c — this file
 * owns the orchestration: validation, metrics, dogfood logging,
 * tokenisation, and the staged matcher.
 *
 * See docs/proposals/done/prospective-memory-and-triggered-recall.md for
 * design notes and the public contract lives in src/headers/memory.h. */
#include "aimee.h"
#include "util.h"
#include "memory.h"
#if !defined(AIMEE_DB2_DISABLED)
#include "db2/prospective_memories.h"
#include "dogfood.h"
#include "log.h"
#endif
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- Prospective Memory and Triggered Recall ---
 *
 * See memory.h for the public contract. Detection/matching uses DB2 indexed
 * trigger_text plus exact-match anchors, so the hot path is bounded per turn.
 *
 * Metrics: process-local counters for trigger / complete / expire events
 * and a running match-latency total.  The dashboard card reads these on
 * demand; operators looking at the dashboard get what matters without a
 * cross-process aggregation store. */
#include <pthread.h>
static pthread_mutex_t s_pm_metrics_mu = PTHREAD_MUTEX_INITIALIZER;
static int64_t s_pm_metric_triggered_total = 0;
static int64_t s_pm_metric_completed_total = 0;
static int64_t s_pm_metric_expired_total = 0;
static int64_t s_pm_metric_match_calls = 0;
static double s_pm_metric_match_ms_sum = 0.0;
static double s_pm_metric_match_ms_max = 0.0;

void memory_prospective_metrics(int64_t *triggered_total, int64_t *completed_total,
                                int64_t *expired_total, int64_t *match_calls, double *match_ms_avg,
                                double *match_ms_max)
{
   pthread_mutex_lock(&s_pm_metrics_mu);
   if (triggered_total)
      *triggered_total = s_pm_metric_triggered_total;
   if (completed_total)
      *completed_total = s_pm_metric_completed_total;
   if (expired_total)
      *expired_total = s_pm_metric_expired_total;
   if (match_calls)
      *match_calls = s_pm_metric_match_calls;
   if (match_ms_avg)
      *match_ms_avg = s_pm_metric_match_calls > 0
                          ? s_pm_metric_match_ms_sum / (double)s_pm_metric_match_calls
                          : 0.0;
   if (match_ms_max)
      *match_ms_max = s_pm_metric_match_ms_max;
   pthread_mutex_unlock(&s_pm_metrics_mu);
}

#if defined(AIMEE_DB2_DISABLED)
/* Prospective memory storage and matching are DB2-owned. Server code must use
 * kb_client_* RPCs; these stubs keep the server object free of DB2 reach. */
int memory_prospective_create(const char *trigger_text, const char *action_text,
                              const char *anchor_entity, const char *anchor_file,
                              const char *recurrence, const char *valid_until,
                              const char *source_session, memory_prospective_t *out)
{
   (void)trigger_text;
   (void)action_text;
   (void)anchor_entity;
   (void)anchor_file;
   (void)recurrence;
   (void)valid_until;
   (void)source_session;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int memory_prospective_get(int64_t id, memory_prospective_t *out)
{
   (void)id;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int memory_prospective_list(const char *state, memory_prospective_t *out, int max)
{
   (void)state;
   (void)out;
   (void)max;
   return 0;
}

int memory_prospective_complete(int64_t id)
{
   (void)id;
   return -1;
}

int memory_prospective_sweep_expired(void)
{
   return 0;
}

int memory_prospective_match(const char *turn_text, const char *active_entity,
                             const char *active_file, memory_prospective_t *out, int max)
{
   (void)turn_text;
   (void)active_entity;
   (void)active_file;
   (void)out;
   (void)max;
   return 0;
}

int memory_prospective_mark_triggered(int64_t id)
{
   (void)id;
   return -1;
}
#else
static void pm_metrics_record_match(double ms)
{
   pthread_mutex_lock(&s_pm_metrics_mu);
   s_pm_metric_match_calls++;
   s_pm_metric_match_ms_sum += ms;
   if (ms > s_pm_metric_match_ms_max)
      s_pm_metric_match_ms_max = ms;
   pthread_mutex_unlock(&s_pm_metrics_mu);
}

static void pm_metrics_inc(int64_t *field)
{
   pthread_mutex_lock(&s_pm_metrics_mu);
   (*field)++;
   pthread_mutex_unlock(&s_pm_metrics_mu);
}

static void pm_metrics_add(int64_t *field, int n)
{
   if (n <= 0)
      return;
   pthread_mutex_lock(&s_pm_metrics_mu);
   *field += n;
   pthread_mutex_unlock(&s_pm_metrics_mu);
}

int memory_prospective_create(const char *trigger_text, const char *action_text,
                              const char *anchor_entity, const char *anchor_file,
                              const char *recurrence, const char *valid_until,
                              const char *source_session, memory_prospective_t *out)
{
   if (!trigger_text || !trigger_text[0] || !action_text || !action_text[0])
      return -1;

   const char *recur = (recurrence && recurrence[0]) ? recurrence : MEMORY_PROSPECTIVE_RECUR_ONCE;
   if (strcmp(recur, MEMORY_PROSPECTIVE_RECUR_ONCE) != 0 &&
       strcmp(recur, MEMORY_PROSPECTIVE_RECUR_REPEAT) != 0)
      return -1;

   int64_t new_id = db2_prospective_insert(trigger_text, action_text, anchor_entity, anchor_file,
                                           recur, valid_until, source_session);
   if (new_id <= 0)
      return -1;

   if (out)
      db2_prospective_get(new_id, out);
   return 0;
}

int memory_prospective_get(int64_t id, memory_prospective_t *out)
{
   if (!out || id <= 0)
      return -1;
   return db2_prospective_get(id, out);
}

int memory_prospective_list(const char *state, memory_prospective_t *out, int max)
{
   return db2_prospective_list(state, out, max);
}

int memory_prospective_complete(int64_t id)
{
   if (id <= 0)
      return -1;
   /* Reject transitions out of a terminal state so callers get a clear
    * error rather than silently double-completing. */
   memory_prospective_t cur;
   if (db2_prospective_get(id, &cur) != 0)
      return -1;
   if (strcmp(cur.state, MEMORY_PROSPECTIVE_STATE_COMPLETED) == 0 ||
       strcmp(cur.state, MEMORY_PROSPECTIVE_STATE_EXPIRED) == 0)
      return -1;

   if (db2_prospective_set_state(id, MEMORY_PROSPECTIVE_STATE_COMPLETED) != 0)
      return -1;
   pm_metrics_inc(&s_pm_metric_completed_total);
   return 0;
}

int memory_prospective_sweep_expired(void)
{
   int n = db2_prospective_sweep_expired();
   pm_metrics_add(&s_pm_metric_expired_total, n);
   return n;
}

/* Lowercase-copy helper shared by the matcher.  Keeps the semantics of
 * agg_normalize (collapses whitespace) but doesn't strip punctuation — we
 * want "src/tests/Rules.mk" to survive as a single token for file-anchor
 * matching. */
/* Append every row from `src` to `out` that's not already present in
 * `seen`. Stops when `out` fills up. Updates seen[] in place. */
static void pm_merge_unique(memory_prospective_t *out, int *out_count, int max, int64_t *seen,
                            int *seen_count, int seen_cap, const memory_prospective_t *src,
                            int src_count)
{
   for (int i = 0; i < src_count && *out_count < max; i++)
   {
      int dup = 0;
      for (int s = 0; s < *seen_count; s++)
      {
         if (seen[s] == src[i].id)
         {
            dup = 1;
            break;
         }
      }
      if (dup)
         continue;
      out[(*out_count)++] = src[i];
      if (*seen_count < seen_cap)
         seen[(*seen_count)++] = src[i].id;
   }
}

int memory_prospective_match(const char *turn_text, const char *active_entity,
                             const char *active_file, memory_prospective_t *out, int max)
{
   if (!out || max <= 0)
      return 0;

   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);

   /* Proactively expire stale armed reminders before matching so we don't
    * surface something past its due date. Cheap — bounded by the
    * (state, valid_until) index. */
   memory_prospective_sweep_expired();

   /* Use a set of seen ids to keep the staged queries from returning the
    * same reminder twice. */
   int64_t seen[MEMORY_PROSPECTIVE_MAX_MATCHES * 3];
   int seen_count = 0;
   const int seen_cap = (int)(sizeof(seen) / sizeof(seen[0]));
   int out_count = 0;

   /* Stage 1: exact entity anchor match — strongest signal. */
   if (active_entity && active_entity[0])
   {
      char ent_lc[MEMORY_PROSPECTIVE_ANCHOR_LEN];
      util_lower_copy(active_entity, ent_lc, sizeof(ent_lc));
      memory_prospective_t buf[MEMORY_PROSPECTIVE_MAX_MATCHES];
      int n = db2_prospective_list_by_entity(ent_lc, buf, max);
      pm_merge_unique(out, &out_count, max, seen, &seen_count, seen_cap, buf, n);
   }

   /* Stage 2: exact file anchor match. */
   if (active_file && active_file[0] && out_count < max)
   {
      memory_prospective_t buf[MEMORY_PROSPECTIVE_MAX_MATCHES];
      int n = db2_prospective_list_by_file(active_file, buf, max);
      pm_merge_unique(out, &out_count, max, seen, &seen_count, seen_cap, buf, n);
   }

   /* Stage 3: DB2-owned lexical trigger match against the current turn. */
   if (turn_text && turn_text[0] && out_count < max)
   {
      memory_prospective_t buf[MEMORY_PROSPECTIVE_MAX_MATCHES];
      int n = db2_prospective_list_by_trigger_terms(turn_text, buf, max);
      pm_merge_unique(out, &out_count, max, seen, &seen_count, seen_cap, buf, n);
   }

   /* Stage 4: fuzzy stem-overlap match — catches "rotate" vs "rotation"
    * and similar morphological variants the lexical trigger match misses. Only
    * runs when the earlier stages failed so the hot path stays cheap on
    * typical point queries.  Stems are produced by dropping common
    * suffixes from each token longer than 4 chars; we then check whether
    * any stem from the turn appears as a substring in the reminder's
    * trigger_text. */
   if (turn_text && turn_text[0] && out_count < max)
   {
      /* Collect up to 8 stems from the turn; deduplicated. */
      char stems[8][48];
      int stem_count = 0;
      size_t tlen = strlen(turn_text);
      for (size_t i = 0; i < tlen && stem_count < 8;)
      {
         while (i < tlen && !isalnum((unsigned char)turn_text[i]))
            i++;
         size_t start = i;
         while (i < tlen && (isalnum((unsigned char)turn_text[i]) || turn_text[i] == '_'))
            i++;
         size_t wlen = i - start;
         if (wlen < 5)
            continue;
         char stem[48];
         size_t copy = wlen < sizeof(stem) - 1 ? wlen : sizeof(stem) - 1;
         for (size_t k = 0; k < copy; k++)
            stem[k] = (char)tolower((unsigned char)turn_text[start + k]);
         stem[copy] = '\0';
         /* Drop common English suffixes so "rotation" / "rotate" / "rotating"
          * all collapse to "rotat". */
         static const char *suffixes[] = {"ations", "ations", "ization", "ations", "ations",
                                          "ings",   "ations", "tion",    "tions",  "sion",
                                          "sions",  "ment",   "ments",   "ness",   "ing",
                                          "ed",     "es",     "s",       NULL};
         for (int s = 0; suffixes[s]; s++)
         {
            size_t slen = strlen(suffixes[s]);
            if (copy > slen + 2 && strcmp(stem + copy - slen, suffixes[s]) == 0)
            {
               copy -= slen;
               stem[copy] = '\0';
               break;
            }
         }
         if (copy < 4)
            continue;
         int dup = 0;
         for (int k = 0; k < stem_count; k++)
            if (strcmp(stems[k], stem) == 0)
            {
               dup = 1;
               break;
            }
         if (!dup)
         {
            snprintf(stems[stem_count], sizeof(stems[stem_count]), "%s", stem);
            stem_count++;
         }
      }

      if (stem_count > 0)
      {
         /* Walk the armed rows and keep the ones whose trigger_text
          * contains any stem as a substring. The fetch is bounded by
          * armed-reminder count, which is tiny in practice. */
         memory_prospective_t armed[MEMORY_PROSPECTIVE_MAX_MATCHES * 4];
         int armed_count =
             db2_prospective_list_armed(armed, (int)(sizeof(armed) / sizeof(armed[0])));
         for (int r = 0; r < armed_count && out_count < max; r++)
         {
            char trigger_lc[MEMORY_PROSPECTIVE_TRIGGER_LEN];
            size_t tl = strlen(armed[r].trigger_text);
            if (tl >= sizeof(trigger_lc))
               tl = sizeof(trigger_lc) - 1;
            for (size_t k = 0; k < tl; k++)
               trigger_lc[k] = (char)tolower((unsigned char)armed[r].trigger_text[k]);
            trigger_lc[tl] = '\0';

            int matched = 0;
            for (int s = 0; s < stem_count && !matched; s++)
            {
               if (strstr(trigger_lc, stems[s]))
                  matched = 1;
            }
            if (!matched)
               continue;
            int dup = 0;
            for (int s = 0; s < seen_count; s++)
               if (seen[s] == armed[r].id)
               {
                  dup = 1;
                  break;
               }
            if (!dup && out_count < max)
            {
               out[out_count++] = armed[r];
               if (seen_count < seen_cap)
                  seen[seen_count++] = armed[r].id;
            }
         }
      }
   }

   clock_gettime(CLOCK_MONOTONIC, &t1);
   double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1.0e6;
   pm_metrics_record_match(ms);

   /* Tag dogfood when a reminder armed in a prior session surfaces here.
    * The monthly report calls these out explicitly — they're the whole
    * point of the cross-session prospective-memory flow. */
   if (out_count > 0)
   {
      int64_t ids[MEMORY_PROSPECTIVE_MAX_MATCHES];
      int n =
          out_count < MEMORY_PROSPECTIVE_MAX_MATCHES ? out_count : MEMORY_PROSPECTIVE_MAX_MATCHES;
      for (int i = 0; i < n; i++)
         ids[i] = out[i].id;
      dogfood_label_t label = {0};
      label.prospective_surfaced = 1;
      label.surprise = 1;
      label.has_surprise = 1;
      label.notes = "prospective-memory reminder surfaced";
      dogfood_log_moment_tagged("prospective_match", turn_text, ids, n, &label);
   }

   return out_count;
}

int memory_prospective_mark_triggered(int64_t id)
{
   if (id <= 0)
      return -1;
   memory_prospective_t cur;
   if (db2_prospective_get(id, &cur) != 0)
      return -1;
   if (strcmp(cur.state, MEMORY_PROSPECTIVE_STATE_ARMED) != 0)
      return -1;

   int is_once = strcmp(cur.recurrence, MEMORY_PROSPECTIVE_RECUR_ONCE) == 0;
   if (db2_prospective_record_trigger(id, is_once) != 0)
      return -1;

   pm_metrics_inc(&s_pm_metric_triggered_total);
   /* One log line per trigger with reminder id and recurrence so
    * operators can trace which reminder fired when. */
   aimee_log(LOG_INFO, "memory.prospective", "triggered id=%lld recurrence=%s trigger_count=%d",
             (long long)id, is_once ? "once" : "repeat", cur.trigger_count + 1);
   return 0;
}
#endif
