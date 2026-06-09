/* memory_advanced.c: anti-patterns, temporal facts, drift detection, style
 * learning, provenance surfacing, and memory-to-memory linking. Other
 * subsystems that used to live here are now in sibling files:
 *   memory_graph.c     — entity edges, graph boost, pruning, normalization
 *   memory_scan.c      — JSONL session-log scanner
 *   memory_improve.c   — improve loop, cognification, async jobs
 *   memory_episodes.c  — per-session episode cards + scene clustering
 */
#include "aimee.h"
#include "cJSON.h"
#include "config.h"
#include "db1.h"
#if !defined(AIMEE_DB2_DISABLED)
#include "db2/anti_patterns.h"
#include "db2/bandit.h"
#include "db2/decision_log.h"
#include "db2/feedback.h"
#include "db2/memory_briefing.h"
#include "db2/memory_query.h"
#include "db2/memory_relations.h"
#include "db2/rules.h"
#include "db2/tasks.h"
#endif
#include "dogfood.h"
#include "kb_reasoning.h"
#include "log.h"
#include "memory.h"
#include "memory_ontology.h"
#include "platform_process.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(AIMEE_DB2_DISABLED)
int anti_pattern_extract_from_feedback(void)
{
   return 0;
}

int anti_pattern_extract_from_failures(void)
{
   return 0;
}

int anti_pattern_escalate(int hit_threshold)
{
   (void)hit_threshold;
   return 0;
}

int memory_supersede(int64_t old_id, const char *new_content, double confidence,
                     const char *session_id, memory_t *out)
{
   (void)old_id;
   (void)new_content;
   (void)confidence;
   (void)session_id;
   (void)out;
   return -1;
}

int memory_fact_history(const char *key, memory_t *out, int max)
{
   (void)key;
   (void)out;
   (void)max;
   return 0;
}

