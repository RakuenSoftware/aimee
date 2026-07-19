#if defined(AIMEE_DB2_DISABLED)
#error "memory_core KB-real TU must not be compiled into the AIMEE_DB2_DISABLED (server) build"
#endif
#ifndef _GNU_SOURCE /* strcasestr/memmem are GNU extensions (container gcc) */
#define _GNU_SOURCE
#endif
#include "memory_core_internal.h"
/* memory_core_search.c: split from memory_core.c into a real translation unit
 * (was memory_core_search.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#include "aimee.h"
#include "memory_context_internal.h"
#include "memory_rewrite_llm.h" /* weak in-process rewrite seam (KB build only) */
#include <math.h>
#include "db1_optional.h"
#include "db2/entity_edges.h"
#include "db2/kb_runtime_state.h"
#include "db2/memory_health.h"
#include "db2/memory_payload.h"
#include "db2/feature_rows.h"
#include "db2/memory_promotion.h"
#include "db2/memory_query.h"
#include "memory_graph_fusion.h"
#include "kb_mdl.h"
#include "db2/memory_relations.h"
#include "db2/memory_scenes.h"
#include "db2/stopwords.h"
#include "db2/vector_index_ops.h"
#include "db2/vector_verify.h"
#include "memory_vectors.h"
#include "lifecycle.h"
#include "platform_process.h"
#include "memory_platform.h"
#include "log.h"
#include "util.h"       /* util_now_ms — memory.search stage timing */
#include "agent_exec.h" /* agent_http_post: in-process HTTP embedding (no fork) */
#include "cJSON.h"
#include "dogfood.h"
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

static double memory_temporal_bonus(int64_t memory_id, memory_query_intent_t intent,
                                    char qtokens[][64], int nq)
{
   db2_memory_temporal_ref_row_t rows[64];
   int nrows =
       db2_memory_temporal_refs_list(memory_id, rows, (int)(sizeof(rows) / sizeof(rows[0])));
   if (nrows <= 0)
      return 0.0;

   double bonus = 0.0;
   char norm_query[512];
   size_t used = 0;
   norm_query[0] = '\0';
   for (int i = 0; i < nq && used + 65 < sizeof(norm_query); i++)
      used += (size_t)snprintf(norm_query + used, sizeof(norm_query) - used, "%s%s",
                               used ? " " : "", qtokens[i]);
   int explicit_date = memory_temporal_query_has_explicit_date(qtokens, nq);
   int wants_recent = memory_query_wants_recent(norm_query);
   int wants_past = memory_query_wants_past(norm_query);
   int wants_future = memory_query_wants_future(norm_query);
   memory_temporal_constraint_t constraint;
   int has_constraint = memory_parse_temporal_constraint(norm_query, qtokens, nq, &constraint);
   for (int r = 0; r < nrows; r++)
   {
      const char *ref_key = rows[r].ref_key;
      const char *granularity = rows[r].granularity;
      double weight = rows[r].weight;
      int matched_ref = 0;
      if (!ref_key[0])
         continue;

      if (intent == MEM_QUERY_TEMPORAL)
         bonus += 0.42 + weight * 0.18;

      if (memory_temporal_ref_matches_query(ref_key, qtokens, nq))
      {
         bonus += 0.34 + weight * 0.12;
         matched_ref = 1;
      }

      if (!matched_ref && granularity[0] && strcmp(granularity, "date_phrase") == 0 &&
          intent == MEM_QUERY_TEMPORAL)
         bonus += 0.15;
      if (intent == MEM_QUERY_TEMPORAL && granularity[0] &&
          (strcmp(granularity, "ordering") == 0 || strcmp(granularity, "range") == 0))
         bonus += 0.25;
      if (explicit_date && granularity[0] &&
          (strcmp(granularity, "absolute_day") == 0 || strcmp(granularity, "date_phrase") == 0))
         bonus += 0.18;
      if (wants_recent && granularity[0] &&
          (strcmp(granularity, "absolute_day") == 0 || strcmp(granularity, "date_phrase") == 0))
         bonus += 0.08 + weight * 0.03;
      if ((wants_past || wants_future) && granularity[0] && strcmp(granularity, "ordering") == 0)
         bonus += 0.14;
      if (has_constraint)
      {
         memory_parsed_date_t candidate_date;
         if (memory_parse_temporal_ref_date(ref_key, granularity, &candidate_date))
            bonus += memory_temporal_constraint_score(&constraint, &candidate_date);
      }
   }
   return bonus;
}

static double memory_evidence_bonus(int64_t memory_id)
{
   double evidence = 0.0;
   int observations = 0;
   if (!db2_memory_get_evidence_fields(memory_id, &evidence, &observations))
      return 0.0;
   double bonus = evidence * 0.8;
   if (observations > 1)
      bonus += (double)(observations - 1) * 0.05;
   return bonus;
}

static double memory_salience_bonus(int64_t memory_id)
{
   if (!memory_salience_enabled())
      return 0.0;
   double salience = db2_memory_get_salience(memory_id, 0.5);
   return (salience - 0.5) * memory_salience_weight();
}

static double memory_surprise_bonus(int64_t memory_id)
{
   if (!memory_surprise_enabled())
      return 0.0;
   double surprise = db2_memory_get_surprise(memory_id, 0.5);
   return (surprise - 0.5) * memory_surprise_weight();
}

static double memory_unit_graph_bonus(int64_t memory_id, char qtokens[][64], int nq)
{
   db2_memory_unit_row_t units[64];
   int nunits = db2_memory_units_list(memory_id, units, (int)(sizeof(units) / sizeof(units[0])));
   if (nunits <= 0)
      return 0.0;

   int64_t matched_ids[16];
   int matched_count = 0;
   int saw_summary = 0, saw_event = 0, saw_temporal = 0, saw_entity = 0, saw_chunk = 0;
   double bonus = 0.0;
   int signal_count = 0;
   for (int u = 0; u < nunits; u++)
   {
      int64_t unit_id = units[u].id;
      const char *unit_type = units[u].unit_type;
      const char *unit_key = units[u].unit_key;
      const char *unit_text = units[u].unit_text;
      double weight = units[u].weight;
      int matched_tokens = 0;
      for (int i = 0; i < nq; i++)
      {
         if (!memory_is_signal_token(qtokens[i]))
            continue;
         char canonical_q[64];
         memory_canonicalize_term(qtokens[i], canonical_q, sizeof(canonical_q));
         if ((canonical_q[0] && ((unit_key[0] && memory_token_in_norm(unit_key, canonical_q)) ||
                                 (unit_text[0] && memory_token_in_norm(unit_text, canonical_q)))) ||
             (unit_key[0] && memory_token_in_norm(unit_key, qtokens[i])) ||
             (unit_text[0] && memory_token_in_norm(unit_text, qtokens[i])))
            matched_tokens++;
      }
      if (matched_tokens <= 0)
         continue;
      int allow_single =
          unit_type[0] && (strcmp(unit_type, "entity") == 0 || strcmp(unit_type, "temporal") == 0 ||
                           strcmp(unit_type, "event") == 0);
      if (matched_tokens < 2 && !allow_single)
         continue;
      signal_count++;
      bonus += 0.08 + weight * 0.04 + (double)(matched_tokens > 2 ? 2 : matched_tokens) * 0.03;
      if (matched_count < 16)
         matched_ids[matched_count++] = unit_id;
      if (unit_type[0])
      {
         if (strcmp(unit_type, "summary") == 0)
            saw_summary = 1;
         else if (strcmp(unit_type, "event") == 0)
            saw_event = 1;
         else if (strcmp(unit_type, "temporal") == 0)
            saw_temporal = 1;
         else if (strcmp(unit_type, "entity") == 0)
            saw_entity = 1;
         else if (strcmp(unit_type, "chunk") == 0)
            saw_chunk = 1;
      }
   }

   if (signal_count == 0)
      return 0.0;

   if ((saw_event && saw_temporal) || (saw_event && saw_entity))
      bonus += 0.14;
   if (saw_summary && (saw_event || saw_chunk))
      bonus += 0.08;

   for (int i = 0; i < matched_count; i++)
   {
      for (int j = i + 1; j < matched_count; j++)
      {
         if (db2_memory_unit_edge_exists(matched_ids[i], matched_ids[j]))
            bonus += 0.04;
      }
   }
   if (bonus > 0.85)
      bonus = 0.85;
   return bonus;
}

static double memory_state_penalty(int64_t memory_id)
{
   int has_valid_until = 0;
   int observations = 0;
   int use_count = 0;
   if (!db2_memory_get_state_fields(memory_id, &has_valid_until, &observations, &use_count))
      return 0.0;
   double penalty = 0.0;
   if (has_valid_until)
      penalty -= 1.8;
   if (observations <= 1)
      penalty -= 0.15;
   if (use_count == 0)
      penalty -= 0.05;
   return penalty;
}

