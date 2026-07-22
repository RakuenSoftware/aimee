/* memory_lifecycle.c: lifecycle state machine, pending-TTL sweep, and the
 * memory_alerts bundle. SQL surface lives in db2/memory_lifecycle.c — this
 * file owns validation, transition rules, and the alert JSON assembly.
 *
 * See docs/proposals/done/memory-lifecycle-states-and-alerts.md for design
 * notes; public contract in src/headers/memory.h. */
#include "aimee.h"
#include "util.h"
#include "cJSON.h"
#if !defined(AIMEE_DB2_DISABLED)
#include "db2/memory_lifecycle.h"
#include "dogfood.h"
#include "log.h"
#endif
#include "memory.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- Commitment-shape detection ---
 *
 * Three shape bands per the proposal:
 *   - "date":       explicit date anchor; TTL = parsed date + 1 day (we
 *                   can't parse every date form, so fall back to the
 *                   relative default when absent)
 *   - "relative":   "this week", "next week", "next month", "today",
 *                   "tomorrow"
 *   - "open-ended": future-tense + agent subject, no time anchor
 *
 * Returns 1 when the content reads as a commitment. The heuristic errs
 * toward under-tagging since the proposal's Trade-offs note says
 * ambiguous content should default to `active`. */
static int lc_contains_phrase(const char *content_lc, const char *phrase)
{
   if (!content_lc || !phrase)
      return 0;
   return strstr(content_lc, phrase) != NULL;
}

int memory_detect_commitment_shape(const char *content, char *shape_out, size_t shape_out_len,
                                   int *ttl_days_out)
{
   if (shape_out && shape_out_len > 0)
      shape_out[0] = '\0';
   if (ttl_days_out)
      *ttl_days_out = 0;
   if (!content || !content[0])
      return 0;

   char lc[1024];
   util_lower_copy(content, lc, sizeof(lc));

   /* Agent-role subject + future tense — the strongest signal. Matches
    * "i'll", "i will", "we'll", "we will", "i'm going to", "we're going
    * to". Rejects third-party future ("they'll review it") which isn't
    * a commitment *we* need to surface. */
   int future_agent =
       lc_contains_phrase(lc, "i'll ") || lc_contains_phrase(lc, "i will ") ||
       lc_contains_phrase(lc, "we'll ") || lc_contains_phrase(lc, "we will ") ||
       lc_contains_phrase(lc, "i'm going to ") || lc_contains_phrase(lc, "we're going to ") ||
       lc_contains_phrase(lc, "i am going to ") || lc_contains_phrase(lc, "we are going to ") ||
       lc_contains_phrase(lc, "i plan to ") || lc_contains_phrase(lc, "we plan to ");

   /* Explicit "pending:" / "todo:" caller tags — trust them regardless of
    * surrounding tense. */
   int explicit_tag = lc_contains_phrase(lc, "pending:") || lc_contains_phrase(lc, "todo:") ||
                      lc_contains_phrase(lc, "[pending]") || lc_contains_phrase(lc, "[todo]");

   if (!future_agent && !explicit_tag)
      return 0;

   /* Date anchors: "by <weekday>", "by <date>", "on <date>". Without a
    * full date parser we can't produce a wall-clock TTL, so the band is
    * tagged "date" and we fall back to the relative default. */
   static const char *date_markers[] = {
       "by monday",    "by tuesday", "by wednesday",  "by thursday", "by friday",  "by saturday",
       "by sunday",    "by eod",     "by end of day", "by noon",     "by morning", "by tomorrow",
       "by next week", "by 2026",    "by 2027",       NULL};
   for (int i = 0; date_markers[i]; i++)
   {
      if (strstr(lc, date_markers[i]))
      {
         if (shape_out && shape_out_len > 0)
            snprintf(shape_out, shape_out_len, "%s", "date");
         if (ttl_days_out)
            *ttl_days_out = MEMORY_LIFECYCLE_TTL_DEFAULT_RELATIVE_DAYS;
         return 1;
      }
   }

   /* Relative time windows. */
   static const char *relative_markers[] = {"this week",  "next week",  "today",         "tomorrow",
                                            "next month", "this month", "in a few days", NULL};
   for (int i = 0; relative_markers[i]; i++)
   {
      if (strstr(lc, relative_markers[i]))
      {
         if (shape_out && shape_out_len > 0)
            snprintf(shape_out, shape_out_len, "%s", "relative");
         if (ttl_days_out)
            *ttl_days_out = MEMORY_LIFECYCLE_TTL_DEFAULT_RELATIVE_DAYS;
         return 1;
      }
   }

   /* Agent commitment with no time anchor — open-ended 30 day horizon. */
   if (shape_out && shape_out_len > 0)
      snprintf(shape_out, shape_out_len, "%s", "open-ended");
   if (ttl_days_out)
      *ttl_days_out = MEMORY_LIFECYCLE_TTL_DEFAULT_OPEN_ENDED_DAYS;
   return 1;
}

#if defined(AIMEE_DB2_DISABLED)
/* Lifecycle mutations and alert queries are DB2-owned. Server code should
 * access them through aimee-kb RPCs, not by linking DB2 lifecycle helpers. */