int memory_check_drift(int64_t task_id, const char *file_path, const char *command,
                       drift_result_t *out)
{
   (void)task_id;
   (void)file_path;
   (void)command;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int memory_learn_style(void)
{
   return 0;
}

int memory_get_provenance(int64_t memory_id, provenance_entry_t *out, int max)
{
   (void)memory_id;
   (void)out;
   (void)max;
   return 0;
}

int memory_link_create(int64_t source_id, int64_t target_id, const char *relation)
{
   (void)source_id;
   (void)target_id;
   (void)relation;
   return -1;
}

int memory_link_query(int64_t memory_id, memory_link_t *out, int max)
{
   (void)memory_id;
   (void)out;
   (void)max;
   return 0;
}

int memory_link_delete(int64_t link_id)
{
   (void)link_id;
   return -1;
}

cJSON *memory_briefing(int limit_tokens)
{
   if (limit_tokens <= 0)
      limit_tokens = MEMORY_BRIEFING_DEFAULT_LIMIT_TOKENS;

   cJSON *bundle = cJSON_CreateObject();
   if (!bundle)
      return NULL;
   cJSON_AddNumberToObject(bundle, "limit_tokens", limit_tokens);
   cJSON_AddItemToObject(bundle, "key_facts", cJSON_CreateArray());
   cJSON_AddItemToObject(bundle, "recent_activity", cJSON_CreateArray());
   cJSON_AddItemToObject(bundle, "active_entities", cJSON_CreateArray());
   cJSON_AddNumberToObject(bundle, "approx_tokens", 0);
   return bundle;
}

#else

static const char *memory_briefing_style(void)
{
   static __thread char promoted[64];
   promoted[0] = '\0';
   if (db2_bandit_promotion_get("briefing_style", promoted, sizeof(promoted)) == 0)
   {
      if (strcmp(promoted, "compact") == 0 || strcmp(promoted, "evidence_heavy") == 0)
         return promoted;
   }
   return "compact";
}

static int memory_briefing_apply_style_limit(const char *style, int limit_tokens)
{
   if (limit_tokens <= 0)
      limit_tokens = MEMORY_BRIEFING_DEFAULT_LIMIT_TOKENS;
   if (style && strcmp(style, "compact") == 0 && limit_tokens > 1024)
      limit_tokens = 1024;
   if (style && strcmp(style, "evidence_heavy") == 0 && limit_tokens < 3000)
      limit_tokens = 3000;
   return limit_tokens;
}

/* --- Anti-Patterns: high-level extraction/escalation. Storage primitives
 * live in db1/anti_patterns.{h,c}. --- */

int anti_pattern_extract_from_feedback(void)
{
   rule_t rules[128];
   int rcount = db2_rules_list_by_tier(50, rules, 128);
   int extracted = 0;
   for (int i = 0; i < rcount; i++)
   {
      if (strcmp(rules[i].polarity, "negative") != 0)
         continue;
      if (!rules[i].title[0])
         continue;

      char ref[64];
      snprintf(ref, sizeof(ref), "rule:%d", rules[i].id);
      if (db2_anti_pattern_exists_by_source_ref(ref))
         continue;

      db2_anti_pattern_insert(rules[i].title, rules[i].description, "feedback", ref, 0.8, NULL);
      extracted++;

      /* Shadow duplicate check via graph reasoning layer (Phase 1 — log only). */
      config_t rcfg;
      config_load(&rcfg);
      if (rcfg.reasoning_datalog_command[0])
      {
         char title64[65];
         snprintf(title64, sizeof(title64), "%.64s", rules[i].title);
         kb_reasoning_result_t res;
         if (kb_reasoning_query(&rcfg, "superseded_by(?a, ?b)", NULL, "anti_pattern", title64,
                                &res) == 0 &&
             res.n_rows > 0)
            LOG_DEBUG("reasoning.learning", "anti_pattern='%s' superseded by case — flagged",
                      title64);
         kb_reasoning_result_free(&res);
      }
   }
   return extracted;
}

int anti_pattern_extract_from_failures(void)
{
   db2_decision_log_row_t decisions[256];
   int decision_count = db2_decision_log_list("failure", 0, decisions, 256);
   int extracted = 0;
   for (int i = 0; i < decision_count; i++)
   {
      int64_t dec_id = decisions[i].id;
      const char *chosen = decisions[i].chosen;
      const char *rationale = decisions[i].rationale;

      if (!chosen)
         continue;

      char ref[64];
      snprintf(ref, sizeof(ref), "decision:%lld", (long long)dec_id);
      if (db2_anti_pattern_exists_by_source_ref(ref))
         continue;

      db2_anti_pattern_insert(chosen, rationale ? rationale : "", "failure", ref, 0.7, NULL);
      extracted++;

      /* Shadow duplicate check via graph reasoning layer (Phase 1 — log only). */
      config_t rcfg;
      config_load(&rcfg);
      if (rcfg.reasoning_datalog_command[0])
      {
         char title64[65];
         snprintf(title64, sizeof(title64), "%.64s", chosen);
         kb_reasoning_result_t res;
         if (kb_reasoning_query(&rcfg, "superseded_by(?a, ?b)", NULL, "anti_pattern", title64,
                                &res) == 0 &&
             res.n_rows > 0)
            LOG_DEBUG("reasoning.learning", "anti_pattern='%s' superseded by case — flagged",
                      title64);
         kb_reasoning_result_free(&res);
      }
   }
   return extracted;
}

int anti_pattern_escalate(int hit_threshold)
{
   if (hit_threshold <= 0)
      return 0;

   anti_pattern_t hot[128];
   int hot_count = db2_anti_pattern_list_hot(hit_threshold, hot, 128);
   int escalated = 0;
   for (int i = 0; i < hot_count; i++)
   {
      const char *pattern = hot[i].pattern;
      if (!pattern[0])
         continue;

      rule_t existing;
      if (db2_rules_find_by_title(pattern, &existing) == 0)
      {
         /* Already a rule; bump to hard directive if not already */
         if (strcmp(existing.directive_type, "hard") != 0)
         {
            if (db2_rules_reinforce_directive(existing.id, "hard", 90) == 0)
               escalated++;
         }
         continue;
      }

      /* Create a new hard directive rule from the anti-pattern */
      int dummy = 0;
      db2_feedback_record("negative", pattern, hot[i].description, 90, &dummy);
      rule_t new_rule;
      if (db2_rules_find_by_title(pattern, &new_rule) == 0)
         db2_rules_reinforce_directive(new_rule.id, "hard", -1);
      escalated++;
   }
   return escalated;
}

/* --- Temporal Facts --- */

int memory_supersede(int64_t old_id, const char *new_content, double confidence,
                     const char *session_id, memory_t *out)
{
   if (old_id <= 0)
      return -1;

   memory_t old_mem;
   if (memory_get(old_id, &old_mem) != 0)
      return -1;

   int version = db2_memory_count_versions(old_mem.key) + 1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   char versioned_key[560];
   snprintf(versioned_key, sizeof(versioned_key), "%s#v%d", old_mem.key, version);
   if (db2_memory_set_versioned_key(old_id, versioned_key, ts) != 0)
      return -1;

   int rc = memory_insert(old_mem.tier, old_mem.kind, old_mem.key, new_content, confidence,
                          session_id, out);
   if (rc != 0)
      return -1;

   if (out)
   {
      db2_memory_set_valid_from(out->id, ts);

      char details[128];
      snprintf(details, sizeof(details), "supersedes memory %lld", (long long)old_id);
      add_provenance(out->id, session_id, "supersede", details);

      memory_link_create(out->id, old_id, "supersedes");
   }

   return 0;
}

int memory_fact_history(const char *key, memory_t *out, int max)
{
   if (!key)
      return 0;
   char norm[512];
   normalize_key(key, norm, sizeof(norm));
   return db2_memory_fact_history(norm, out, max);
}

/* --- Drift Detection --- */

int memory_check_drift(int64_t task_id, const char *file_path, const char *command,
                       drift_result_t *out)
{
   if (!out)
      return -1;

   memset(out, 0, sizeof(*out));

   /* Get active task */
   aimee_task_t task;
   if (db2_task_get(task_id, &task) != 0)
      return -1;

   out->task_id = task_id;
   snprintf(out->task_title, sizeof(out->task_title), "%s", task.title);

   /* Build scope from title words */
   char *scope_terms[64];
   int scope_count = tokenize_for_search(task.title, scope_terms, 64);

   /* Add subtask title terms */
   {
      aimee_task_t subs[32];
      int sub_count = db2_task_get_subtasks(task_id, subs, 32);
      for (int i = 0; i < sub_count; i++)
      {
         char *sub_terms[16];
         int sc = tokenize_for_search(subs[i].title, sub_terms, 16);
         for (int j = 0; j < sc && scope_count < 64; j++)
            scope_terms[scope_count++] = sub_terms[j];
      }
   }

   /* Check if file_path overlaps scope */
   int file_in_scope = 0;
   if (file_path && file_path[0])
   {
      /* Extract basename and directory components */
      char fp_lower[MAX_PATH_LEN];
      snprintf(fp_lower, sizeof(fp_lower), "%s", file_path);
      for (int i = 0; fp_lower[i]; i++)
         fp_lower[i] = tolower((unsigned char)fp_lower[i]);

      for (int s = 0; s < scope_count; s++)
      {
         if (scope_terms[s] && strstr(fp_lower, scope_terms[s]))
         {
            file_in_scope = 1;
            break;
         }
      }
   }

   /* Check if command overlaps scope */
   int cmd_in_scope = 0;
   if (command && command[0])
   {
      char cmd_lower[1024];
      snprintf(cmd_lower, sizeof(cmd_lower), "%s", command);
      for (int i = 0; cmd_lower[i]; i++)
         cmd_lower[i] = tolower((unsigned char)cmd_lower[i]);

      for (int s = 0; s < scope_count; s++)
      {
         if (scope_terms[s] && strstr(cmd_lower, scope_terms[s]))
         {
            cmd_in_scope = 1;
            break;
         }
      }
   }

   /* Determine drift */
   int has_input = (file_path && file_path[0]) || (command && command[0]);
   if (has_input && !file_in_scope && !cmd_in_scope)
   {
      out->drifted = 1;
      snprintf(out->message, sizeof(out->message), "Action does not appear related to task: %s",
               task.title);
   }
   else
   {
      out->drifted = 0;
   }

   /* Clean up */
   for (int i = 0; i < scope_count; i++)
      free(scope_terms[i]);

   return 0;
}

/* --- Style Learning --- */

#define STYLE_MAX_KEYWORDS    16
#define STYLE_MATCH_THRESHOLD 2

typedef struct
{
   const char *dimension;                        /* memory key: style_{dimension} */
   const char *neg_keywords[STYLE_MAX_KEYWORDS]; /* triggers from negative rules */
   const char *pos_keywords[STYLE_MAX_KEYWORDS]; /* triggers from positive rules */
   const char *neg_preference;                   /* generated when neg matches >= threshold */
   const char *pos_preference;                   /* generated when pos matches >= threshold */
} style_dimension_t;

static const style_dimension_t style_dimensions[] = {
    {"verbosity",
     {"verbose", "wordy", "lengthy", "chatty", "too long", "too much", "wall of text",
      "overwhelming", NULL},
     {"concise", "brief", "terse", "short", "minimal", "succinct", "compact", NULL},
     "User prefers concise, non-verbose output",
     "User explicitly prefers brief responses"},
    {"explanations",
     {"obvious", "unnecessary", "over-explain", "don't explain", "stop explaining", "already know",
      NULL},
     {"explain", "why", "reasoning", "context", "more detail", "elaborate", NULL},
     "User prefers minimal explanations; avoid stating the obvious",
     "User values detailed explanations with reasoning"},
    {"commit_style",
     {"vague commit", "bad message", "commit message", "unclear commit", NULL},
     {"conventional", "semantic commit", "good commit", "commit convention", NULL},
     "User wants clear, descriptive commit messages",
     "User follows conventional commit style"},
    {"naming",
     {"inconsistent", "wrong case", "naming", "rename", NULL},
     {"snake_case", "camelcase", "consistent naming", "naming convention", NULL},
     "User wants consistent naming conventions",
     "User prefers specific naming conventions"},
    {"comments",
     {"too many comments", "obvious comments", "unnecessary comment", "remove comment", NULL},
     {"document", "comment", "annotate", "docstring", "jsdoc", NULL},
     "User prefers minimal code comments",
     "User values thorough code documentation"},
    {"structure",
     {"wall of text", "no headers", "unstructured", "hard to read", "formatting", NULL},
     {"headers", "sections", "bullet", "structured", "organized", NULL},
     "User prefers structured output with sections and headers",
     "User values well-organized, structured responses"},
};

#define STYLE_DIMENSION_COUNT           (sizeof(style_dimensions) / sizeof(style_dimensions[0]))

static int match_keywords(const char *text, const char *const keywords[])
{
   for (int k = 0; keywords[k]; k++)
      if (strstr(text, keywords[k]))
         return 1;
   return 0;
}

int memory_learn_style(void)
{
   int neg_counts[STYLE_DIMENSION_COUNT];
   int pos_counts[STYLE_DIMENSION_COUNT];
   memset(neg_counts, 0, sizeof(neg_counts));
   memset(pos_counts, 0, sizeof(pos_counts));

   /* Scan both positive and negative feedback rules. Read through the
    * typed DB1 surface so this function does not own SQL against the
    * rules table. */
   {
      rule_t rules[256];
      int rcount = db2_rules_list(rules, (int)(sizeof(rules) / sizeof(rules[0])));
      for (int i = 0; i < rcount; i++)
      {
         const char *polarity = rules[i].polarity;
         const char *desc = rules[i].description;
         if (!polarity[0] || !desc[0])
            continue;

         int is_neg = (strcmp(polarity, "negative") == 0);
         int is_pos = (strcmp(polarity, "positive") == 0);
         if (!is_neg && !is_pos)
            continue;

         char lower[1024];
         snprintf(lower, sizeof(lower), "%s", desc);
         for (int j = 0; lower[j]; j++)
            lower[j] = (char)tolower((unsigned char)lower[j]);

         for (size_t d = 0; d < STYLE_DIMENSION_COUNT; d++)
         {
            if (is_neg && match_keywords(lower, style_dimensions[d].neg_keywords))
               neg_counts[d]++;
            else if (is_pos && match_keywords(lower, style_dimensions[d].pos_keywords))
               pos_counts[d]++;
         }
      }
   }

   /* Scan DB1 decision_log for style-relevant successful decisions. */
   {
      db2_decision_log_row_t decisions[256];
      int decision_count = db2_decision_log_list("success", 0, decisions, 256);
      for (int i = 0; i < decision_count; i++)
      {
         const char *chosen = decisions[i].chosen;
         const char *rationale = decisions[i].rationale;

         char lower[2048];
         snprintf(lower, sizeof(lower), "%s %s", chosen ? chosen : "", rationale ? rationale : "");
         for (int j = 0; lower[j]; j++)
            lower[j] = tolower((unsigned char)lower[j]);

         if (!strstr(lower, "style") && !strstr(lower, "format") && !strstr(lower, "naming") &&
             !strstr(lower, "convention") && !strstr(lower, "commit") && !strstr(lower, "comment"))
            continue;

         for (size_t d = 0; d < STYLE_DIMENSION_COUNT; d++)
         {
            if (match_keywords(lower, style_dimensions[d].pos_keywords))
               pos_counts[d]++;
            else if (match_keywords(lower, style_dimensions[d].neg_keywords))
               neg_counts[d]++;
         }
      }
   }

   /* Generate preference memories for dimensions with sufficient signal */
   int generated = 0;
   for (size_t d = 0; d < STYLE_DIMENSION_COUNT; d++)
   {
      char key[128];
      snprintf(key, sizeof(key), "style_%s", style_dimensions[d].dimension);

      if (neg_counts[d] >= STYLE_MATCH_THRESHOLD && style_dimensions[d].neg_preference)
      {
         char content[512];
         snprintf(content, sizeof(content), "%s. Evidence: %d feedback signal(s).",
                  style_dimensions[d].neg_preference, neg_counts[d]);
         memory_insert(TIER_L1, KIND_PREFERENCE, key, content, 0.8, "style_learning", NULL);
         generated++;
      }
      else if (pos_counts[d] >= STYLE_MATCH_THRESHOLD && style_dimensions[d].pos_preference)
      {
         char content[512];
         snprintf(content, sizeof(content), "%s. Evidence: %d feedback signal(s).",
                  style_dimensions[d].pos_preference, pos_counts[d]);
         memory_insert(TIER_L1, KIND_PREFERENCE, key, content, 0.9, "style_learning", NULL);
         generated++;
      }
   }

   return generated;
}

/* --- Provenance Surfacing --- */

int memory_get_provenance(int64_t memory_id, provenance_entry_t *out, int max)
{
   return db2_memory_provenance_list(memory_id, out, max);
}

/* --- Memory-to-Memory Linking --- */

int memory_link_create(int64_t source_id, int64_t target_id, const char *relation)
{
   return db2_memory_link_create(source_id, target_id, relation);
}

int memory_link_query(int64_t memory_id, memory_link_t *out, int max)
{
   return db2_memory_link_query(memory_id, out, max);
}

int memory_link_delete(int64_t link_id)
{
   return db2_memory_link_delete(link_id);
}

/* --- Session Briefing ---
 *
 * See memory.h for contract.  Ranking uses only DB columns that exist today
 * (no lifecycle_state yet), so the "open commitments" section from the
 * proposal is intentionally omitted until that schema lands.
 */

/* Approximate byte -> token ratio used for the rendered payload budget.
 * 4 chars ~= 1 token is close enough for the GPT-family tokenizers we care
 * about.  Budget is tracked on the serialised JSON string length so the
 * wire-size is what gets capped. */
#define MEMORY_BRIEFING_CHARS_PER_TOKEN 4

static double memory_briefing_salience_proxy(double confidence, double evidence_strength,
                                             int observation_count)
{
   double obs = observation_count > 0 ? (double)observation_count : 0.0;
   /* log1p keeps the tail from exploding once a fact has been observed many
    * times; the 0.2 multiplier keeps observation-driven lift bounded relative
    * to the base confidence/evidence signal. */
   return confidence + evidence_strength + 0.2 * log(1.0 + obs);
}

static int memory_briefing_approx_tokens(const cJSON *obj)
{
   if (!obj)
      return 0;
   char *rendered = cJSON_PrintUnformatted(obj);
   if (!rendered)
      return 0;
   int len = (int)strlen(rendered);
   free(rendered);
   return (len + MEMORY_BRIEFING_CHARS_PER_TOKEN - 1) / MEMORY_BRIEFING_CHARS_PER_TOKEN;
}

static void memory_briefing_fill_key_facts(cJSON *arr, int max_rows)
{
   db2_memory_briefing_fact_t rows[64];
   if (max_rows > (int)(sizeof(rows) / sizeof(rows[0])))
      max_rows = (int)(sizeof(rows) / sizeof(rows[0]));
   int n = db2_memory_briefing_list_key_facts(rows, max_rows);
   for (int i = 0; i < n; i++)
   {
      cJSON *row = cJSON_CreateObject();
      if (!row)
         break;
      cJSON_AddNumberToObject(row, "memory_id", (double)rows[i].memory_id);
      cJSON_AddStringToObject(row, "tier", rows[i].tier);
      cJSON_AddStringToObject(row, "kind", rows[i].kind);
      cJSON_AddStringToObject(row, "key", rows[i].key);
      cJSON_AddStringToObject(row, "text", rows[i].text);
      cJSON_AddNumberToObject(row, "confidence", rows[i].confidence);
      cJSON_AddNumberToObject(row, "evidence_strength", rows[i].evidence_strength);
      cJSON_AddNumberToObject(row, "observation_count", rows[i].observation_count);
      cJSON_AddNumberToObject(row, "salience",
                              memory_briefing_salience_proxy(rows[i].confidence,
                                                             rows[i].evidence_strength,
                                                             rows[i].observation_count));
      cJSON_AddStringToObject(row, "last_seen_at", rows[i].last_seen_at);
      cJSON_AddItemToArray(arr, row);
   }
}

static void memory_briefing_fill_recent_activity(cJSON *arr, int max_rows)
{
   db2_memory_briefing_activity_t rows[16];
   if (max_rows > (int)(sizeof(rows) / sizeof(rows[0])))
      max_rows = (int)(sizeof(rows) / sizeof(rows[0]));
   int n = db2_memory_briefing_list_recent_activity(rows, max_rows);
   for (int i = 0; i < n; i++)
   {
      cJSON *row = cJSON_CreateObject();
      if (!row)
         break;
      cJSON_AddStringToObject(row, "session_id", rows[i].session_id);
      cJSON_AddStringToObject(row, "summary", rows[i].summary);
      cJSON_AddStringToObject(row, "reference_time", rows[i].reference_time);
      cJSON_AddStringToObject(row, "created_at", rows[i].created_at);
      cJSON_AddItemToArray(arr, row);
   }
}

static void memory_briefing_fill_active_entities(cJSON *arr, int max_rows)
{
   db2_memory_briefing_entity_t rows[64];
   if (max_rows > (int)(sizeof(rows) / sizeof(rows[0])))
      max_rows = (int)(sizeof(rows) / sizeof(rows[0]));
   int n = db2_memory_briefing_list_active_entities(rows, max_rows);
   for (int i = 0; i < n; i++)
   {
      cJSON *row = cJSON_CreateObject();
      if (!row)
         break;
      cJSON_AddStringToObject(row, "name", rows[i].name);
      cJSON_AddNumberToObject(row, "mentions", rows[i].mentions);
      cJSON_AddStringToObject(row, "last_seen", rows[i].last_seen);
      cJSON_AddItemToArray(arr, row);
   }
}

/* Drop items from the end of arr until the full bundle fits under
 * limit_tokens.  Called per section, ordered from lowest priority up, so
 * key_facts is the last thing to shrink. */
static void memory_briefing_truncate_to_budget(cJSON *bundle, cJSON *arr, int limit_tokens)
{
   if (!bundle || !arr || limit_tokens <= 0)
      return;

   while (cJSON_GetArraySize(arr) > 0 && memory_briefing_approx_tokens(bundle) > limit_tokens)
   {
      int last = cJSON_GetArraySize(arr) - 1;
      cJSON_DeleteItemFromArray(arr, last);
   }
}

cJSON *memory_briefing(int limit_tokens)
{
   const char *style = memory_briefing_style();
   limit_tokens = memory_briefing_apply_style_limit(style, limit_tokens);
   if (limit_tokens < MEMORY_BRIEFING_MIN_LIMIT_TOKENS)
      limit_tokens = MEMORY_BRIEFING_MIN_LIMIT_TOKENS;
   if (limit_tokens > MEMORY_BRIEFING_MAX_LIMIT_TOKENS)
      limit_tokens = MEMORY_BRIEFING_MAX_LIMIT_TOKENS;

   cJSON *bundle = cJSON_CreateObject();
   cJSON *key_facts = cJSON_CreateArray();
   cJSON *recent_activity = cJSON_CreateArray();
   cJSON *active_entities = cJSON_CreateArray();
   if (!bundle || !key_facts || !recent_activity || !active_entities)
   {
      cJSON_Delete(bundle);
      cJSON_Delete(key_facts);
      cJSON_Delete(recent_activity);
      cJSON_Delete(active_entities);
      return NULL;
   }

   cJSON_AddStringToObject(bundle, "briefing_style", style);
   cJSON_AddNumberToObject(bundle, "limit_tokens", limit_tokens);
   cJSON_AddItemToObject(bundle, "key_facts", key_facts);
   cJSON_AddItemToObject(bundle, "recent_activity", recent_activity);
   cJSON_AddItemToObject(bundle, "active_entities", active_entities);

   /* Over-fetch each section relative to the budget; truncation below trims
    * to fit.  The caps double as DoS guards if the DB is huge. */
   int evidence_heavy = style && strcmp(style, "evidence_heavy") == 0;
   memory_briefing_fill_key_facts(key_facts, evidence_heavy ? 60 : 30);
   memory_briefing_fill_recent_activity(recent_activity, evidence_heavy ? 10 : 5);
   memory_briefing_fill_active_entities(active_entities, evidence_heavy ? 40 : 20);

   /* Priority order: trim lowest-priority sections first. */
   memory_briefing_truncate_to_budget(bundle, active_entities, limit_tokens);
   memory_briefing_truncate_to_budget(bundle, recent_activity, limit_tokens);
   memory_briefing_truncate_to_budget(bundle, key_facts, limit_tokens);

   cJSON_AddNumberToObject(bundle, "approx_tokens", memory_briefing_approx_tokens(bundle));
   dogfood_log_moment_live("memory_briefing", NULL, NULL, 0, NULL);
   return bundle;
}

#endif

/* --- Aggregation-Aware Query Routing ---
 *
 * See memory.h for the public contract.  Detection is a tight structural
 * heuristic: the goal is cheap, high-precision classification of "coverage"
 * queries so they can bypass similarity ranking.  Missing a query is fine —
 * callers fall back to the normal hybrid retrieval path.
 */

/* Lowercase copy with whitespace collapse.  out is always NUL-terminated
 * (within out_len).  Separates adjacent punctuation from words so our
 * token walker sees clean word boundaries. */
static void agg_normalize(const char *src, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   size_t o = 0;
   int in_space = 1;
   for (size_t i = 0; src && src[i] && o + 1 < out_len; i++)
   {
      unsigned char c = (unsigned char)src[i];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == '.' || c == '?' ||
          c == '!' || c == ';' || c == ':' || c == '(' || c == ')' || c == '\'' || c == '"')
      {
         if (!in_space)
         {
            out[o++] = ' ';
            in_space = 1;
         }
      }
      else
      {
         out[o++] = (char)tolower(c);
         in_space = 0;
      }
   }
   while (o > 0 && out[o - 1] == ' ')
      o--;
   out[o] = '\0';
}