memory_ranker_input_t memory_ranker_input_from(const memory_t *m)
{
   memory_ranker_input_t r;
   memset(&r, 0, sizeof(r));
   if (!m)
      return r;
   r.id = m->id;
   snprintf(r.kind, sizeof(r.kind), "%s", m->kind);
   snprintf(r.key, sizeof(r.key), "%s", m->key);
   snprintf(r.content, sizeof(r.content), "%s", m->content);
   snprintf(r.use_cases, sizeof(r.use_cases), "%s", m->use_cases);
   return r;
}

void memory_compute_score_parts(const char *raw_query, const char *norm_query,
                                const memory_ranker_input_t *m, const int64_t *semantic_ids,
                                const double *semantic_scores, int semantic_hit_count,
                                const memory_pagerank_score_t *pagerank_scores, int pagerank_count,
                                memory_score_parts_t *parts)
{
   if (!parts)
      return;
   memset(parts, 0, sizeof(*parts));

   char qtokens[32][64];
   char norm_key[sizeof(m->key)];
   char norm_content[sizeof(m->content)];
   char norm_use_cases[sizeof(m->use_cases)];
   int nq = memory_split_tokens(norm_query, qtokens, 32);
   if (nq <= 0)
      return;

   memory_rank_weights_t w = memory_rank_weights();
   memory_query_intent_t intent = memory_query_intent(raw_query, norm_query);
   normalize_key(m->key, norm_key, sizeof(norm_key));
   normalize_key(m->content, norm_content, sizeof(norm_content));
   normalize_key(m->use_cases, norm_use_cases, sizeof(norm_use_cases));

   int matched = 0;
   for (int i = 0; i < nq; i++)
   {
      int in_key = memory_token_in_norm(norm_key, qtokens[i]);
      int in_content = memory_token_in_norm(norm_content, qtokens[i]);
      int in_use_cases = memory_token_in_norm(norm_use_cases, qtokens[i]);
      if (in_key || in_content || in_use_cases)
      {
         double v = in_key ? w.lexical_key : w.lexical_content;
         size_t len = strlen(qtokens[i]);
         if (len >= 6)
            v += w.lexical_long_token_bonus;
         parts->lexical += v;
         matched++;
      }
   }
   parts->entity = memory_entity_bonus(m->id, qtokens, nq) * w.entity +
                   memory_speaker_bonus(m->id, raw_query) * w.speaker;
   parts->temporal = memory_temporal_bonus(m->id, intent, qtokens, nq) * w.temporal;
   parts->evidence =
       (memory_evidence_bonus(m->id) + memory_unit_graph_bonus(m->id, qtokens, nq)) * w.evidence;
   parts->semantic =
       memory_semantic_bonus_lookup(m->id, semantic_ids, semantic_scores, semantic_hit_count) *
       w.semantic;
   parts->state = memory_state_penalty(m->id) * w.state;
   parts->coverage = ((double)matched / (double)nq) * w.coverage;
   parts->salience = memory_salience_bonus(m->id) * w.salience;
   parts->surprise = memory_surprise_bonus(m->id) * w.surprise;
   parts->pagerank = memory_pagerank_bonus_lookup(pagerank_scores, pagerank_count, m->id);
   parts->intent = 0.0;
   if (intent == MEM_QUERY_PROCEDURAL && strcmp(m->kind, KIND_WORKFLOW) == 0)
      parts->intent += w.workflow_intent;
   else if (intent == MEM_QUERY_ENTITY &&
            (strcmp(m->kind, KIND_FACT) == 0 || strcmp(m->kind, KIND_PREFERENCE) == 0))
      parts->intent += w.entity_intent;
   else if (intent == MEM_QUERY_TEMPORAL &&
            (strcmp(m->kind, KIND_FACT) == 0 || strcmp(m->kind, KIND_EPISODE) == 0))
      parts->intent += w.temporal_intent;

   if (matched == 0)
   {
      double support = parts->semantic + parts->entity + parts->temporal + parts->evidence;
      double min_support = memory_env_weight("AIMEE_MEMORY_NO_LEXICAL_MIN_SUPPORT", 0.85);
      double no_lexical_penalty = memory_env_weight("AIMEE_MEMORY_NO_LEXICAL_PENALTY", 0.4);
      if (support < min_support)
      {
         memset(parts, 0, sizeof(*parts));
         return;
      }
      parts->state -= no_lexical_penalty;
   }

   /* parts->confidence is NOT set here: confidence is a calibration artifact,
    * not a retrieval-relevance signal.  Callers populate it post-ranking from
    * the full memory_t for display/trace only. */
   /* Graph-code fusion contribution. When the recall path staged graph-vector
    * expansions for this request (graph_code_fusion_state == "on"), this
    * populates parts->graph_score / parts->code_proximity for the memory; when
    * fusion is off the hook is a no-op, graph_score stays 0, the term below adds
    * 0, and the score is byte-identical to the pre-fusion behaviour. */
   memory_fusion_expansions_apply(parts, m->id);
   parts->total = parts->lexical + parts->coverage + parts->entity + parts->temporal +
                  parts->evidence + parts->semantic + parts->state + parts->intent +
                  parts->salience + parts->surprise + parts->pagerank +
                  memory_env_weight("AIMEE_MEMORY_GRAPH_WEIGHT", 0.30) * parts->graph_score;
}

int memory_token_in_norm(const char *norm, const char *token)
{
   if (!norm || !token || !token[0])
      return 0;
   size_t tlen = strlen(token);
   const char *p = norm;
   while ((p = strstr(p, token)) != NULL)
   {
      int left_ok = (p == norm) || p[-1] == ' ';
      int right_ok = p[tlen] == '\0' || p[tlen] == ' ';
      if (left_ok && right_ok)
         return 1;
      p += tlen;
   }
   return 0;
}

int memory_split_tokens(const char *norm, char tokens[][64], int max_tokens)
{
   if (!norm || !norm[0] || !tokens || max_tokens <= 0)
      return 0;
   int count = 0;
   const char *p = norm;
   while (*p && count < max_tokens)
   {
      while (*p == ' ')
         p++;
      if (!*p)
         break;
      int len = 0;
      while (p[len] && p[len] != ' ' && len < 63)
      {
         tokens[count][len] = p[len];
         len++;
      }
      tokens[count][len] = '\0';
      count++;
      p += len;
      while (*p && *p != ' ')
         p++;
   }
   return count;
}

static void memory_build_signal_query(char tokens[][64], int token_count, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (!tokens || token_count <= 0)
      return;

   size_t used = 0;
   for (int i = 0; i < token_count; i++)
   {
      if (!memory_is_signal_token(tokens[i]))
         continue;
      int wrote = snprintf(out + used, out_len - used, "%s%s", used ? " " : "", tokens[i]);
      if (wrote < 0)
         break;
      if ((size_t)wrote >= out_len - used)
      {
         out[out_len - 1] = '\0';
         break;
      }
      used += (size_t)wrote;
   }
}

static int memory_query_has_separator(char tokens[][64], int token_count, int idx)
{
   if (!tokens || idx < 0 || idx >= token_count)
      return 0;
   return strcmp(tokens[idx], "and") == 0 || strcmp(tokens[idx], "or") == 0 ||
          strcmp(tokens[idx], "vs") == 0 || strcmp(tokens[idx], "versus") == 0 ||
          strcmp(tokens[idx], "then") == 0 || strcmp(tokens[idx], "before") == 0 ||
          strcmp(tokens[idx], "after") == 0;
}

static int memory_compact_signal_tokens(char tokens[][64], int start, int end, char *out,
                                        size_t out_len)
{
   if (!out || out_len == 0)
      return 0;
   out[0] = '\0';
   if (!tokens || start < 0 || end <= start)
      return 0;

   size_t used = 0;
   int kept = 0;
   for (int i = start; i < end; i++)
   {
      if (!memory_is_signal_token(tokens[i]) || memory_query_has_separator(tokens, end, i))
         continue;
      int wrote = snprintf(out + used, out_len - used, "%s%s", used ? " " : "", tokens[i]);
      if (wrote < 0 || (size_t)wrote >= out_len - used)
         break;
      used += (size_t)wrote;
      kept++;
   }
   return kept;
}