int memory_transition_lifecycle(int64_t memory_id, const char *new_state,
                                const char *archive_reason)
{
   (void)memory_id;
   (void)new_state;
   (void)archive_reason;
   return -1;
}

int memory_mark_pending(int64_t memory_id, int ttl_days)
{
   (void)memory_id;
   (void)ttl_days;
   return -1;
}

int memory_lifecycle_sweep_expired(void)
{
   return 0;
}

int memory_lifecycle_counts(memory_lifecycle_counts_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

cJSON *memory_alerts(const char *since)
{
   (void)since;
   return NULL;
}
#else
/* --- State machine ---
 *
 * Transitions are whitelisted rather than inferred. Invalid transitions
 * (e.g. `archived -> active`) return -1 so callers get a clear error;
 * the proposal explicitly requires "invalid transitions return an
 * error, not silently drop." */
static int lc_is_valid_state(const char *state)
{
   if (!state || !state[0])
      return 0;
   return (strcmp(state, MEMORY_LIFECYCLE_STATE_ACTIVE) == 0 ||
           strcmp(state, MEMORY_LIFECYCLE_STATE_PENDING) == 0 ||
           strcmp(state, MEMORY_LIFECYCLE_STATE_FULFILLED) == 0 ||
           strcmp(state, MEMORY_LIFECYCLE_STATE_SUPERSEDED) == 0 ||
           strcmp(state, MEMORY_LIFECYCLE_STATE_ARCHIVED) == 0);
}

static int lc_transition_allowed(const char *from, const char *to)
{
   if (!lc_is_valid_state(from) || !lc_is_valid_state(to))
      return 0;
   if (strcmp(from, to) == 0)
      return 0; /* no-op transitions treated as invalid so callers notice */

   if (strcmp(from, MEMORY_LIFECYCLE_STATE_ACTIVE) == 0)
      return strcmp(to, MEMORY_LIFECYCLE_STATE_SUPERSEDED) == 0 ||
             strcmp(to, MEMORY_LIFECYCLE_STATE_ARCHIVED) == 0 ||
             strcmp(to, MEMORY_LIFECYCLE_STATE_PENDING) == 0;
   if (strcmp(from, MEMORY_LIFECYCLE_STATE_PENDING) == 0)
      return strcmp(to, MEMORY_LIFECYCLE_STATE_FULFILLED) == 0 ||
             strcmp(to, MEMORY_LIFECYCLE_STATE_ARCHIVED) == 0;
   if (strcmp(from, MEMORY_LIFECYCLE_STATE_FULFILLED) == 0)
      return strcmp(to, MEMORY_LIFECYCLE_STATE_ARCHIVED) == 0;
   if (strcmp(from, MEMORY_LIFECYCLE_STATE_SUPERSEDED) == 0)
      return strcmp(to, MEMORY_LIFECYCLE_STATE_ARCHIVED) == 0;
   /* archived is terminal */
   return 0;
}

int memory_transition_lifecycle(int64_t memory_id, const char *new_state,
                                const char *archive_reason)
{
   if (memory_id <= 0 || !lc_is_valid_state(new_state))
      return -1;

   char current[32];
   if (db2_memory_lifecycle_get_state(memory_id, current, sizeof(current)) != 0)
      return -1;
   if (!lc_transition_allowed(current, new_state))
      return -1;

   if (db2_memory_lifecycle_update_state(memory_id, new_state, archive_reason) != 0)
      return -1;

   const char *reason = (archive_reason && archive_reason[0]) ? archive_reason : "";
   aimee_log(LOG_INFO, "memory.lifecycle", "transition id=%lld from=%s to=%s reason=%s",
             (long long)memory_id, current, new_state, reason);
   return 0;
}

int memory_mark_pending(int64_t memory_id, int ttl_days)
{
   if (memory_id <= 0)
      return -1;
   if (ttl_days <= 0)
      ttl_days = MEMORY_LIFECYCLE_TTL_DEFAULT_OPEN_ENDED_DAYS;

   char current[32];
   if (db2_memory_lifecycle_get_state(memory_id, current, sizeof(current)) != 0)
      return -1;
   /* Only active rows become pending.  Re-marking an already-pending row
    * would reset the TTL silently, which would mask stale commitments. */
   if (strcmp(current, MEMORY_LIFECYCLE_STATE_ACTIVE) != 0)
      return -1;

   if (db2_memory_lifecycle_mark_pending(memory_id, ttl_days) != 0)
      return -1;

   aimee_log(LOG_INFO, "memory.lifecycle", "mark_pending id=%lld ttl_days=%d", (long long)memory_id,
             ttl_days);
   return 0;
}

int memory_lifecycle_sweep_expired(void)
{
   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);

   int archived = db2_memory_lifecycle_sweep_expired();

   clock_gettime(CLOCK_MONOTONIC, &t1);
   double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1.0e6;
   if (archived > 0)
      aimee_log(LOG_INFO, "memory.lifecycle", "sweep archived=%d elapsed_ms=%.2f", archived, ms);
   return archived;
}