/* 1 iff `needle` appears as a whole-word token in `haystack`.  Both inputs
 * should already be agg_normalize()-d. */
static int agg_has_word(const char *haystack, const char *needle)
{
   if (!haystack || !needle || !*needle)
      return 0;
   size_t nlen = strlen(needle);
   const char *p = haystack;
   while ((p = strstr(p, needle)) != NULL)
   {
      int left_ok = (p == haystack) || (p[-1] == ' ');
      int right_ok = (p[nlen] == '\0') || (p[nlen] == ' ');
      if (left_ok && right_ok)
         return 1;
      p++;
   }
   return 0;
}

/* 1 iff `norm` starts with `prefix` as a whole-token prefix. */
static int agg_starts_with(const char *norm, const char *prefix)
{
   if (!norm || !prefix)
      return 0;
   size_t plen = strlen(prefix);
   if (strncmp(norm, prefix, plen) != 0)
      return 0;
   return norm[plen] == '\0' || norm[plen] == ' ';
}

/* Plural-noun heuristic: any token ending in 's' that isn't on a short
 * stop list ("is", "has", "was", "its", etc.).  Crude but adequate for
 * filtering "all at once" / "every time" from real aggregation queries. */
static int agg_token_is_plural_noun(const char *tok, size_t len)
{
   if (!tok || len < 3)
      return 0;
   if (tok[len - 1] != 's')
      return 0;
   static const char *stops[] = {"is",   "has",   "was",     "its",    "his",   "its",   "us",
                                 "this", "yes",   "does",    "gas",    "plus",  "minus", "thus",
                                 "says", "let's", "there's", "what's", "who's", "it's",  NULL};
   for (int i = 0; stops[i]; i++)
   {
      size_t slen = strlen(stops[i]);
      if (slen == len && strncmp(tok, stops[i], len) == 0)
         return 0;
   }
   return 1;
}