int memory_build_query_decomposition(const char *norm_query, char subqueries[][128],
                                     int max_subqueries)
{
   if (!norm_query || !norm_query[0] || !subqueries || max_subqueries <= 0)
      return 0;

   char tokens[32][64];
   int token_count = memory_split_tokens(norm_query, tokens, 32);
   if (token_count < 4)
      return 0;

   int count = 0;
   for (int i = 0; i < token_count && count + 1 < max_subqueries; i++)
   {
      if (!memory_query_has_separator(tokens, token_count, i))
         continue;
      if (memory_compact_signal_tokens(tokens, 0, i, subqueries[count], sizeof(subqueries[0])) >= 2)
         count++;
      if (memory_compact_signal_tokens(tokens, i + 1, token_count, subqueries[count],
                                       sizeof(subqueries[0])) >= 2)
         count++;
      break;
   }

   if (count == 0)
   {
      char signal_tokens[32][64];
      int signal_count = 0;
      for (int i = 0; i < token_count && signal_count < 32; i++)
      {
         if (!memory_is_signal_token(tokens[i]))
            continue;
         snprintf(signal_tokens[signal_count], sizeof(signal_tokens[signal_count]), "%s",
                  tokens[i]);
         signal_count++;
      }
      if (signal_count >= 5)
      {
         int window = signal_count >= 8 ? 4 : 3;
         if (memory_compact_signal_tokens(signal_tokens, 0, window, subqueries[count],
                                          sizeof(subqueries[0])) >= 2)
            count++;
         if (count < max_subqueries &&
             memory_compact_signal_tokens(signal_tokens, signal_count - window, signal_count,
                                          subqueries[count], sizeof(subqueries[0])) >= 2)
            count++;
      }
   }

   for (int i = 0; i < count; i++)
   {
      for (int j = i + 1; j < count; j++)
      {
         if (strcmp(subqueries[i], subqueries[j]) == 0)
            subqueries[j][0] = '\0';
      }
   }

   int compacted = 0;
   for (int i = 0; i < count; i++)
   {
      if (!subqueries[i][0])
         continue;
      if (compacted != i)
         snprintf(subqueries[compacted], sizeof(subqueries[compacted]), "%s", subqueries[i]);
      compacted++;
   }
   return compacted;
}

static double memory_session_cluster_bonus(const memory_t *matches, int count, int idx)
{
   if (!matches || count <= 1 || idx < 0 || idx >= count || !matches[idx].source_session[0])
      return 0.0;
   int same_session = 0;
   int kind_variety = 0;
   int has_episode = 0;
   for (int i = 0; i < count; i++)
   {
      if (i == idx || strcmp(matches[i].source_session, matches[idx].source_session) != 0)
         continue;
      same_session++;
      if (strcmp(matches[i].kind, matches[idx].kind) != 0)
         kind_variety = 1;
      if (strcmp(matches[i].kind, KIND_EPISODE) == 0)
         has_episode = 1;
   }
   double bonus = 0.0;
   if (same_session > 0)
      bonus += (double)(same_session > 3 ? 3 : same_session) * 0.12;
   if (kind_variety)
      bonus += 0.08;
   if (has_episode)
      bonus += 0.10;
   if (strcmp(matches[idx].kind, KIND_EPISODE) == 0 && same_session > 0)
      bonus += 0.05;
   return bonus;
}

/* Scene cluster bonus: boost candidates that share a scene.
 * Looks up scene memberships for all candidates in one SQL query per candidate,
 * then counts scene co-membership and adds a bonus for popular scenes. */
static double memory_scene_cluster_bonus(const memory_t *matches, int count, int idx)
{
   if (!matches || count <= 1 || idx < 0 || idx >= count)
      return 0.0;

   config_t cfg;
   config_load(&cfg);
   if (!cfg.memory_scenes_enabled)
      return 0.0;

   db2_memory_scene_membership_t memberships[16];
   int scene_count = db2_memory_scene_memberships_for_memory(
       matches[idx].id, memberships, (int)(sizeof(memberships) / sizeof(memberships[0])));
   if (scene_count <= 0)
      return 0.0;

   int co_members = 0;
   double best_strength = 0.0;
   for (int i = 0; i < count && co_members < 3; i++)
   {
      if (i == idx)
         continue;
      for (int s = 0; s < scene_count; s++)
      {
         if (db2_memory_scene_member_exists(matches[i].id, memberships[s].scene_id))
         {
            co_members++;
            if (memberships[s].membership_strength > best_strength)
               best_strength = memberships[s].membership_strength;
         }
         if (co_members >= 3)
            break;
      }
   }

   double bonus = co_members * 0.10 + best_strength * 0.05;
   return bonus;
}

void memory_note_candidate_sources(memory_candidate_source_t *stats, int *stats_count,
                                   int stats_max, const memory_t *matches, int start, int end,
                                   unsigned int source_mask)
{
   if (!stats || !stats_count || !matches || source_mask == 0)
      return;
   if (start < 0)
      start = 0;
   if (end < start)
      return;
   for (int i = start; i < end; i++)
   {
      int found = -1;
      for (int j = 0; j < *stats_count; j++)
      {
         if (stats[j].memory_id == matches[i].id)
         {
            found = j;
            break;
         }
      }
      if (found >= 0)
      {
         stats[found].source_mask |= source_mask;
         continue;
      }
      if (*stats_count >= stats_max)
         continue;
      stats[*stats_count].memory_id = matches[i].id;
      stats[*stats_count].source_mask = source_mask;
      (*stats_count)++;
   }
}

static double memory_source_fusion_bonus(const memory_candidate_source_t *stats, int stats_count,
                                         int64_t memory_id)
{
   double bonus = 0.0;
   for (int i = 0; i < stats_count; i++)
   {
      if (stats[i].memory_id != memory_id)
         continue;
      unsigned int mask = stats[i].source_mask;
      int sources = 0;
      while (mask)
      {
         if (mask & 1u)
            sources++;
         mask >>= 1u;
      }
      if (sources > 1)
         bonus += (double)(sources - 1) * 0.10;
      if ((stats[i].source_mask & MEM_SOURCE_SEMANTIC) &&
          (stats[i].source_mask & MEM_SOURCE_LEXICAL))
         bonus += 0.18;
      if (stats[i].source_mask & MEM_SOURCE_GRAPH)
         bonus += 0.14;
      if ((stats[i].source_mask & MEM_SOURCE_EVENT) || (stats[i].source_mask & MEM_SOURCE_TEMPORAL))
         bonus += 0.08;
      break;
   }
   return bonus;
}

void memory_record_query_stage_metric(const memory_query_plan_t *plan, const char *stage_name)
{
   char key[128];
   if (!stage_name || !stage_name[0])
      return;
   snprintf(key, sizeof(key), "memory.query.stage.%s", stage_name);
   memory_runtime_state_increment(key, 1);
   if (plan)
   {
      snprintf(key, sizeof(key), "memory.query.route.%s.stage.%s",
               memory_query_route_name(plan->route), stage_name);
      memory_runtime_state_increment(key, 1);
   }
}

static int memory_rank_position(const double *values, int count, int idx, double min_value)
{
   if (!values || idx < 0 || idx >= count || values[idx] < min_value)
      return 0;
   int rank = 1;
   for (int i = 0; i < count; i++)
   {
      if (i == idx || values[i] < min_value)
         continue;
      if (values[i] > values[idx] + 1e-9)
         rank++;
   }
   return rank;
}

static double memory_rrf_bonus(const memory_score_parts_t *parts, const double *phrase_bonus,
                               const double *fusion_bonus, int count, int idx)
{
   if (!parts || idx < 0 || idx >= count)
      return 0.0;

   double lexical_signal[128];
   double semantic_signal[128];
   double entity_signal[128];
   double temporal_signal[128];
   double evidence_signal[128];
   double source_signal[128];
   int n = count > 128 ? 128 : count;
   for (int i = 0; i < n; i++)
   {
      lexical_signal[i] =
          parts[i].lexical + parts[i].coverage + (phrase_bonus ? phrase_bonus[i] : 0.0);
      semantic_signal[i] = parts[i].semantic;
      entity_signal[i] = parts[i].entity;
      temporal_signal[i] = parts[i].temporal;
      evidence_signal[i] = parts[i].evidence;
      source_signal[i] = fusion_bonus ? fusion_bonus[i] : 0.0;
   }

   static const double rank_k = 20.0;
   static const double scale = 3.0;
   double sum = 0.0;
   int rank = memory_rank_position(lexical_signal, n, idx, 0.30);
   if (rank > 0)
      sum += 1.0 / (rank_k + (double)rank);
   rank = memory_rank_position(semantic_signal, n, idx, 0.18);
   if (rank > 0)
      sum += 1.0 / (rank_k + (double)rank);
   rank = memory_rank_position(entity_signal, n, idx, 0.12);
   if (rank > 0)
      sum += 1.0 / (rank_k + (double)rank);
   rank = memory_rank_position(temporal_signal, n, idx, 0.12);
   if (rank > 0)
      sum += 1.0 / (rank_k + (double)rank);
   rank = memory_rank_position(evidence_signal, n, idx, 0.12);
   if (rank > 0)
      sum += 1.0 / (rank_k + (double)rank);
   rank = memory_rank_position(source_signal, n, idx, 0.10);
   if (rank > 0)
      sum += 1.0 / (rank_k + (double)rank);
   return sum * scale;
}

