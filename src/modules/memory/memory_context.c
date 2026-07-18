/* memory_context.c: window compaction + retrieval planner + derive-facts
 * helpers + memory_retrieval_confidence. Extracted from memory_logic.c. */
#include "aimee.h"
#include "db1_optional.h"
#if !defined(AIMEE_DB2_DISABLED)
#include "db2/memory_query.h"
#include "db2/memory_relations.h"
#include "db2/rules.h"
#endif
#include "learning_evidence.h"
#include "memory.h"
#include "memory_context_internal.h"
#include "memory_ontology.h"
#include "cJSON.h"
#include "log.h"
#include "platform_process.h"
#include "workspace.h"
#include "kb.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* --- Window Compaction --- */

/* Check if a window's session has any L2 memories (high-value indicator) */
static int window_has_l2_link(int64_t window_id)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)window_id;
   return 0;
#else
   char session_id[128];
   if (!db1_window_session_id)
      return 0;
   if (db1_window_session_id(window_id, session_id, sizeof(session_id)) != 1 || !session_id[0])
      return 0;
   return db2_memory_count_l2_for_session(session_id) > 0;
#endif
}

/* Compute quality-scaled term retention count for a window */
static int compute_keep_terms(int64_t window_id, int base_terms)
{
   if (window_has_l2_link(window_id))
      return base_terms * 2; /* 2x for L2-linked windows */
   return base_terms;
}

int memory_compact_windows(int *summary_count, int *fact_count)
{
   int sc = 0, fc = 0;
   if (!db1_windows_list_ids_by_tier_before_days || !db1_window_prune_terms_keep_top ||
       !db1_window_set_tier || !db1_window_prune_files_keep_top || !db1_window_delete_all_files)
   {
      if (summary_count)
         *summary_count = 0;
      if (fact_count)
         *fact_count = 0;
      return 0;
   }

   /* raw -> summary (>30 days): prune terms by quality-scaled limit */
   {
      int64_t ids[500];
      int id_count = db1_windows_list_ids_by_tier_before_days("raw", SUMMARY_AGE, ids, 500);
      if (id_count < 0)
         id_count = 0;

      for (int i = 0; i < id_count; i++)
      {
         int keep = compute_keep_terms(ids[i], COMPACT_BASE_TERMS_SUMMARY);
         (void)db1_window_prune_terms_keep_top(ids[i], keep);
         (void)db1_window_set_tier(ids[i], "summary");
         sc++;
      }
   }

   /* summary -> fact (>90 days): prune terms, conditionally preserve file refs */
   {
      int64_t ids[500];
      int id_count = db1_windows_list_ids_by_tier_before_days("summary", FACT_AGE, ids, 500);
      if (id_count < 0)
         id_count = 0;

      for (int i = 0; i < id_count; i++)
      {
         int keep = compute_keep_terms(ids[i], COMPACT_BASE_TERMS_FACT);
         int is_l2 = window_has_l2_link(ids[i]);

         (void)db1_window_prune_terms_keep_top(ids[i], keep);
         if (is_l2)
            (void)db1_window_prune_files_keep_top(ids[i], COMPACT_FILE_REFS_KEEP);
         else
            (void)db1_window_delete_all_files(ids[i]);
         (void)db1_window_set_tier(ids[i], "fact");
         fc++;
      }
   }

   if (summary_count)
      *summary_count = sc;
   if (fact_count)
      *fact_count = fc;
   return 0;
}

/* --- Context Assembly --- */

/* --- Retrieval Planner --- */

/* Keyword lists per intent */
static const char *debug_keywords[] = {"fix",   "bug",      "error",     "fail",  "broken",
                                       "crash", "debug",    "issue",     "wrong", "stacktrace",
                                       "panic", "segfault", "exception", NULL};
static const char *plan_keywords[] = {"plan",   "design", "architect", "propose", "new", "feature",
                                      "create", "build",  "implement", "add",     NULL};
static const char *review_keywords[] = {"review", "check", "audit",   "convention", "style",
                                        "verify", "lint",  "inspect", NULL};
static const char *deploy_keywords[] = {"deploy",  "release", "ship",    "rollout", "migrate",
                                        "upgrade", "install", "publish", NULL};

/* Session-shaped query keywords: "what happened", "how did X go", "tell me about that session". */
static const char *session_keywords[] = {
    "what happened",   "how did",      "tell me about",      "recap",          "summary of",
    "session summary", "what went on", "what was discussed", "what did we do", NULL};

static int count_keyword_matches(const char *text, const char **keywords)
{
   int count = 0;
   for (int i = 0; keywords[i]; i++)
   {
      /* Case-insensitive substring search */
      const char *p = text;
      int klen = (int)strlen(keywords[i]);
      while (*p)
      {
         if (strncasecmp(p, keywords[i], klen) == 0)
         {
            /* Check word boundary: previous char must not be alpha */
            if (p == text || !isalpha((unsigned char)p[-1]))
            {
               count++;
               break;
            }
         }
         p++;
      }
   }
   return count;
}

/* Phrases that signal a temporal / recency-anchored query. When any match,
 * retrieval is biased toward memories with matching temporal markers and
 * recency weight is raised (LoCoMo Temporal). Single-word markers must match
 * as whole tokens to avoid spurious hits (e.g. "since" inside "business"). */