static int agg_has_plural_noun(const char *norm)
{
   if (!norm)
      return 0;
   const char *p = norm;
   while (*p)
   {
      while (*p == ' ')
         p++;
      const char *start = p;
      while (*p && *p != ' ')
         p++;
      if (agg_token_is_plural_noun(start, (size_t)(p - start)))
         return 1;
   }
   return 0;
}

/* Extract the first proper-noun-ish seed from the *original* (case-bearing)
 * query.  Skips the sentence-initial word so "What cities has Jon visited"
 * returns "jon", not "what".  Writes a lowercased copy into out. */
static void agg_extract_entity_seed(const char *raw, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (!raw)
      return;

   int word_index = 0;
   const char *p = raw;
   while (*p)
   {
      while (*p && !isalpha((unsigned char)*p))
         p++;
      if (!*p)
         break;
      const char *start = p;
      while (*p && (isalnum((unsigned char)*p) || *p == '-' || *p == '\''))
         p++;
      size_t len = (size_t)(p - start);

      /* Skip sentence-initial word so common quantifiers ("What", "Which",
       * "Every", "All", "List", "Show", "Enumerate") don't get picked as
       * the entity seed even when capitalised. */
      if (word_index > 0 && len >= 2 && isupper((unsigned char)start[0]))
      {
         /* Also skip a short allow-list of capitalised non-entities that
          * can appear mid-sentence ("I", "Ive", "Id"). */
         if (!(len <= 3 && (start[0] == 'I' || start[0] == 'i')))
         {
            size_t copy = len < out_len - 1 ? len : out_len - 1;
            for (size_t i = 0; i < copy; i++)
               out[i] = (char)tolower((unsigned char)start[i]);
            out[copy] = '\0';
            return;
         }
      }
      word_index++;
   }
}