static double memory_phrase_bonus(const char *raw_query, const char *norm_query,
                                  const memory_ranker_input_t *m)
{
   if (!m)
      return 0.0;

   char norm_key[512];
   char norm_content[2048];
   normalize_key(m->key, norm_key, sizeof(norm_key));
   normalize_key(m->content, norm_content, sizeof(norm_content));

   double bonus = 0.0;
   if (norm_query && norm_query[0])
   {
      if (strstr(norm_key, norm_query))
         bonus += 1.15;
      if (strstr(norm_content, norm_query))
         bonus += 0.95;
   }

   if (raw_query && raw_query[0] && strstr(m->content, raw_query))
      bonus += 0.35;

   return bonus;
}

/* Cross-encoder reranker: calls an external command with a JSON array of
 * [query, candidate_text] pairs on stdin; expects a JSON float array of scores on stdout.
 * Returns the number of scores populated in out_scores (≤ count).
 * On any failure (command missing, parse error, timeout) returns 0 so the caller
 * can fall back to hybrid ordering. */
static int memory_cross_encoder_scores(const char *command, const char *query,
                                       const memory_t *matches, int count, double *out_scores)
{
   if (!command || !command[0] || !query || !query[0] || !matches || count <= 0 || !out_scores)
      return 0;

   /* Build JSON input: [[query, text0], [query, text1], ...] */
   cJSON *arr = cJSON_CreateArray();
   if (!arr)
      return 0;

   for (int i = 0; i < count; i++)
   {
      /* Candidate text = "key: content" (same format used for embeddings) */
      char cand[3072];
      snprintf(cand, sizeof(cand), "%s: %s", matches[i].key, matches[i].content);

      cJSON *pair = cJSON_CreateArray();
      if (!pair)
      {
         cJSON_Delete(arr);
         return 0;
      }
      cJSON_AddItemToArray(pair, cJSON_CreateString(query));
      cJSON_AddItemToArray(pair, cJSON_CreateString(cand));
      cJSON_AddItemToArray(arr, pair);
   }

   char *input_json = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   if (!input_json)
      return 0;

   char *buf = NULL;
   size_t buf_len = 0;
   long long _rr_t0 = util_now_ms();
   int rc = platform_exec_pipe(command, input_json, strlen(input_json), &buf, &buf_len);
   s_rerank_ms += util_now_ms() - _rr_t0;
   s_rerank_calls++;
   free(input_json);
   if (rc != 0)
   {
      aimee_log(LOG_WARN, "memory", "cross-encoder command failed (exit %d)", rc);
      free(buf);
      return 0;
   }
   if (!buf || buf_len == 0)
   {
      free(buf);
      return 0;
   }

   cJSON *scores = cJSON_Parse(buf);
   free(buf);
   if (!scores || !cJSON_IsArray(scores))
   {
      cJSON_Delete(scores);
      aimee_log(LOG_WARN, "memory", "cross-encoder returned invalid JSON; falling back");
      return 0;
   }

   int n = 0;
   cJSON *el;
   cJSON_ArrayForEach(el, scores)
   {
      if (n >= count)
         break;
      out_scores[n++] = cJSON_IsNumber(el) ? el->valuedouble : 0.0;
   }
   cJSON_Delete(scores);
   return n;
}

/* Return the effective cross-encoder command, or NULL if disabled.
 * AIMEE_MEMORY_RERANK_FORCE_OFF overrides config — used by the
 * benchmark harness' --compare-rerank mode to run the same corpus
 * twice and surface the delta. */
static const char *memory_cross_encoder_command(const config_t *cfg)
{
   if (!cfg || !cfg->memory_rerank_enabled)
      return NULL;
   const char *force_off = getenv("AIMEE_MEMORY_RERANK_FORCE_OFF");
   if (force_off && force_off[0] && force_off[0] != '0')
      return NULL;
   if (!cfg->memory_rerank_command[0])
      return NULL;
   return cfg->memory_rerank_command;
}

/* Count shared whitespace-delimited tokens between two token strings (as produced
 * by extract_negation_tokens, e.g. "not_disk not_written"). Used by the
 * negation-aware rerank to measure how strongly a candidate's negated concept
 * overlaps the query's. O(a*b) over the few negation tokens each carries. */
static int neg_token_overlap(const char *a, const char *b)
{
   if (!a || !a[0] || !b || !b[0])
      return 0;
   int shared = 0;
   char abuf[1024];
   snprintf(abuf, sizeof(abuf), "%s", a);
   for (char *sa = strtok(abuf, " "); sa; sa = strtok(NULL, " "))
   {
      /* word-boundary search for sa within b */
      const char *p = b;
      size_t la = strlen(sa);
      while ((p = strstr(p, sa)) != NULL)
      {
         int left_ok = (p == b) || p[-1] == ' ';
         int right_ok = (p[la] == '\0' || p[la] == ' ');
         if (left_ok && right_ok)
         {
            shared++;
            break;
         }
         p += la;
      }
   }
   return shared;
}