static const char *temporal_phrases[] = {
    "yesterday",   "last week", "last month", "before we",     "before the",
    "after we",    "after the", "previously", "used to",       "when we",
    "at the time", "back when", "prior to",   "earlier today", "this morning",
    "this week",   "recently",  "lately",     "since we",      "once we",
    NULL};

int has_temporal_markers(const char *text)
{
   if (!text || !text[0])
      return 0;

   char buf[2048];
   snprintf(buf, sizeof(buf), "%s", text);
   for (char *p = buf; *p; p++)
      *p = (char)tolower((unsigned char)*p);

   for (int i = 0; temporal_phrases[i]; i++)
   {
      if (strstr(buf, temporal_phrases[i]))
         return 1;
   }
   return 0;
}

/*
 * memory_is_session_query: return 1 if |task_hint| looks like a session-shaped
 * question ("what happened", "how did X go", "recap", "summary of session").
 * These queries should preferentially surface episode cards.
 */
int memory_is_session_query(const char *task_hint)
{
   if (!task_hint || !task_hint[0])
      return 0;
   /* Lower-case copy for case-insensitive matching */
   char lower[512];
   size_t i = 0;
   for (; task_hint[i] && i + 1 < sizeof(lower); i++)
      lower[i] = (char)tolower((unsigned char)task_hint[i]);
   lower[i] = '\0';

   for (int k = 0; session_keywords[k]; k++)
   {
      if (strstr(lower, session_keywords[k]))
         return 1;
   }
   return 0;
}

/* --- Quantitative / Date-Arithmetic Post-Retrieval Deriver --- */

/* Quantitative query keywords */
static const char *quantitative_keywords[] = {"how many",     "how often", "how much", "count",
                                              "number of",    "total",     "times",    "frequency",
                                              "how frequent", NULL};

/* Temporal-interval query keywords */
static const char *interval_keywords[] = {
    "how long ago", "how long since", "how long until", "days between", "days since",
    "days ago",     "weeks ago",      "months ago",     "years ago",    "time between",
    "time since",   "interval",       "duration",       "elapsed",      NULL};

/*
 * memory_classify_deriver_shape: detect if |query| is a quantitative or
 * temporal-interval question that the deriver can help with.
 */
memory_query_shape_t memory_classify_deriver_shape(const char *query)
{
   if (!query || !query[0])
      return MEM_SHAPE_UNKNOWN;

   char lower[512];
   size_t i = 0;
   for (; query[i] && i + 1 < sizeof(lower); i++)
      lower[i] = (char)tolower((unsigned char)query[i]);
   lower[i] = '\0';

   for (int k = 0; quantitative_keywords[k]; k++)
      if (strstr(lower, quantitative_keywords[k]))
         return MEM_SHAPE_QUANTITATIVE;

   for (int k = 0; interval_keywords[k]; k++)
      if (strstr(lower, interval_keywords[k]))
         return MEM_SHAPE_TEMPORAL_INTERVAL;

   return MEM_SHAPE_UNKNOWN;
}

/*
 * parse_iso8601_date: parse a date string like "2023-05-01" (or with a T
 * prefix) into a struct tm. Returns 0 on success, -1 on failure.
 */