/* Common non-aggregation uses of the quantifier words.  "All at once",
 * "all right", "every time", etc. look like aggregation shapes to a naive
 * scanner but are idioms. */
static int agg_is_stop_phrase(const char *norm)
{
   static const char *stops[] = {"all at once",     "all right",  "all good",      "all day long",
                                 "all of a sudden", "every time", "every now and", "every so often",
                                 "every once in",   "each other", "each of us",    NULL};
   if (!norm)
      return 0;
   for (int i = 0; stops[i]; i++)
   {
      if (strstr(norm, stops[i]))
         return 1;
   }
   return 0;
}

int memory_detect_aggregation_shape(const char *query, memory_aggregation_hint_t *hint)
{
   if (!hint)
      return 0;
   memset(hint, 0, sizeof(*hint));
   if (!query || !query[0])
      return 0;

   char norm[1024];
   agg_normalize(query, norm, sizeof(norm));
   if (!norm[0])
      return 0;

   if (agg_is_stop_phrase(norm))
      return 0;

   /* Quantifier triggers — any one is enough. */
   int q_list = agg_starts_with(norm, "list") || agg_has_word(norm, "enumerate");
   int q_every = agg_has_word(norm, "every") || agg_has_word(norm, "each");
   int q_all = agg_has_word(norm, "all");
   int q_which = agg_starts_with(norm, "which");
   int q_what_plural = agg_starts_with(norm, "what") && agg_has_plural_noun(norm);
   int q_show = agg_starts_with(norm, "show me all") || agg_starts_with(norm, "show all");

   int quantifier = q_list || q_every || q_all || q_which || q_what_plural || q_show;
   if (!quantifier)
      return 0;

   /* "all" is the weakest signal — most "all X" idioms use it as an
    * intensifier ("all ready", "all good") rather than a quantifier.
    * Require a plural-noun target to accept it unless a stronger
    * quantifier is also present.  "every" wants a singular noun by
    * definition ("every decision", "every project"), so we trust the
    * stop-phrase list to screen its idioms. */
   int needs_plural = q_all && !(q_list || q_show || q_which || q_what_plural || q_every);
   if (needs_plural && !agg_has_plural_noun(norm))
      return 0;

   hint->has_quantifier = 1;

   /* Status words — trigger lifecycle routing once memory-lifecycle-states
    * lands.  Today we just record the keyword on the hint so the
    * aggregator can narrow results by content match. */
   static const char *status_words[] = {"open",      "pending", "unresolved", "past",
                                        "completed", "done",    "overdue",    NULL};
   for (int i = 0; status_words[i]; i++)
   {
      if (agg_has_word(norm, status_words[i]))
      {
         hint->has_status_word = 1;
         snprintf(hint->status_word, sizeof(hint->status_word), "%s", status_words[i]);
         break;
      }
   }

   agg_extract_entity_seed(query, hint->entity_seed, sizeof(hint->entity_seed));

   hint->detected = 1;
   return 1;
}