int memory_rerank_matches(const char *raw_query, memory_t *matches, int count, int limit,
                          const int64_t *semantic_ids, const double *semantic_scores,
                          int semantic_hit_count, const memory_candidate_source_t *source_stats,
                          int source_stats_count)
{
   if (!raw_query || !matches || count <= 1)
      return count < limit ? count : limit;

   char norm_query[512];
   normalize_key(raw_query, norm_query, sizeof(norm_query));
   if (!norm_query[0])
      snprintf(norm_query, sizeof(norm_query), "%s", raw_query);

   double scores[128];
   double phrase_bonus[128];
   double fusion_bonus[128];
   memory_score_parts_t parts[128];
   memory_pagerank_score_t pagerank_scores[128];
   memory_pagerank_config_t pagerank_cfg;
   if (count > (int)(sizeof(scores) / sizeof(scores[0])))
      count = (int)(sizeof(scores) / sizeof(scores[0]));
   memory_load_pagerank_config(&pagerank_cfg);
   if (pagerank_cfg.enabled)
      count = memory_append_linked_neighbors(matches, count, 128, pagerank_cfg.relations);
   int pagerank_count =
       memory_compute_pagerank_scores(matches, count, &pagerank_cfg, pagerank_scores, 128);
   for (int i = 0; i < count; i++)
   {
      memory_ranker_input_t ri = memory_ranker_input_from(&matches[i]);
      memory_compute_score_parts(raw_query, norm_query, &ri, semantic_ids, semantic_scores,
                                 semantic_hit_count, pagerank_scores, pagerank_count, &parts[i]);
      scores[i] = parts[i].total;
      double fusion = memory_source_fusion_bonus(source_stats, source_stats_count, matches[i].id);
      if (scores[i] < 3.5)
         fusion *= 0.15;
      else if (scores[i] < 5.5)
         fusion *= 0.45;
      fusion_bonus[i] = fusion;
      /* Phase 6: surface the applied multi-source fusion bonus in score parts
       * so `memory diagnose` can explain the fused contribution. */
      parts[i].source_fusion = fusion;
      phrase_bonus[i] = 0.0;
      scores[i] += fusion_bonus[i];
      scores[i] += memory_session_cluster_bonus(matches, count, i);
      scores[i] += memory_scene_cluster_bonus(matches, count, i);
   }

   if (memory_rerank_is_slow())
   {
      int slow_top = count > 16 ? 16 : count;
      for (int i = 0; i < slow_top; i++)
      {
         memory_ranker_input_t pb_ri = memory_ranker_input_from(&matches[i]);
         phrase_bonus[i] = memory_phrase_bonus(raw_query, norm_query, &pb_ri);
         scores[i] += phrase_bonus[i];
      }
   }

   for (int i = 0; i < count; i++)
   {
      double rrf = memory_rrf_bonus(parts, phrase_bonus, fusion_bonus, count, i);
      if (scores[i] < 3.5)
         rrf *= 0.35;
      scores[i] += rrf;
   }

   /* Capture the pre-rerank hybrid score on every candidate so the explain
    * surface can differentiate hybrid vs blended even for rows outside the
    * cross-encoder top_k window. When the CE pass runs, it overwrites
    * blended_total + rerank_mix for its window; rows beyond it keep a zero
    * rerank_mix (which the explain serializer reads as "not reranked"). */
   for (int i = 0; i < count; i++)
   {
      parts[i].hybrid_total = scores[i];
      parts[i].blended_total = scores[i];
      parts[i].rerank_mix = 0.0;
      matches[i].retrieval_score = scores[i];
      matches[i].hybrid_rank = 0;
   }

   /* Cross-encoder second pass: score top-K candidates with a local cross-encoder model
    * (external command), then blend with the hybrid score.
    * Falls back silently to hybrid ordering if the command is not configured or fails. */
   config_t ce_cfg;
   config_load(&ce_cfg);
   const char *ce_cmd = memory_cross_encoder_command(&ce_cfg);
   if (ce_cmd)
   {
      int top_k = ce_cfg.memory_rerank_top_k > 0 ? ce_cfg.memory_rerank_top_k : 50;
      if (top_k > count)
         top_k = count;
      double mix = (ce_cfg.memory_rerank_mix > 0.0 && ce_cfg.memory_rerank_mix <= 1.0)
                       ? ce_cfg.memory_rerank_mix
                       : 0.7;

      double ce_scores[128] = {0};
      int ce_count = memory_cross_encoder_scores(ce_cmd, raw_query, matches, top_k, ce_scores);
      if (ce_count == top_k)
      {
         /* Normalise cross-encoder scores to [0, 1] for blending */
         double ce_min = ce_scores[0], ce_max = ce_scores[0];
         for (int i = 1; i < ce_count; i++)
         {
            if (ce_scores[i] < ce_min)
               ce_min = ce_scores[i];
            if (ce_scores[i] > ce_max)
               ce_max = ce_scores[i];
         }
         double ce_range = ce_max - ce_min;

         /* Normalise hybrid scores for the same pool */
         double h_min = scores[0], h_max = scores[0];
         for (int i = 1; i < top_k; i++)
         {
            if (scores[i] < h_min)
               h_min = scores[i];
            if (scores[i] > h_max)
               h_max = scores[i];
         }
         double h_range = h_max - h_min;

         for (int i = 0; i < ce_count; i++)
         {
            double ce_norm = (ce_range > 0) ? (ce_scores[i] - ce_min) / ce_range : 0.5;
            double h_norm = (h_range > 0) ? (scores[i] - h_min) / h_range : 0.5;
            double blended = mix * ce_norm + (1.0 - mix) * h_norm;
            parts[i].cross_encoder = ce_scores[i];
            parts[i].hybrid_total = scores[i];
            parts[i].rerank_mix = mix;
            scores[i] = blended * 10.0; /* rescale to ~same magnitude as hybrid */
            parts[i].blended_total = scores[i];
         }
      }
      else if (ce_count > 0)
      {
         /* Partial result — warn and fall back */
         aimee_log(LOG_WARN, "memory",
                   "cross-encoder returned %d scores for %d candidates; using hybrid ordering",
                   ce_count, top_k);
      }
   }

   /* Contradiction-aware reranking: penalize older memories that contradict a
    * higher-ranked candidate.  Only applied to the top-16 pool to keep cost
    * proportional.  A memory that is dominated by a newer conflicting fact
    * receives a score reduction of 30% of the dominator's score gap. */
   {
      int contra_top = count > 16 ? 16 : count;
      for (int i = 0; i < contra_top; i++)
      {
         for (int j = i + 1; j < contra_top; j++)
         {
            if (!matches[i].content[0] || !matches[j].content[0])
               continue;
            if (!is_contradiction(matches[i].content, matches[j].content))
               continue;
            if (matches[i].created_at[0] && matches[j].created_at[0])
            {
               int newer = strcmp(matches[i].created_at, matches[j].created_at);
               if (newer == 0)
                  continue;
               int newer_idx = newer > 0 ? i : j;
               int older_idx = newer > 0 ? j : i;
               double gap = fabs(scores[newer_idx] - scores[older_idx]);
               double shift = 0.18 + gap * 0.22;
               scores[newer_idx] += shift;
               scores[older_idx] -= shift;
            }
         }
      }
   }

   /* Negation-aware reranking (memory_negation_enabled): when the query is
    * negatively polarised, promote candidates whose content carries the SAME
    * negated concept as the query -- i.e. whose extracted "not_<token>" set
    * overlaps the query's. A negated fact and its affirmative near-twin score
    * near-identically under bag-of-words / hash-embedding retrieval; the twin
    * emits NO negation tokens, so only the correctly-negated fact is boosted and
    * it rises above the twin. Deterministic and store-agnostic (operates on the
    * candidate text already in hand), so it holds on both the sqlite shim and
    * libpq. Complements the FTS candidate-generation lane (memory_negation_fts_tsv)
    * which widens RECALL at scale; this fixes RANKING. */
   {
      config_t neg_cfg;
      if (config_load(&neg_cfg) == 0 && neg_cfg.memory_negation_enabled &&
          memory_query_polarity(raw_query) == POLARITY_NEGATIVE)
      {
         char qneg[1024];
         int qn = extract_negation_tokens(raw_query, qneg, sizeof(qneg));
         if (qn > 0 && qneg[0])
         {
            int neg_top = count > 32 ? 32 : count;
            for (int i = 0; i < neg_top; i++)
            {
               if (!matches[i].content[0])
                  continue;
               char combined[3072];
               snprintf(combined, sizeof(combined), "%s %s", matches[i].key, matches[i].content);
               char cneg[2048];
               int cn = extract_negation_tokens(combined, cneg, sizeof(cneg));
               if (cn <= 0)
                  continue;
               int shared = neg_token_overlap(qneg, cneg);
               if (shared > 0)
               {
                  /* Boost by the strength of the negated-concept overlap (per
                   * shared "not_<token>"), scaled by the fraction of the query's
                   * negation matched so a candidate that covers MORE of the query
                   * wins over one sharing a single incidental token. Capped so a
                   * strong polarity match reliably flips a near-tie with an
                   * affirmative twin without swamping the whole ranking. */
                  double frac = (double)shared / (double)qn;
                  if (frac > 1.0)
                     frac = 1.0;
                  double boost = 3.0 * (double)shared * (0.5 + 0.5 * frac);
                  if (boost > 10.0)
                     boost = 10.0;
                  scores[i] += boost;
               }
            }
         }
      }
   }

   for (int i = 0; i < count - 1; i++)
   {
      int best = i;
      for (int j = i + 1; j < count; j++)
      {
         if (scores[j] > scores[best])
            best = j;
      }
      if (best != i)
      {
         double ts = scores[i];
         memory_t tm = matches[i];
         memory_score_parts_t tp = parts[i];
         scores[i] = scores[best];
         matches[i] = matches[best];
         parts[i] = parts[best];
         scores[best] = ts;
         matches[best] = tm;
         parts[best] = tp;
      }
   }

   /* Ordering is fixed.  Now reattach confidence from the full memory record
    * for display/trace surfaces — it must not have entered the score above. */
   for (int i = 0; i < count; i++)
      parts[i].confidence = matches[i].confidence;

   for (int i = 0; i < count; i++)
   {
      int rank = 1;
      for (int j = 0; j < count; j++)
      {
         if (i != j && matches[j].retrieval_score > matches[i].retrieval_score)
            rank++;
      }
      matches[i].hybrid_rank = rank;
   }

   return count < limit ? count : limit;
}

int memory_scope_matches(int64_t memory_id, const char *scope_type, const char *scope_value)
{
   if (!scope_type || !scope_type[0] || !scope_value || !scope_value[0])
      return 1;
   if (db2_memory_scope_matches(memory_id, scope_type, scope_value))
      return 1;
   if (strcmp(scope_type, "workspace") == 0 && db2_memory_workspace_matches(memory_id, scope_value))
      return 1;
   return 0;
}

int memory_filter_scope(memory_t *matches, int count, const char *scope_type,
                        const char *scope_value)
{
   if (!scope_type || !scope_type[0] || !scope_value || !scope_value[0])
      return count;
   int kept = 0;
   for (int i = 0; i < count; i++)
   {
      if (!memory_scope_matches(matches[i].id, scope_type, scope_value))
         continue;
      if (kept != i)
         matches[kept] = matches[i];
      kept++;
   }
   return kept;
}

int memory_find_facts_lexical_fallback(const char *query, const char *scope_type,
                                       const char *scope_value, int limit, memory_t *out, int max)
{
   if (!query || !query[0] || !out || max <= 0)
      return 0;
   if (limit <= 0)
      limit = 20;
   if (limit > max)
      limit = max;

   MEMORY_AUTOFREE memory_t *scratch = calloc(64, sizeof(*scratch));
   if (!scratch)
      return 0;
   int fetch_limit = limit * 4;
   int cap = 64;
   if (fetch_limit < limit)
      fetch_limit = limit;
   if (fetch_limit > cap)
      fetch_limit = cap;

   int count = 0;
   int got = memory_find_facts_like(query, fetch_limit, scratch, cap);
   if (got < 0)
      return got;
   got = memory_filter_scope(scratch, got, scope_type, scope_value);
   for (int i = 0; i < got && count < limit; i++)
      count = memory_append_unique(out, count, max, &scratch[i]);

   char norm_query[512];
   char tokens[24][64];
   normalize_key(query, norm_query, sizeof(norm_query));
   int token_count = memory_split_tokens(norm_query, tokens, 24);
   for (int t = 0; t < token_count && count < limit; t++)
   {
      if (!memory_is_signal_token(tokens[t]))
         continue;
      got = memory_find_facts_like(tokens[t], fetch_limit, scratch, cap);
      if (got < 0)
         return got;
      got = memory_filter_scope(scratch, got, scope_type, scope_value);
      for (int i = 0; i < got && count < limit; i++)
         count = memory_append_unique(out, count, max, &scratch[i]);
   }

   return count;
}