static int parse_iso8601_date(const char *s, struct tm *out)
{
   if (!s || !s[0] || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   /* Accept "YYYY-MM-DD" or "YYYY-MM-DDThh..." */
   int y = 0, m = 0, d = 0;
   if (sscanf(s, "%d-%d-%d", &y, &m, &d) != 3)
      return -1;
   if (y < 1900 || m < 1 || m > 12 || d < 1 || d > 31)
      return -1;
   out->tm_year = y - 1900;
   out->tm_mon = m - 1;
   out->tm_mday = d;
   return 0;
}

/*
 * days_between: return the number of days between two ISO-8601 date strings.
 * Returns INT_MIN on parse error.
 */
static int days_between(const char *earlier, const char *later)
{
   struct tm a, b;
   if (parse_iso8601_date(earlier, &a) != 0 || parse_iso8601_date(later, &b) != 0)
      return (int)(-2147483647 - 1); /* INT_MIN */
   time_t ta = mktime(&a);
   time_t tb = mktime(&b);
   if (ta == (time_t)-1 || tb == (time_t)-1)
      return (int)(-2147483647 - 1);
   return (int)((tb - ta) / 86400);
}

/*
 * memory_derive_facts: scan memory_relations for candidate IDs and compute
 * counts, date diffs, and aggregated intervals.
 *
 * Current derivation rules:
 *   - SHAPE_QUANTITATIVE: count distinct OCCURRED_AT relations per candidate.
 *   - SHAPE_TEMPORAL_INTERVAL: compute date diffs between OCCURRED_AT dates;
 *     report min/max/avg gap and, if there is a "reference" date in the query
 *     (today = now_local_iso8601), the time since the most recent event.
 */
int memory_derive_facts(const char *query, int64_t *candidate_ids, int cand_count,
                        const config_t *cfg, memory_derived_facts_t *out)
{
   if (!out)
      return 0;
   memset(out, 0, sizeof(*out));

   if (!cfg || !cfg->memory_derive_facts_enabled)
      return 0;
   if (!query || !candidate_ids || cand_count <= 0)
      return 0;

   memory_query_shape_t shape = memory_classify_deriver_shape(query);
   if (shape != MEM_SHAPE_QUANTITATIVE && shape != MEM_SHAPE_TEMPORAL_INTERVAL)
      return 0;

#define MAX_DATES 256
   char dates[MAX_DATES][32];
   int date_count = 0;

#if !defined(AIMEE_DB2_DISABLED)
   for (int i = 0; i < cand_count && date_count < MAX_DATES; i++)
   {
      db2_memory_relation_date_row_t drows[64];
      int dn = db2_memory_relation_dates_for_memory(candidate_ids[i], drows, 64);
      for (int j = 0; j < dn && date_count < MAX_DATES; j++)
      {
         snprintf(dates[date_count], sizeof(dates[0]), "%s", drows[j].date);
         date_count++;
      }
   }
#else
   (void)candidate_ids;
   (void)cand_count;
#endif
#undef MAX_DATES

   /* Also scan memories for temporal fields in the content/key. */
   /* (We rely on memory_relations typed edges — further extraction
    * is left for the typed-knowledge-graph integration.) */

   if (shape == MEM_SHAPE_QUANTITATIVE)
   {
      /* Count distinct events (deduplicate by date string). */
      /* Simple O(n^2) dedup — small sets only. */
      int unique = 0;
      for (int i = 0; i < date_count; i++)
      {
         int dup = 0;
         for (int j = 0; j < i; j++)
            if (strcmp(dates[i], dates[j]) == 0)
            {
               dup = 1;
               break;
            }
         if (!dup)
            unique++;
      }
      /* Always add a raw count derived fact. */
      if (out->count < MEMORY_DERIVED_MAX_FACTS)
      {
         snprintf(out->facts[out->count].label, sizeof(out->facts[out->count].label),
                  "Event count (from retrieved evidence)");
         snprintf(out->facts[out->count].value, sizeof(out->facts[out->count].value), "%d",
                  unique > 0 ? unique : cand_count);
         out->count++;
      }
   }
   else if (shape == MEM_SHAPE_TEMPORAL_INTERVAL && date_count >= 1)
   {
      /* Sort dates (already ordered by SQL but let's be safe). */
      for (int i = 0; i < date_count - 1; i++)
         for (int j = i + 1; j < date_count; j++)
            if (strcmp(dates[i], dates[j]) > 0)
            {
               char tmp[32];
               memcpy(tmp, dates[i], sizeof(tmp));
               memcpy(dates[i], dates[j], sizeof(tmp));
               memcpy(dates[j], tmp, sizeof(tmp));
            }

      /* Report the most-recent date */
      const char *latest = dates[date_count - 1];
      if (out->count < MEMORY_DERIVED_MAX_FACTS)
      {
         snprintf(out->facts[out->count].label, sizeof(out->facts[out->count].label),
                  "Most recent event date");
         snprintf(out->facts[out->count].value, sizeof(out->facts[out->count].value), "%s", latest);
         out->count++;
      }

      /* Compute days since most recent event (from today). */
      time_t now = time(NULL);
      struct tm *t = gmtime(&now);
      char today[32];
      snprintf(today, sizeof(today), "%04d-%02d-%02d", t->tm_year + 1900, t->tm_mon + 1,
               t->tm_mday);
      int delta = days_between(latest, today);
      if (delta != (int)(-2147483647 - 1) && out->count < MEMORY_DERIVED_MAX_FACTS)
      {
         snprintf(out->facts[out->count].label, sizeof(out->facts[out->count].label),
                  "Days since most recent event");
         snprintf(out->facts[out->count].value, sizeof(out->facts[out->count].value), "%d days",
                  delta < 0 ? -delta : delta);
         out->count++;
      }

      /* Compute min/max/avg gaps between consecutive events. */
      if (date_count >= 2)
      {
         int min_gap = INT_MAX, max_gap = -INT_MAX, sum_gap = 0, gap_count = 0;
         for (int i = 0; i < date_count - 1; i++)
         {
            int gap = days_between(dates[i], dates[i + 1]);
            if (gap == (int)(-2147483647 - 1))
               continue;
            if (gap < 0)
               gap = -gap;
            if (gap < min_gap)
               min_gap = gap;
            if (gap > max_gap)
               max_gap = gap;
            sum_gap += gap;
            gap_count++;
         }
         if (gap_count > 0 && out->count + 2 < MEMORY_DERIVED_MAX_FACTS)
         {
            snprintf(out->facts[out->count].label, sizeof(out->facts[out->count].label),
                     "Gap between events (min/max)");
            snprintf(out->facts[out->count].value, sizeof(out->facts[out->count].value),
                     "%d / %d days", min_gap, max_gap);
            out->count++;
            snprintf(out->facts[out->count].label, sizeof(out->facts[out->count].label),
                     "Average gap between events");
            snprintf(out->facts[out->count].value, sizeof(out->facts[out->count].value), "%d days",
                     sum_gap / gap_count);
            out->count++;
         }
      }
   }

   return out->count;
}

/*
 * memory_derived_facts_to_json: format derived facts as a JSON object string.
 * Caller must free the returned string.
 */
char *memory_derived_facts_to_json(const memory_derived_facts_t *facts)
{
   if (!facts || facts->count == 0)
      return NULL;

   cJSON *obj = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(obj, "derived_facts");
   for (int i = 0; i < facts->count; i++)
   {
      cJSON *f = cJSON_CreateObject();
      cJSON_AddStringToObject(f, "label", facts->facts[i].label);
      cJSON_AddStringToObject(f, "value", facts->facts[i].value);
      cJSON_AddItemToArray(arr, f);
   }
   char *s = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return s;
}

task_intent_t classify_intent(const char *task_hint)
{
   if (!task_hint || !task_hint[0])
      return INTENT_GENERAL;

   int scores[4];
   scores[0] = count_keyword_matches(task_hint, debug_keywords);
   scores[1] = count_keyword_matches(task_hint, plan_keywords);
   scores[2] = count_keyword_matches(task_hint, review_keywords);
   scores[3] = count_keyword_matches(task_hint, deploy_keywords);

   int best = 0;
   int best_score = scores[0];
   for (int i = 1; i < 4; i++)
   {
      if (scores[i] > best_score)
      {
         best = i;
         best_score = scores[i];
      }
   }

   if (best_score == 0)
   {
      /* Temporal queries with no other signal default to REVIEW — moderate
       * recency weight and structural bias toward recently edited memories. */
      if (has_temporal_markers(task_hint))
         return INTENT_REVIEW;
      return INTENT_GENERAL;
   }

   return (task_intent_t)best;
}

/*                                   fact  pref  dec   epi   task  scratch proc  policy = 1.0
 * Every plan reserves >=0.12 for KIND_PREFERENCE (idx 1) and the pref+policy
 * pair reserves >=0.15 — preferences are treated as global constraints that
 * must survive tight-context assembly (LongMemEval SP). */
static const double plan_debug[] = {0.14, 0.12, 0.10, 0.23, 0.05, 0.00, 0.26, 0.10};
static const double plan_plan[] = {0.23, 0.12, 0.23, 0.05, 0.09, 0.00, 0.05, 0.23};
static const double plan_review[] = {0.13, 0.15, 0.18, 0.09, 0.00, 0.00, 0.10, 0.35};
static const double plan_deploy[] = {0.13, 0.12, 0.10, 0.14, 0.05, 0.00, 0.27, 0.19};
static const double plan_general[] = {0.18, 0.12, 0.14, 0.14, 0.10, 0.00, 0.15, 0.17};

void retrieval_plan_for_intent(task_intent_t intent, retrieval_plan_t *plan)
{
   plan->intent = intent;

   const double *budgets;
   switch (intent)
   {
   case INTENT_DEBUG:
      budgets = plan_debug;
      plan->include_l3 = 1;
      plan->recency_weight = 0.7;
      break;
   case INTENT_PLAN:
      budgets = plan_plan;
      plan->include_l3 = 0;
      plan->recency_weight = 0.2;
      break;
   case INTENT_REVIEW:
      budgets = plan_review;
      plan->include_l3 = 0;
      plan->recency_weight = 0.3;
      break;
   case INTENT_DEPLOY:
      budgets = plan_deploy;
      plan->include_l3 = 1;
      plan->recency_weight = 0.5;
      break;
   default:
      budgets = plan_general;
      plan->include_l3 = 0;
      plan->recency_weight = 0.3;
      break;
   }

   for (int i = 0; i < NUM_KINDS; i++)
      plan->kind_budget[i] = budgets[i];
}

/* MAX_CANDIDATES and context_candidate_t live in memory_context_internal.h
 * so memory_assemble.c can see them. */

/* Stop-words excluded from coverage computation (short/common English words). */
static const char *coverage_stopwords[] = {
    "a",     "an",   "the", "is",   "in",    "on",    "at",     "to",   "for",   "of",   "and",
    "or",    "but",  "it",  "its",  "was",   "are",   "be",     "been", "has",   "have", "had",
    "do",    "does", "did", "will", "would", "could", "should", "may",  "might", "me",   "my",
    "we",    "us",   "our", "you",  "your",  "he",    "she",    "they", "them",  "what", "when",
    "where", "who",  "how", "why",  "which", "that",  "this",   NULL};

int is_coverage_stopword(const char *word)
{
   for (int i = 0; coverage_stopwords[i]; i++)
      if (strcasecmp(word, coverage_stopwords[i]) == 0)
         return 1;
   return 0;
}

/* --- Topic-pivot detection ---
 * Bounded, predictable, DB-free so callers (hook paths, tests) can
 * run it in front of memory_recall without worrying about cost. The
 * 64-token cap per side keeps worst-case work constant even if a
 * turn's text is pathologically long — topic pivots are decided by
 * the first few dozen significant tokens anyway, not by an essay. */
#define PIVOT_MAX_TOKENS_PER_SIDE 64
#define PIVOT_MIN_TOKEN_LEN       3

typedef struct
{
   char words[PIVOT_MAX_TOKENS_PER_SIDE][32];
   int count;
} pivot_tokens_t;

static void pivot_extract_tokens(const char *text, pivot_tokens_t *out)
{
   out->count = 0;
   if (!text || !text[0])
      return;
   const char *p = text;
   while (*p && out->count < PIVOT_MAX_TOKENS_PER_SIDE)
   {
      while (*p && !isalpha((unsigned char)*p))
         p++;
      if (!*p)
         break;
      const char *start = p;
      while (*p && (isalpha((unsigned char)*p) || *p == '_'))
         p++;
      size_t len = (size_t)(p - start);
      if (len < PIVOT_MIN_TOKEN_LEN || len >= sizeof(out->words[0]))
         continue;
      char tok[32];
      for (size_t i = 0; i < len; i++)
         tok[i] = (char)tolower((unsigned char)start[i]);
      tok[len] = '\0';
      if (is_coverage_stopword(tok))
         continue;
      /* De-dup against earlier tokens — Jaccard is set-valued. */
      int dup = 0;
      for (int i = 0; i < out->count; i++)
         if (strcmp(out->words[i], tok) == 0)
         {
            dup = 1;
            break;
         }
      if (dup)
         continue;
      snprintf(out->words[out->count], sizeof(out->words[0]), "%s", tok);
      out->count++;
   }
}

static int pivot_contains(const pivot_tokens_t *set, const char *word)
{
   for (int i = 0; i < set->count; i++)
      if (strcmp(set->words[i], word) == 0)
         return 1;
   return 0;
}

int memory_recall_topic_pivot(const char *prev_text, const char *cur_text, double threshold)
{
   if (!prev_text || !prev_text[0])
      return 0; /* First turn in a session — no pivot. */
   if (!cur_text || !cur_text[0])
      return 0; /* Nothing to pivot to. */

   if (threshold <= 0.0)
      threshold = MEMORY_RECALL_PIVOT_DEFAULT_THRESHOLD;
   if (threshold > 1.0)
      threshold = 1.0;

   pivot_tokens_t a = {{{0}}, 0};
   pivot_tokens_t b = {{{0}}, 0};
   pivot_extract_tokens(prev_text, &a);
   pivot_extract_tokens(cur_text, &b);

   /* If either side has no significant tokens after filtering, treat
    * as "no pivot" — a vague "ok thanks" turn shouldn't drop the
    * prior-turn context; session runners decide separately whether
    * to retain recall when the current turn carries no signal. */
   if (a.count == 0 || b.count == 0)
      return 0;

   int intersect = 0;
   for (int i = 0; i < a.count; i++)
      if (pivot_contains(&b, a.words[i]))
         intersect++;

   int u = a.count + b.count - intersect;
   if (u <= 0)
      return 0;
   double jaccard = (double)intersect / (double)u;
   return jaccard < threshold ? 1 : 0;
}

void memory_retrieval_confidence(const char **query_terms, int term_count,
                                 const void *candidates_opaque, int cand_count, double threshold,
                                 retrieval_confidence_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));

   if (!query_terms || term_count <= 0 || !candidates_opaque || cand_count <= 0)
   {
      out->below_threshold = (threshold > 0.0) ? 1 : 0;
      return;
   }

   const context_candidate_t *candidates = (const context_candidate_t *)candidates_opaque;

   /* Coverage: fraction of non-trivial query tokens found in top-K hits */
   int n_scan = cand_count < 10 ? cand_count : 10;
   int significant = 0;
   int covered = 0;

   for (int t = 0; t < term_count; t++)
   {
      const char *term = query_terms[t];
      if (!term || strlen(term) < 3 || is_coverage_stopword(term))
         continue;
      significant++;

      char lower_term[128];
      snprintf(lower_term, sizeof(lower_term), "%s", term);
      for (char *p = lower_term; *p; p++)
         *p = (char)tolower((unsigned char)*p);

      int found = 0;
      for (int i = 0; i < n_scan && !found; i++)
      {
         char lower_k[512];
         snprintf(lower_k, sizeof(lower_k), "%s", candidates[i].key);
         for (char *p = lower_k; *p; p++)
            *p = (char)tolower((unsigned char)*p);
         if (strstr(lower_k, lower_term))
         {
            found = 1;
            break;
         }
         char lower_v[2048];
         snprintf(lower_v, sizeof(lower_v), "%s", candidates[i].content);
         for (char *p = lower_v; *p; p++)
            *p = (char)tolower((unsigned char)*p);
         if (strstr(lower_v, lower_term))
            found = 1;
      }
      if (found)
         covered++;
   }

   out->coverage = (significant > 0) ? (double)covered / (double)significant : 0.0;

   /* Separation: normalised gap between top-1 and third-place scores */
   if (cand_count >= 2)
   {
      double top1 = candidates[0].score;
      int ref_idx = cand_count >= 3 ? 2 : 1;
      double ref = candidates[ref_idx].score;
      double gap = top1 - ref;
      out->separation = (top1 > 1e-9) ? (gap / top1 < 1.0 ? gap / top1 : 1.0) : 0.0;
   }
   else
   {
      out->separation = 0.5; /* single candidate — moderate default */
   }

   /* Blended confidence score */
   out->score = 0.6 * out->coverage + 0.4 * out->separation;

   double eff_threshold = (threshold > 0.0) ? threshold : 0.35;
   out->below_threshold = (out->score < eff_threshold) ? 1 : 0;
}