int memory_lifecycle_counts(memory_lifecycle_counts_t *out)
{
   if (!out)
      return -1;
   db2_memory_lifecycle_counts_t db_counts;
   if (db2_memory_lifecycle_counts(&db_counts) != 0)
      return -1;
   memset(out, 0, sizeof(*out));
   out->active = db_counts.active;
   out->pending = db_counts.pending;
   out->fulfilled = db_counts.fulfilled;
   out->superseded = db_counts.superseded;
   out->archived = db_counts.archived;
   return 0;
}

/* --- Alerts ---
 *
 * Stale pending = currently-pending rows where the fraction of the TTL
 * window that has elapsed is >= 80%. */
static void lc_append_stale_pending(cJSON *arr, int max_rows)
{
   db2_memory_lifecycle_stale_t rows[64];
   if (max_rows > (int)(sizeof(rows) / sizeof(rows[0])))
      max_rows = (int)(sizeof(rows) / sizeof(rows[0]));
   int n = db2_memory_lifecycle_list_stale_pending(rows, max_rows);
   for (int i = 0; i < n; i++)
   {
      cJSON *row = cJSON_CreateObject();
      if (!row)
         break;
      cJSON_AddNumberToObject(row, "memory_id", (double)rows[i].memory_id);
      cJSON_AddStringToObject(row, "text", rows[i].text);
      cJSON_AddStringToObject(row, "created_at", rows[i].created_at);
      cJSON_AddStringToObject(row, "ttl_at", rows[i].ttl_at);
      cJSON_AddNumberToObject(row, "age_days", rows[i].age_days);
      cJSON_AddNumberToObject(row, "window_days", rows[i].window_days);
      cJSON_AddItemToArray(arr, row);
   }
}

static void lc_append_unresolved_contradictions(cJSON *arr, int max_rows)
{
   db2_memory_lifecycle_conflict_t rows[64];
   if (max_rows > (int)(sizeof(rows) / sizeof(rows[0])))
      max_rows = (int)(sizeof(rows) / sizeof(rows[0]));
   int n = db2_memory_lifecycle_list_unresolved_contradictions(rows, max_rows);
   for (int i = 0; i < n; i++)
   {
      cJSON *row = cJSON_CreateObject();
      if (!row)
         break;
      cJSON_AddNumberToObject(row, "conflict_id", (double)rows[i].conflict_id);
      cJSON *ids = cJSON_AddArrayToObject(row, "memory_ids");
      cJSON_AddItemToArray(ids, cJSON_CreateNumber((double)rows[i].memory_a_id));
      cJSON_AddItemToArray(ids, cJSON_CreateNumber((double)rows[i].memory_b_id));
      cJSON_AddStringToObject(row, "detected_at", rows[i].detected_at);
      cJSON_AddStringToObject(row, "topic", rows[i].a_key);
      cJSON_AddStringToObject(row, "a", rows[i].a_content);
      cJSON_AddStringToObject(row, "b", rows[i].b_content);
      cJSON_AddItemToArray(arr, row);
   }
}

static void lc_append_newly_superseded(cJSON *arr, const char *since, int max_rows)
{
   db2_memory_lifecycle_superseded_t rows[64];
   if (max_rows > (int)(sizeof(rows) / sizeof(rows[0])))
      max_rows = (int)(sizeof(rows) / sizeof(rows[0]));
   int n = db2_memory_lifecycle_list_newly_superseded(since, rows, max_rows);
   for (int i = 0; i < n; i++)
   {
      cJSON *row = cJSON_CreateObject();
      if (!row)
         break;
      cJSON_AddNumberToObject(row, "memory_id", (double)rows[i].memory_id);
      cJSON_AddStringToObject(row, "key", rows[i].key);
      cJSON_AddStringToObject(row, "text", rows[i].text);
      cJSON_AddStringToObject(row, "superseded_at", rows[i].superseded_at);
      cJSON_AddItemToArray(arr, row);
   }
}

cJSON *memory_alerts(const char *since)
{
   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);

   cJSON *bundle = cJSON_CreateObject();
   cJSON *stale = cJSON_CreateArray();
   cJSON *conflicts = cJSON_CreateArray();
   cJSON *superseded = cJSON_CreateArray();
   if (!bundle || !stale || !conflicts || !superseded)
   {
      cJSON_Delete(bundle);
      cJSON_Delete(stale);
      cJSON_Delete(conflicts);
      cJSON_Delete(superseded);
      return NULL;
   }
   cJSON_AddItemToObject(bundle, "stale_pending", stale);
   cJSON_AddItemToObject(bundle, "unresolved_contradictions", conflicts);
   cJSON_AddItemToObject(bundle, "newly_superseded", superseded);

   lc_append_stale_pending(stale, 50);
   lc_append_unresolved_contradictions(conflicts, 50);
   lc_append_newly_superseded(superseded, since, 50);

   clock_gettime(CLOCK_MONOTONIC, &t1);
   double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1.0e6;
   cJSON_AddNumberToObject(bundle, "elapsed_ms", ms);
   dogfood_log_moment_live("memory_alerts", since, NULL, 0, NULL);
   return bundle;
}
#endif