int memory_find_facts_visible_lexical_fallback(const char *query, const char *workspace,
                                               const char *project, int limit, memory_t *out,
                                               int max)
{
   if (!query || !query[0] || !out || max <= 0)
      return 0;
   if (limit <= 0)
      limit = 20;
   if (limit > max)
      limit = max;

   MEMORY_AUTOFREE memory_t *scratch = calloc(64, sizeof(*scratch));
   if (!scratch)
      return 0;
   int fetch_limit = limit * 4;
   int cap = 64;
   if (fetch_limit < limit)
      fetch_limit = limit;
   if (fetch_limit > cap)
      fetch_limit = cap;

   int count = 0;
   int got = memory_find_facts_like(query, fetch_limit, scratch, cap);
   if (got < 0)
      return got;
   for (int i = 0; i < got && count < cap; i++)
      count = memory_append_unique(scratch, count, cap, &scratch[i]);

   char norm_query[512];
   char tokens[24][64];
   normalize_key(query, norm_query, sizeof(norm_query));
   int token_count = memory_split_tokens(norm_query, tokens, 24);
   for (int t = 0; t < token_count && count < cap; t++)
   {
      if (!memory_is_signal_token(tokens[t]))
         continue;
      MEMORY_AUTOFREE memory_t *term_matches = calloc(64, sizeof(*term_matches));
      if (!term_matches)
         break;
      int term_cap = 64;
      got = memory_find_facts_like(tokens[t], fetch_limit, term_matches, term_cap);
      if (got < 0)
         return got;
      for (int i = 0; i < got && count < cap; i++)
         count = memory_append_unique(scratch, count, cap, &term_matches[i]);
   }

   int kept = 0;
   int scope_rank[64];
   for (int i = 0; i < count; i++)
   {
      int rank = memory_scope_visibility_rank(scratch[i].id, workspace, project);
      if (rank <= 0)
         continue;
      if (kept != i)
         scratch[kept] = scratch[i];
      scope_rank[kept] = rank;
      kept++;
   }

   for (int i = 0; i < kept - 1; i++)
   {
      int best = i;
      for (int j = i + 1; j < kept; j++)
         if (scope_rank[j] > scope_rank[best])
            best = j;
      if (best != i)
      {
         memory_t tmp_mem = scratch[i];
         int tmp_rank = scope_rank[i];
         scratch[i] = scratch[best];
         scope_rank[i] = scope_rank[best];
         scratch[best] = tmp_mem;
         scope_rank[best] = tmp_rank;
      }
   }

   if (kept > limit)
      kept = limit;
   for (int i = 0; i < kept; i++)
      out[i] = scratch[i];
   return kept;
}

int memory_query_is_code_like(const char *query)
{
   if (!query || !query[0])
      return 0;
   int upper = 0;
   int lower = 0;
   int has_space = 0;
   int camel_case = 0;
   unsigned char prev = '\0';
   for (const unsigned char *p = (const unsigned char *)query; *p; p++)
   {
      if (isspace(*p))
         has_space = 1;
      if (*p == '_' || *p == '/' || *p == '.' || *p == ':' || *p == '-' || *p == '#')
         return 1;
      if (prev && islower(prev) && isupper(*p))
         camel_case = 1;
      if (isupper(*p))
         upper++;
      else if (islower(*p))
         lower++;
      prev = *p;
   }
   if (camel_case)
      return 1;
   return !has_space && upper > 1 && lower > 1;
}

/* Code-shaped queries used to hit separate lexical indexes. With direct text
 * recall routed through pgvector, code_matches now runs the same
 * memory-level vector search; the caller still tags hits with MEM_SOURCE_CODE
 * so retrieval-source diagnostics keep working. */
static int memory_collect_code_matches(const char *query, int fetch_limit, memory_t *out, int count,
                                       int max)
{
   if (!query || !query[0] || !out || count >= max || fetch_limit <= 0)
      return count;
   MEMORY_AUTOFREE memory_t *scratch = calloc(64, sizeof(*scratch));
   if (!scratch)
      return count;
   int cap = 64;
   config_t code_cfg;
   config_load(&code_cfg);
   const char *embed_cmd = config_embedding_command(&code_cfg, NULL);
   int got = memory_collect_memory_matches_via_vector(query, embed_cmd, fetch_limit, scratch, cap);
   for (int i = 0; i < got && count < max; i++)
      count = memory_append_unique(out, count, max, &scratch[i]);
   return count;
}

/* Populate an expanded_terms[][] buffer from semantically-near memory
 * keys.  Parallels memory_expand_query_terms but uses the embedding
 * space instead of a hardcoded synonym table.
 *
 * Walks the top-k vector hits, extracts signal tokens from each hit's
 * normalised key, and appends them to `expanded` up to `max_terms`.
 * Caller is responsible for seeding `expanded` with the base tokens
 * first — this function only augments. Returns the new count, or the
 * original count when semantic expansion is unavailable. */
/* Search pgvector for unit points matching the embedded query, group hits by
 * parent memory_id, require ≥2 corroborating units per memory, and fill
 * `out` with the parent memory_t rows ordered by hit_count DESC then best
 * score DESC. Replaces the old unit-text lookup path with a vector-native
 * corroboration filter. */
int memory_collect_unit_matches_via_vector(const char *query, const char *embed_cmd, int limit,
                                           memory_t *out, int max)
{
   if (!query || !query[0] || !out || limit <= 0 || max <= 0)
      return 0;
   const char *effective_cmd = memory_effective_embedding_cmd(embed_cmd);
   float qvec[EMBED_MAX_DIM];
   int qdim = memory_embed_text_runtime(query, effective_cmd, qvec, EMBED_MAX_DIM);
   if (qdim <= 0)
      return 0;

   /* Over-fetch unit hits so the corroboration filter (≥2 distinct units per
    * memory) has room to find groups. 4× the requested limit, capped at 256. */
   int fetch_k = limit * 4;
   if (fetch_k > 256)
      fetch_k = 256;
   int64_t point_ids[256];
   double scores[256];
   int hits =
       pgvec_memory_vector_search_record_type("unit", qvec, qdim, fetch_k, point_ids, scores, 256);
   if (hits <= 0)
      return 0;

   /* Group by parent memory_id. Linear scan is fine — fetch_k ≤ 256 and the
    * number of distinct memories is even smaller. */
   struct
   {
      int64_t memory_id;
      int hit_count;
      double best_score;
   } groups[64];
   int n_groups = 0;
   for (int i = 0; i < hits; i++)
   {
      int64_t unit_id = point_ids[i] - PGVEC_MEMORY_VECTOR_UNIT_ID_OFFSET;
      if (unit_id <= 0)
         continue;
      memory_t parent;
      if (db2_memory_get_by_unit_id(unit_id, &parent) != 0)
         continue;
      int gi = -1;
      for (int g = 0; g < n_groups; g++)
         if (groups[g].memory_id == parent.id)
         {
            gi = g;
            break;
         }
      if (gi < 0)
      {
         if (n_groups >= (int)(sizeof(groups) / sizeof(groups[0])))
            continue;
         gi = n_groups++;
         groups[gi].memory_id = parent.id;
         groups[gi].hit_count = 0;
         groups[gi].best_score = scores[i];
      }
      groups[gi].hit_count++;
      if (scores[i] > groups[gi].best_score)
         groups[gi].best_score = scores[i];
   }

   /* Sort by hit_count DESC, then best_score DESC. n_groups is small. */
   for (int i = 0; i < n_groups - 1; i++)
   {
      int best = i;
      for (int j = i + 1; j < n_groups; j++)
      {
         if (groups[j].hit_count > groups[best].hit_count ||
             (groups[j].hit_count == groups[best].hit_count &&
              groups[j].best_score > groups[best].best_score))
            best = j;
      }
      if (best != i)
      {
         typeof(groups[0]) tmp = groups[i];
         groups[i] = groups[best];
         groups[best] = tmp;
      }
   }

   int n = 0;
   for (int g = 0; g < n_groups && n < max; g++)
   {
      if (groups[g].hit_count < 2)
         continue;
      memory_t mem;
      if (memory_fetch_row_by_id(groups[g].memory_id, &mem) == 0)
         out[n++] = mem;
   }
   return n;
}

/* Embed `query`, search the pgvector memory index, and fill `out` with the
 * resulting memory_t rows in score order. Dense memory retrieval lives in
 * pgvector inside DB2 (no separate tier). */