/* --- Proactive Recall ---
 *
 * Builds a six-section "what's relevant right now" bundle that callers
 * inject into the agent prompt before response generation. Pure SQL
 * against the memories / prospective tables — no LLM calls on the hot
 * path. Section-independent budgets and a final trim pass keep one
 * noisy category from crowding the rest out. See
 * docs/proposals/done/proactive-recall-core.md for design notes. */
#include <pthread.h>

static pthread_mutex_t s_recall_mu = PTHREAD_MUTEX_INITIALIZER;
static int64_t s_recall_assemblies_total = 0;
static int64_t s_recall_session_start_assemblies = 0;
static double s_recall_ms_sum = 0.0;
static double s_recall_ms_max = 0.0;

static void recall_metrics_record(double ms, int session_start)
{
   pthread_mutex_lock(&s_recall_mu);
   s_recall_assemblies_total++;
   if (session_start)
      s_recall_session_start_assemblies++;
   s_recall_ms_sum += ms;
   if (ms > s_recall_ms_max)
      s_recall_ms_max = ms;
   pthread_mutex_unlock(&s_recall_mu);
}

void memory_recall_metrics(int64_t *assemblies_total, int64_t *session_start_assemblies,
                           double *ms_avg, double *ms_max)
{
   pthread_mutex_lock(&s_recall_mu);
   if (assemblies_total)
      *assemblies_total = s_recall_assemblies_total;
   if (session_start_assemblies)
      *session_start_assemblies = s_recall_session_start_assemblies;
   if (ms_avg)
      *ms_avg =
          s_recall_assemblies_total > 0 ? s_recall_ms_sum / (double)s_recall_assemblies_total : 0.0;
   if (ms_max)
      *ms_max = s_recall_ms_max;
   pthread_mutex_unlock(&s_recall_mu);
}