/* Build a reasonable fallback keyword when the hint lacks an entity seed.
 * Picks the longest non-plural, non-stopword alphabetic token from the
 * query.  Rationale: in a query like "list all decisions about pricing",
 * the plural "decisions" is the quantifier target (i.e. the `kind` axis)
 * rather than the topic the user cares about.  Preferring singular tokens
 * pushes the match toward "pricing", which is what lives in the memory
 * content.  Falls back to the longest plural-ended token when nothing
 * singular survives so single-noun queries like "list all meetings"
 * still get a non-empty keyword. */
#if !defined(AIMEE_DB2_DISABLED)
static void agg_extract_fallback_keyword(const char *query, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (!query)
      return;
   static const char *skip[] = {
       "the", "a",     "an",   "and", "or",   "of",   "to", "is", "are",   "was",  "has",   "have",
       "all", "every", "each", "any", "list", "show", "me", "us", "which", "what", "who",   "when",
       "how", "why",   "did",  "do",  "does", "been", "be", "it", "its",   "this", "about", NULL};

   char norm[1024];
   agg_normalize(query, norm, sizeof(norm));

   size_t best_len = 0;
   const char *best_start = NULL;
   size_t plural_best_len = 0;
   const char *plural_best_start = NULL;
   const char *p = norm;
   while (*p)
   {
      while (*p == ' ')
         p++;
      const char *start = p;
      while (*p && *p != ' ')
         p++;
      size_t len = (size_t)(p - start);
      if (len < 4)
         continue;
      int skipped = 0;
      for (int i = 0; skip[i]; i++)
      {
         size_t slen = strlen(skip[i]);
         if (slen == len && strncmp(start, skip[i], len) == 0)
         {
            skipped = 1;
            break;
         }
      }
      if (skipped)
         continue;

      int is_plural = (start[len - 1] == 's');
      if (is_plural)
      {
         if (len > plural_best_len)
         {
            plural_best_len = len;
            plural_best_start = start;
         }
      }
      else if (len > best_len)
      {
         best_len = len;
         best_start = start;
      }
   }
   if (!best_start)
   {
      best_start = plural_best_start;
      best_len = plural_best_len;
   }
   if (best_start && best_len > 0)
   {
      size_t copy = best_len < out_len - 1 ? best_len : out_len - 1;
      memcpy(out, best_start, copy);
      out[copy] = '\0';
   }
}
#endif

int memory_aggregate(const memory_aggregation_hint_t *hint, const char *query, int max_items,
                     memory_t *out, int max, int *truncated)
{
   if (truncated)
      *truncated = 0;
   if (!out || max <= 0 || !hint)
      return 0;

#if defined(AIMEE_DB2_DISABLED)
   (void)query;
   (void)max_items;
   return 0;
#else
   if (max_items <= 0)
      max_items = MEMORY_AGGREGATION_DEFAULT_MAX_ITEMS;
   if (max_items > max)
      max_items = max;

   const char *entity_seed = hint->entity_seed[0] ? hint->entity_seed : NULL;
   char keyword[128];
   keyword[0] = '\0';
   if (!entity_seed)
      agg_extract_fallback_keyword(query, keyword, sizeof(keyword));

   return db2_memory_aggregate(entity_seed, keyword[0] ? keyword : NULL, out, max_items, truncated);
#endif
}