int memory_collect_memory_matches_via_vector(const char *query, const char *embed_cmd, int limit,
                                             memory_t *out, int max)
{
   if (!query || !query[0] || !out || limit <= 0 || max <= 0)
      return 0;
   const char *effective_cmd = memory_effective_embedding_cmd(embed_cmd);
   float qvec[EMBED_MAX_DIM];
   int qdim = memory_embed_text_runtime(query, effective_cmd, qvec, EMBED_MAX_DIM);
   if (qdim <= 0)
      return 0;

   int fetch_k = limit < max ? limit : max;
   if (fetch_k > 64)
      fetch_k = 64;
   int64_t ids[64];
   double scores[64];
   int hits = pgvec_memory_vector_search_record_type("memory", qvec, qdim, fetch_k, ids, scores,
                                                     (int)(sizeof(ids) / sizeof(ids[0])));
   if (hits <= 0)
      return 0;

   int n = 0;
   for (int i = 0; i < hits && n < max; i++)
   {
      memory_t mem;
      if (memory_fetch_row_by_id(ids[i], &mem) == 0)
         out[n++] = mem;
   }
   return n;
}

/* Two-lane retrieval: lane membership communicated from memory_generate_candidates
 * to the rerank callers (memory_recall, memory_find_facts_visible).  Written on
 * every memory_generate_candidates call (zeroed when lanes are disabled). */
int64_t s_lane_summary_ids[96];
int s_lane_summary_count;
int64_t s_lane_fact_ids[96];
int s_lane_fact_count;

/* Parse a comma-separated kinds string into an array of pointers into `buf`.
 * Returns the number of non-empty entries written to `ptrs[]`. */
static int memory_parse_kinds_csv(const char *csv, char buf[][16], const char **ptrs, int max)
{
   if (!csv || !csv[0] || !buf || !ptrs || max <= 0)
      return 0;
   int n = 0;
   const char *p = csv;
   while (*p && n < max)
   {
      while (*p == ',' || *p == ' ')
         p++;
      if (!*p)
         break;
      int i = 0;
      while (*p && *p != ',' && i < 15)
         buf[n][i++] = *p++;
      buf[n][i] = '\0';
      if (buf[n][0])
      {
         ptrs[n] = buf[n];
         n++;
      }
   }
   return n;
}

/* Kind-filtered variant: embeds `query`, issues a pgvector search restricted to
 * memories whose `kind` field matches one of the entries in `kinds[0..n_kinds)`,
 * and fills `out` with the resulting rows. A NULL/empty kinds list falls back to
 * the unfiltered path. */
static int memory_collect_memory_matches_via_vector_with_kinds(const char *query,
                                                               const char *embed_cmd, int limit,
                                                               const char *const *kinds,
                                                               int n_kinds, memory_t *out, int max)
{
   if (!query || !query[0] || !out || limit <= 0 || max <= 0)
      return 0;
   if (!kinds || n_kinds <= 0)
      return memory_collect_memory_matches_via_vector(query, embed_cmd, limit, out, max);

   const char *effective_cmd = memory_effective_embedding_cmd(embed_cmd);
   float qvec[EMBED_MAX_DIM];
   int qdim = memory_embed_text_runtime(query, effective_cmd, qvec, EMBED_MAX_DIM);
   if (qdim <= 0)
      return 0;

   int fetch_k = limit < max ? limit : max;
   if (fetch_k > 64)
      fetch_k = 64;
   int64_t ids[64];
   double scores[64];
   int hits = pgvec_memory_vector_search_with_kinds(qvec, qdim, kinds, n_kinds, fetch_k, ids,
                                                    scores, (int)(sizeof(ids) / sizeof(ids[0])));
   if (hits <= 0)
      return 0;

   int n = 0;
   for (int i = 0; i < hits && n < max; i++)
   {
      memory_t mem;
      if (memory_fetch_row_by_id(ids[i], &mem) == 0)
         out[n++] = mem;
   }
   return n;
}

/* Post-rerank lane floor: after `matches[0..count)` have been sorted by score,
 * ensure at least `floor_n` of them belong to lane `lane_ids[0..lane_count)`.
 * If fewer than `floor_n` lane members are already in the top `total` results,
 * swap in the highest-ranked missing lane member from the tail.
 *
 * Returns the new effective top-window size after promotion.  Callers that
 * apply floors for multiple lanes in sequence MUST pass the return value of
 * each call as `total` to the next call — this prevents the second floor from
 * displacing items promoted by the first. */
int memory_apply_lane_floor(memory_t *matches, int count, const int64_t *lane_ids, int lane_count,
                            int floor_n, int total)
{
   if (floor_n <= 0 || count <= 0 || lane_count <= 0 || total <= 0)
      return total;
   if (total > count)
      total = count;

   /* Count how many of the top `total` are already from this lane. */
   int lane_in_top = 0;
   for (int i = 0; i < total; i++)
   {
      for (int j = 0; j < lane_count; j++)
         if (matches[i].id == lane_ids[j])
         {
            lane_in_top++;
            break;
         }
   }
   if (lane_in_top >= floor_n)
      return total;

   /* Scan the tail (positions >= total) for lane members and swap them in. */
   int needed = floor_n - lane_in_top;
   for (int tail = total; tail < count && needed > 0; tail++)
   {
      int is_lane = 0;
      for (int j = 0; j < lane_count; j++)
         if (matches[tail].id == lane_ids[j])
         {
            is_lane = 1;
            break;
         }
      if (!is_lane)
         continue;
      /* Swap the last top-`total` slot with this tail entry. */
      int victim = total - 1;
      memory_t tmp = matches[victim];
      matches[victim] = matches[tail];
      matches[tail] = tmp;
      total--; /* the swapped-in lane member is now "protected" — shrink top window */
      needed--;
   }
   return total;
}

static int memory_expand_query_terms_semantic(const char *query, const char *embed_cmd, int k,
                                              char expanded[][64], int count, int max_terms)
{
   if (!query || !query[0] || !expanded || count >= max_terms)
      return count;
   const char *effective_cmd = memory_effective_embedding_cmd(embed_cmd);

   float qvec[EMBED_MAX_DIM];
   int qdim = memory_embed_text_runtime(query, effective_cmd, qvec, EMBED_MAX_DIM);
   if (qdim <= 0)
      return count;

   int fetch_k = (k > 0 && k < 16) ? k : 5;
   int64_t ids[16];
   double scores[16];
   int hits = pgvec_memory_vector_search_record_type("memory", qvec, qdim, fetch_k, ids, scores,
                                                     (int)(sizeof(ids) / sizeof(ids[0])));
   if (hits <= 0)
      return count;

   for (int i = 0; i < hits && count < max_terms; i++)
   {
      memory_t mem;
      if (memory_fetch_row_by_id(ids[i], &mem) != 0 || !mem.key[0])
         continue;
      char key_norm[256];
      normalize_key(mem.key, key_norm, sizeof(key_norm));
      char ktokens[8][64];
      int kn = memory_split_tokens(key_norm, ktokens, 8);
      for (int j = 0; j < kn && count < max_terms; j++)
      {
         if (strlen(ktokens[j]) < 3)
            continue;
         if (!memory_is_signal_token(ktokens[j]))
            continue;
         count = memory_add_expanded_term(expanded, count, max_terms, ktokens[j]);
      }
   }
   return count;
}

/* Semantic query expansion: find semantically similar memory keys via pgvector
 * and append their signal tokens to the lexical query string. */
static int memory_expand_query_semantic(const char *query, const char *embed_cmd, int k,
                                        char *lexical_out, size_t lexical_out_len)
{
   if (!query || !query[0] || !lexical_out || lexical_out_len == 0)
      return 0;
   const char *effective_cmd = memory_effective_embedding_cmd(embed_cmd);

   float qvec[EMBED_MAX_DIM];
   int qdim = memory_embed_text_runtime(query, effective_cmd, qvec, EMBED_MAX_DIM);
   if (qdim <= 0)
      return 0;

   int fetch_k = (k > 0 && k < 16) ? k : 5;
   int64_t ids[16];
   double scores[16];
   int hits = pgvec_memory_vector_search_record_type("memory", qvec, qdim, fetch_k, ids, scores,
                                                     (int)(sizeof(ids) / sizeof(ids[0])));
   if (hits < 0)
      return memory_semantic_query_unavailable();
   if (hits <= 0)
      return 0;

   /* Append signal tokens from the top-k keys to the lexical query. */
   size_t used = strlen(lexical_out);
   for (int i = 0; i < hits; i++)
   {
      memory_t mem;
      if (memory_fetch_row_by_id(ids[i], &mem) != 0 || !mem.key[0])
         continue;
      char key_norm[256];
      normalize_key(mem.key, key_norm, sizeof(key_norm));
      char ktokens[8][64];
      int kn = memory_split_tokens(key_norm, ktokens, 8);
      for (int j = 0; j < kn && used + 72 < lexical_out_len; j++)
      {
         if (memory_is_signal_token(ktokens[j]))
            used += snprintf(lexical_out + used, lexical_out_len - used, " OR %s", ktokens[j]);
      }
   }
   return 0;
}