#define RECALL_CHARS_PER_TOKEN 4

static int recall_approx_tokens(cJSON *obj)
{
   if (!obj)
      return 0;
   char *rendered = cJSON_PrintUnformatted(obj);
   if (!rendered)
      return 0;
   int len = (int)strlen(rendered);
   free(rendered);
   return (len + RECALL_CHARS_PER_TOKEN - 1) / RECALL_CHARS_PER_TOKEN;
}

/* Fill one recall section from a pre-fetched row array. Returns count
 * appended. Each row gets a `why` field so the explain surface can surface
 * injection reasons. */
#if !defined(AIMEE_DB2_DISABLED)
static int recall_fill_from_rows(const db2_memory_cand_row_t *src_rows, int src_count, cJSON *arr,
                                 const char *why, int max_rows)
{
   if (!src_rows || !arr)
      return 0;
   int n = 0;
   for (int i = 0; i < src_count && n < max_rows; i++)
   {
      cJSON *row = cJSON_CreateObject();
      if (!row)
         break;
      cJSON_AddNumberToObject(row, "memory_id", (double)src_rows[i].id);
      cJSON_AddStringToObject(row, "tier", src_rows[i].tier);
      cJSON_AddStringToObject(row, "kind", src_rows[i].kind);
      cJSON_AddStringToObject(row, "key", src_rows[i].key);
      cJSON_AddStringToObject(row, "text", src_rows[i].content);
      cJSON_AddStringToObject(row, "why", why ? why : "");
      cJSON_AddItemToArray(arr, row);
      n++;
   }
   return n;
}
#endif

/* Section 1: identity facts. */
static int recall_fill_identity(cJSON *arr, int max_rows)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)arr;
   (void)max_rows;
   return 0;
#else
   db2_memory_cand_row_t rows[64];
   int cap = max_rows > 64 ? 64 : max_rows;
   int n = db2_memory_list_recall_section(DB2_MEM_RECALL_IDENTITY, rows, cap);
   return recall_fill_from_rows(rows, n, arr, "identity key prefix", max_rows);
#endif
}

/* Section 2: stable preferences. */
static int recall_fill_preferences(cJSON *arr, int max_rows)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)arr;
   (void)max_rows;
   return 0;
#else
   db2_memory_cand_row_t rows[64];
   int cap = max_rows > 64 ? 64 : max_rows;
   int n = db2_memory_list_recall_section(DB2_MEM_RECALL_PREFERENCES, rows, cap);
   return recall_fill_from_rows(rows, n, arr, "stable preference", max_rows);
#endif
}

/* Section 3: active project / task context. */
static int recall_fill_active_context(cJSON *arr, int max_rows)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)arr;
   (void)max_rows;
   return 0;
#else
   db2_memory_cand_row_t rows[64];
   int cap = max_rows > 64 ? 64 : max_rows;
   int n = db2_memory_list_recall_section(DB2_MEM_RECALL_ACTIVE_CONTEXT, rows, cap);
   return recall_fill_from_rows(rows, n, arr, "recent active context", max_rows);
#endif
}

/* Section 4: open commitments. */
static int recall_fill_open_commitments(cJSON *arr, int max_rows)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)arr;
   (void)max_rows;
   return 0;
#else
   db2_memory_cand_row_t rows[64];
   int cap = max_rows > 64 ? 64 : max_rows;
   int n = db2_memory_list_recall_section(DB2_MEM_RECALL_OPEN_COMMITMENTS, rows, cap);
   return recall_fill_from_rows(rows, n, arr, "pending commitment", max_rows);
#endif
}