int memory_collect_variant_candidates(const char *raw_query, const char *norm_variant,
                                      memory_query_intent_t intent, int fetch_limit, memory_t *out,
                                      int count, int max, memory_candidate_source_t *source_stats,
                                      int *source_stats_count)
{
   if (!raw_query || !raw_query[0] || !norm_variant || !norm_variant[0] || !out || max <= 0)
      return count;

   char qtokens[32][64];
   char expanded_terms[48][64];
   int qtoken_count = memory_split_tokens(norm_variant, qtokens, 32);
   char signal_query[256];
   memory_build_signal_query(qtokens, qtoken_count, signal_query, sizeof(signal_query));

   char expanded_signal[1024];
   memory_expand_synonyms(signal_query[0] ? signal_query : norm_variant, expanded_signal,
                          sizeof(expanded_signal));

   /* Query expansion modes:
    *   "lexical"  (default) — hardcoded synonym table only
    *   "semantic"          — embedding-space neighbors only
    *   "hybrid"            — both (expansion budget is shared)
    * Semantic mode augments BOTH the lexical query (for unit/memory matches)
    * AND the expanded_terms[][] buffer (for alias/entity/summary/chunk paths),
    * so the two retrieval legs see the same expansion footprint. */
   config_t qe_cfg;
   config_load(&qe_cfg);
   const char *qe_mode =
       qe_cfg.memory_query_expansion_mode[0] ? qe_cfg.memory_query_expansion_mode : "lexical";
   const int qe_semantic =
       (strcmp(qe_mode, "semantic") == 0 || strcmp(qe_mode, "hybrid") == 0) ? 1 : 0;
   const int qe_lexical =
       (strcmp(qe_mode, "lexical") == 0 || strcmp(qe_mode, "hybrid") == 0) ? 1 : 0;

   if (qe_semantic)
   {
      const char *embed_cmd = config_embedding_command(&qe_cfg, NULL);
      int k = qe_cfg.memory_query_expansion_k > 0 ? qe_cfg.memory_query_expansion_k : 5;
      if (memory_expand_query_semantic(signal_query[0] ? signal_query : norm_variant, embed_cmd, k,
                                       expanded_signal, sizeof(expanded_signal)) < 0)
         return -1;
   }

   const char *lexical_query = expanded_signal;
   const char *structured_query = signal_query[0] ? signal_query : norm_variant;
   int expanded_count = 0;
   if (qe_lexical)
      expanded_count =
          memory_expand_query_terms(norm_variant, qtokens, qtoken_count, expanded_terms, 48);
   else
   {
      /* semantic-only: seed the base tokens then augment with embedding-
       * derived signal terms so the alias/entity/summary/chunk/unit/
       * temporal legs see the same neighborhood as the lexical query. */
      for (int i = 0; i < qtoken_count && expanded_count < 48; i++)
         expanded_count = memory_add_expanded_term(expanded_terms, expanded_count, 48, qtokens[i]);
   }
   if (qe_semantic)
   {
      const char *embed_cmd = config_embedding_command(&qe_cfg, NULL);
      int k = qe_cfg.memory_query_expansion_k > 0 ? qe_cfg.memory_query_expansion_k : 5;
      expanded_count =
          memory_expand_query_terms_semantic(signal_query[0] ? signal_query : norm_variant,
                                             embed_cmd, k, expanded_terms, expanded_count, 48);
   }

   {
      int before = count;
      count = memory_collect_unit_matches(lexical_query, fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_UNIT);
   }

   {
      MEMORY_AUTOFREE memory_t *lexical_scratch = calloc(64, sizeof(*lexical_scratch));
      if (!lexical_scratch)
         return count;
      int lexical_cap = 64;
      const char *lexical_embed_cmd = config_embedding_command(&qe_cfg, NULL);
      config_t lanes_cfg;
      config_load(&lanes_cfg);
      if (lanes_cfg.memory_recall_lanes_enabled)
      {
         char sum_buf[16][16], fact_buf[16][16];
         const char *sum_ptrs[16], *fact_ptrs[16];
         int n_sum = memory_parse_kinds_csv(lanes_cfg.memory_recall_lanes_summary_kinds[0]
                                                ? lanes_cfg.memory_recall_lanes_summary_kinds
                                                : "episode",
                                            sum_buf, sum_ptrs, 16);
         int n_fact = memory_parse_kinds_csv(lanes_cfg.memory_recall_lanes_fact_kinds[0]
                                                 ? lanes_cfg.memory_recall_lanes_fact_kinds
                                                 : "fact,preference",
                                             fact_buf, fact_ptrs, 16);
         int k_sum = lanes_cfg.memory_recall_lanes_k_summary > 0
                         ? lanes_cfg.memory_recall_lanes_k_summary
                         : 40;
         int k_fact =
             lanes_cfg.memory_recall_lanes_k_fact > 0 ? lanes_cfg.memory_recall_lanes_k_fact : 40;

         /* Summary lane */
         int got_sum = memory_collect_memory_matches_via_vector_with_kinds(
             lexical_query, lexical_embed_cmd, k_sum, sum_ptrs, n_sum, lexical_scratch,
             lexical_cap);
         s_lane_summary_count = got_sum < 96 ? got_sum : 96;
         for (int s = 0; s < s_lane_summary_count; s++)
            s_lane_summary_ids[s] = lexical_scratch[s].id;
         for (int s = 0; s < got_sum && count < max; s++)
         {
            int before = count;
            count = memory_append_unique(out, count, max, &lexical_scratch[s]);
            memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                          MEM_SOURCE_LEXICAL);
         }

         /* Fact lane — reuse scratch */
         int got_fact = memory_collect_memory_matches_via_vector_with_kinds(
             lexical_query, lexical_embed_cmd, k_fact, fact_ptrs, n_fact, lexical_scratch,
             lexical_cap);
         s_lane_fact_count = got_fact < 96 ? got_fact : 96;
         for (int s = 0; s < s_lane_fact_count; s++)
            s_lane_fact_ids[s] = lexical_scratch[s].id;
         for (int s = 0; s < got_fact && count < max; s++)
         {
            int before = count;
            count = memory_append_unique(out, count, max, &lexical_scratch[s]);
            memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                          MEM_SOURCE_LEXICAL);
         }
         aimee_log(LOG_DEBUG, "memory", "two-lane: summary=%d fact=%d merged_total=%d", got_sum,
                   got_fact, count);
      }
      else
      {
         s_lane_summary_count = 0;
         s_lane_fact_count = 0;
         int got = memory_collect_memory_matches_via_vector(
             lexical_query, lexical_embed_cmd, fetch_limit, lexical_scratch, lexical_cap);
         for (int s = 0; s < got && count < max; s++)
         {
            int before = count;
            count = memory_append_unique(out, count, max, &lexical_scratch[s]);
            memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                          MEM_SOURCE_LEXICAL);
         }
      }
   }

   if (memory_query_is_code_like(raw_query) || memory_query_is_code_like(structured_query))
   {
      int before = count;
      count = memory_collect_code_matches(structured_query, fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_CODE);
   }

   {
      int before = count;
      count = memory_collect_alias_matches(structured_query, fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_ALIAS);
   }
   for (int i = 0; i < expanded_count && count < max; i++)
   {
      if (strlen(expanded_terms[i]) < 3)
         continue;
      int before = count;
      count = memory_collect_alias_matches(expanded_terms[i], fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_ALIAS);
      before = count;
      count = memory_collect_entity_matches(expanded_terms[i], fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_ENTITY);
      before = count;
      count = memory_collect_summary_matches(expanded_terms[i], fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_SUMMARY);
      before = count;
      count = memory_collect_event_frame_matches(expanded_terms[i], fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_EVENT);
      before = count;
      count = memory_collect_chunk_matches(expanded_terms[i], fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_CHUNK);
      before = count;
      count = memory_collect_unit_matches(expanded_terms[i], fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_UNIT);
      if (intent == MEM_QUERY_TEMPORAL)
      {
         before = count;
         count = memory_collect_temporal_matches(expanded_terms[i], fetch_limit, out, count, max);
         memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                       MEM_SOURCE_TEMPORAL);
      }
   }

   if (intent == MEM_QUERY_ENTITY || intent == MEM_QUERY_GENERAL)
   {
      int before = count;
      count = memory_collect_entity_matches(structured_query, fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_ENTITY);
   }
   {
      int before = count;
      count = memory_collect_summary_matches(structured_query, fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_SUMMARY);
   }
   {
      int before = count;
      count = memory_collect_event_frame_matches(norm_variant, fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_EVENT);
   }
   {
      int before = count;
      count = memory_collect_chunk_matches(lexical_query, fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_CHUNK);
   }
   {
      int before = count;
      count = memory_collect_unit_matches(lexical_query, fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_UNIT);
   }
   if (intent == MEM_QUERY_TEMPORAL)
   {
      int before = count;
      count = memory_collect_temporal_matches(norm_variant, fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_TEMPORAL);
   }

   return count;
}