/* Section 5: triggered prospective memories. Delegates to the existing
 * matcher so recall sections stay consistent with the pre-turn hook in
 * agent_runtime.c. */
static int recall_fill_reminders(const char *task_hint, cJSON *arr, int max_rows)
{
   memory_prospective_t rows[MEMORY_PROSPECTIVE_MAX_MATCHES];
   int cap = max_rows < MEMORY_PROSPECTIVE_MAX_MATCHES ? max_rows : MEMORY_PROSPECTIVE_MAX_MATCHES;
   int n = memory_prospective_match(task_hint, NULL, NULL, rows, cap);
   for (int i = 0; i < n; i++)
   {
      cJSON *row = cJSON_CreateObject();
      if (!row)
         break;
      cJSON_AddNumberToObject(row, "memory_id", (double)rows[i].id);
      cJSON_AddStringToObject(row, "tier", "");
      cJSON_AddStringToObject(row, "kind", "reminder");
      cJSON_AddStringToObject(row, "key", rows[i].trigger_text);
      cJSON_AddStringToObject(row, "text", rows[i].action_text);
      cJSON_AddStringToObject(row, "why", "prospective matcher fired");
      cJSON_AddItemToArray(arr, row);
   }
   return n;
}

/* Section 6: unresolved epistemic directives. When task_hint is non-empty
 * we run the topic-matcher so only contextually relevant directives
 * surface; for session-start (empty hint) we fall back to listing the
 * top-priority open directives so the agent starts with the durable gaps
 * in view.  Each surfaced row bumps surfaced_count and last_surfaced_at
 * via memory_directive_mark_surfaced so operators can tell which
 * directives the prompt layer is actually presenting. */
static int recall_fill_directives(const char *task_hint, cJSON *arr, int max_rows)
{
   if (!arr || max_rows <= 0)
      return 0;
   /* Sweep expired rows before reading so stale directives don't surface. */
   memory_directive_sweep_expired();

   memory_directive_t rows[MEMORY_DIRECTIVE_MAX_MATCHES];
   int cap = max_rows < MEMORY_DIRECTIVE_MAX_MATCHES ? max_rows : MEMORY_DIRECTIVE_MAX_MATCHES;
   int n = 0;
   if (task_hint && task_hint[0])
      n = memory_directive_match(task_hint, NULL, NULL, rows, cap);
   if (n == 0)
      n = memory_directive_list(MEMORY_DIRECTIVE_STATE_OPEN, NULL, rows, cap);

   for (int i = 0; i < n; i++)
   {
      cJSON *row = cJSON_CreateObject();
      if (!row)
         break;
      cJSON_AddNumberToObject(row, "memory_id", (double)rows[i].id);
      cJSON_AddStringToObject(row, "tier", "");
      cJSON_AddStringToObject(row, "kind", "directive");
      cJSON_AddStringToObject(row, "key", rows[i].topic);
      cJSON_AddStringToObject(row, "text", rows[i].question);
      char why[64];
      snprintf(why, sizeof(why), "directive:%s", rows[i].cause);
      cJSON_AddStringToObject(row, "why", why);
      cJSON_AddItemToArray(arr, row);
      memory_directive_mark_surfaced(rows[i].id);
   }
   return n;
}

/* Section 7: always-on rules (directive_type='hard'). These are invariant
 * constraints that must survive context pressure — they are surfaced first
 * in every recall bundle, before identity and preferences. */
static int recall_fill_always_on_rules(cJSON *arr, int max_rows)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)arr;
   (void)max_rows;
   return 0;
#else
   rule_t rules[32];
   int cap = max_rows > 32 ? 32 : max_rows;
   int n = db2_rules_list_hard(rules, cap);
   for (int i = 0; i < n; i++)
   {
      cJSON *row = cJSON_CreateObject();
      if (!row)
         break;
      cJSON_AddNumberToObject(row, "id", (double)rules[i].id);
      cJSON_AddStringToObject(row, "polarity", rules[i].polarity);
      cJSON_AddStringToObject(row, "title", rules[i].title);
      cJSON_AddStringToObject(row, "description", rules[i].description);
      cJSON_AddNumberToObject(row, "weight", (double)rules[i].weight);
      cJSON_AddItemToArray(arr, row);
   }
   return n;
#endif
}

/* Drop items from the end of a section until the overall bundle fits
 * under the token budget. Called lowest-priority-first so identity and
 * preferences are the last sections to shrink. */
static void recall_truncate_section(cJSON *bundle, cJSON *arr, int limit_tokens)
{
   if (!bundle || !arr || limit_tokens <= 0)
      return;
   while (cJSON_GetArraySize(arr) > 0 && recall_approx_tokens(bundle) > limit_tokens)
   {
      int last = cJSON_GetArraySize(arr) - 1;
      cJSON_DeleteItemFromArray(arr, last);
   }
}

#if !defined(AIMEE_DB2_DISABLED)
/* Collect unique memory_id values from all recall sections for attribution. */
static int recall_collect_ids(const cJSON *bundle, int64_t *out, int max)
{
   static const char *sections[] = {
       "identity",   "preferences", "active_context", "open_commitments", "reminders",
       "directives", NULL};
   int n = 0;
   for (int s = 0; sections[s] && n < max; s++)
   {
      const cJSON *arr = cJSON_GetObjectItemCaseSensitive(bundle, sections[s]);
      if (!cJSON_IsArray(arr))
         continue;
      const cJSON *item;
      cJSON_ArrayForEach(item, arr)
      {
         const cJSON *mid = cJSON_GetObjectItemCaseSensitive(item, "memory_id");
         if (cJSON_IsNumber(mid) && n < max)
            out[n++] = (int64_t)mid->valuedouble;
      }
   }
   return n;
}
#endif

cJSON *memory_recall(const char *task_hint, int limit_tokens, int session_start)
{

   if (limit_tokens <= 0)
      limit_tokens = session_start ? MEMORY_RECALL_DEFAULT_LIMIT_TOKENS_SESSION
                                   : MEMORY_RECALL_DEFAULT_LIMIT_TOKENS_TURN;
   if (limit_tokens < MEMORY_RECALL_MIN_LIMIT_TOKENS)
      limit_tokens = MEMORY_RECALL_MIN_LIMIT_TOKENS;
   if (limit_tokens > MEMORY_RECALL_MAX_LIMIT_TOKENS)
      limit_tokens = MEMORY_RECALL_MAX_LIMIT_TOKENS;

   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);

   cJSON *bundle = cJSON_CreateObject();
   cJSON *always_on_rules = cJSON_CreateArray();
   cJSON *identity = cJSON_CreateArray();
   cJSON *preferences = cJSON_CreateArray();
   cJSON *active_context = cJSON_CreateArray();
   cJSON *open_commitments = cJSON_CreateArray();
   cJSON *reminders = cJSON_CreateArray();
   cJSON *directives = cJSON_CreateArray();
   if (!bundle || !always_on_rules || !identity || !preferences || !active_context ||
       !open_commitments || !reminders || !directives)
   {
      cJSON_Delete(bundle);
      cJSON_Delete(always_on_rules);
      cJSON_Delete(identity);
      cJSON_Delete(preferences);
      cJSON_Delete(active_context);
      cJSON_Delete(open_commitments);
      cJSON_Delete(reminders);
      cJSON_Delete(directives);
      return NULL;
   }

   cJSON_AddBoolToObject(bundle, "session_start", session_start ? 1 : 0);
   cJSON_AddNumberToObject(bundle, "limit_tokens", limit_tokens);
   cJSON_AddItemToObject(bundle, "always_on_rules", always_on_rules);
   cJSON_AddItemToObject(bundle, "identity", identity);
   cJSON_AddItemToObject(bundle, "preferences", preferences);
   cJSON_AddItemToObject(bundle, "active_context", active_context);
   cJSON_AddItemToObject(bundle, "open_commitments", open_commitments);
   cJSON_AddItemToObject(bundle, "reminders", reminders);
   cJSON_AddItemToObject(bundle, "directives", directives);

   /* Per-section caps — generous on session start, tight per-turn.
    * The final token-budget trim catches cases where a section hits
    * its cap but the overall bundle still exceeds the token budget. */
   int cap_rules = session_start ? 16 : 8;
   int cap_identity = session_start ? 6 : 3;
   int cap_preferences = session_start ? 8 : 4;
   int cap_active = session_start ? 10 : 5;
   int cap_commitments = session_start ? 6 : 3;
   int cap_reminders = session_start ? 5 : 3;
   int cap_directives = session_start ? 5 : 2;

   recall_fill_always_on_rules(always_on_rules, cap_rules);
   recall_fill_identity(identity, cap_identity);
   recall_fill_preferences(preferences, cap_preferences);
   recall_fill_active_context(active_context, cap_active);
   recall_fill_open_commitments(open_commitments, cap_commitments);
   recall_fill_reminders(task_hint, reminders, cap_reminders);
   recall_fill_directives(task_hint, directives, cap_directives);

   /* Trim order inverts priority — directives/reminders/commitments/rules
    * get trimmed first so identity and preferences survive under pressure.
    * always_on_rules trim last (after identity) — hard rules must not be
    * silently dropped under token pressure without explicit override. */
   recall_truncate_section(bundle, directives, limit_tokens);
   recall_truncate_section(bundle, reminders, limit_tokens);
   recall_truncate_section(bundle, open_commitments, limit_tokens);
   recall_truncate_section(bundle, active_context, limit_tokens);
   recall_truncate_section(bundle, preferences, limit_tokens);
   recall_truncate_section(bundle, identity, limit_tokens);
   recall_truncate_section(bundle, always_on_rules, limit_tokens);

   clock_gettime(CLOCK_MONOTONIC, &t1);
   double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1.0e6;
   recall_metrics_record(ms, session_start);
   cJSON_AddNumberToObject(bundle, "approx_tokens", recall_approx_tokens(bundle));
   cJSON_AddNumberToObject(bundle, "elapsed_ms", ms);

#if !defined(AIMEE_DB2_DISABLED)
   {
      int64_t ids[256];
      int n_ids = recall_collect_ids(bundle, ids, 256);
      char ev_id[64] = "";
      learning_evidence_write_retrieval_event(task_hint ? task_hint : "", "Recall", ids, n_ids,
                                              ev_id, sizeof(ev_id));
      if (ev_id[0])
         cJSON_AddStringToObject(bundle, "retrieval_event_id", ev_id);
   }
#endif

   return bundle;
}
