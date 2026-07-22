/* memory_assemble.c: XML/text serialization helpers, candidate scoring,
 * memory_assemble_context + _explain + _ws, and the context cache. */
#include "aimee.h"
#include "memory.h"
#include "memory_assemble_util.h"
#include "memory_context_internal.h"
#include "memory_ontology.h"
#include "cJSON.h"
#include "log.h"
#include "platform_process.h"
#include "workspace.h"
#include "kb.h"
#include "db1_optional.h"
#if !defined(AIMEE_DB2_DISABLED)
#include "db2/anti_patterns.h"
#include "db2/entity_edges.h"
#include "db2/memory_query.h"
#include "db2/memory_relations.h"
#include "db2/rules.h"
#endif
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#if defined(AIMEE_DB2_DISABLED)
static char *memory_empty_context(void)
{
   const char *body = "# Memory Context";
   char *buf = malloc(strlen(body) + 1);
   if (!buf)
      return NULL;
   snprintf(buf, strlen(body) + 1, "%s", body);
   return buf;
}

char *memory_assemble_context(const char *task_hint)
{
   (void)task_hint;
   return memory_empty_context();
}

char *memory_assemble_context_ws(const char *task_hint, const char *workspace)
{
   (void)workspace;
   return memory_assemble_context(task_hint);
}

char *memory_assemble_context_explain(const char *task_hint,
                                      context_assemble_explain_entry_t *explain, int *explain_count,
                                      int explain_max, context_budget_metrics_t *metrics)
{
   (void)explain;
   (void)explain_max;
   if (explain_count)
      *explain_count = 0;
   if (metrics)
   {
      metrics->budget_tokens = 0;
      metrics->used_tokens = 0;
      metrics->rejected_for_budget = 0;
   }
   return memory_assemble_context(task_hint);
}

char *cache_input_hash(char *buf, size_t buf_len)
{
   if (buf && buf_len > 0)
      snprintf(buf, buf_len, "db2-disabled");
   return buf;
}

#else

/* xml_escape_text and context_xml_tag_for_header now live in
 * memory_assemble_util.h (static inline, unit-tested). */

static int context_split_tokens(const char *norm, char tokens[][64], int max_tokens)
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

static int append_section(char *buf, int pos, int cap, const char *header,
                          const db2_memory_kv_row_t *rows, int row_count, int max_chars,
                          int max_items)
{
   int section_start = pos;
   pos += snprintf(buf + pos, cap - pos, "\n## %s\n", header);

   int items = 0;
   int section_len = 0;

   for (int i = 0; i < row_count && items < max_items; i++)
   {
      const char *key = rows[i].key;
      const char *content = rows[i].content;
      if (!key[0])
         continue;

      const char *text = content[0] ? content : key;

      char truncated[MAX_MEM_CONTENT_LEN + 8];
      if ((int)strlen(text) > MAX_MEM_CONTENT_LEN)
      {
         snprintf(truncated, sizeof(truncated), "%.*s...", MAX_MEM_CONTENT_LEN, text);
         text = truncated;
      }

      int line_len = (int)strlen(text) + 4;
      if (section_len + line_len > max_chars)
         break;

      pos += snprintf(buf + pos, cap - pos, "- %s\n", text);
      section_len += line_len;
      items++;
   }

   if (items == 0)
      pos = section_start;

   return pos;
}

typedef struct
{
   int scope_rank;
   int ordinal;
   char text[MAX_MEM_CONTENT_LEN + 8];
} scoped_section_item_t;

static int compare_scoped_section_item(const void *a, const void *b)
{
   const scoped_section_item_t *lhs = a;
   const scoped_section_item_t *rhs = b;
   if (lhs->scope_rank != rhs->scope_rank)
      return rhs->scope_rank - lhs->scope_rank;
   return lhs->ordinal - rhs->ordinal;
}

static void memory_scope_labels_for_cwd(const char *workspace_hint, char *workspace_out,
                                        size_t workspace_len, char *project_out, size_t project_len)
{
   char cwd[MAX_PATH_LEN];
   const char *workspace = workspace_hint;

   if (!getcwd(cwd, sizeof(cwd)))
      cwd[0] = '\0';

   /* Prefer a path-independent repository identity (canonical remote URL) so
    * recall matches across clones/machines instead of keying on the local
    * checkout path. Falls through to the legacy path/basename labels below when
    * cwd is not inside a resolvable git repo. An explicit workspace_hint still
    * wins for the workspace label. */
   if (cwd[0])
   {
      char repo_project[MAX_PATH_LEN];
      char repo_workspace[MAX_PATH_LEN];
      if (workspace_repo_identity(cwd, repo_project, sizeof(repo_project), repo_workspace,
                                  sizeof(repo_workspace)) == 0)
      {
         if (workspace_out && workspace_len > 0)
            snprintf(workspace_out, workspace_len, "%s",
                     (workspace_hint && workspace_hint[0]) ? workspace_hint : repo_workspace);
         if (project_out && project_len > 0)
            snprintf(project_out, project_len, "%s", repo_project);
         return;
      }
   }

   if (!workspace || !workspace[0])
   {
      workspace = cwd;
   }

   if (workspace_out && workspace_len > 0)
      snprintf(workspace_out, workspace_len, "%s", workspace ? workspace : "");
   if (project_out && project_len > 0)
      project_out[0] = '\0';

   if (!project_out || project_len == 0)
      return;

   /* Project label tracks the repo root's basename so it matches
    * memory_tag_project(basename(workspace_active_root(cwd))) — the convention
    * encoded in the scope-tagging tests and the only one stable across
    * workspace sub-directories.  Falls back to cwd / workspace basename when
    * the cwd isn't inside any git repo or configured workspace. */
   char project_source_buf[MAX_PATH_LEN];
   project_source_buf[0] = '\0';
   const char *project_source = NULL;
   if (cwd[0] &&
       workspace_active_root(NULL, cwd, project_source_buf, sizeof(project_source_buf)) == 0 &&
       project_source_buf[0])
   {
      project_source = project_source_buf;
   }
   if (!project_source || !project_source[0])
      project_source = cwd[0] ? cwd : workspace;
   if (!project_source || !project_source[0])
      return;

   const char *slash = strrchr(project_source, '/');
   const char *project_name = slash ? slash + 1 : project_source;
   snprintf(project_out, project_len, "%s", project_name ? project_name : "");
}

static int append_scoped_section(char *buf, int pos, int cap, const char *header,
                                 const db2_memory_id_kv_row_t *src_rows, int src_count,
                                 const char *workspace, const char *project, int include_visible,
                                 int max_chars, int max_items)
{
   scoped_section_item_t items[64];
   int item_count = 0;

   for (int i = 0; i < src_count && item_count < (int)(sizeof(items) / sizeof(items[0])); i++)
   {
      int scope_rank = memory_scope_visibility_rank(src_rows[i].id, workspace, project);
      if (include_visible)
      {
         if (scope_rank <= 0)
            continue;
      }
      else if (scope_rank > 0)
      {
         continue;
      }

      const char *key = src_rows[i].key;
      const char *content = src_rows[i].content;
      if (!key[0])
         continue;

      const char *text = content[0] ? content : key;
      snprintf(items[item_count].text, sizeof(items[item_count].text), "%s", text);
      items[item_count].scope_rank = scope_rank;
      items[item_count].ordinal = i + 1;
      item_count++;
   }

   if (item_count == 0)
      return pos;

   qsort(items, (size_t)item_count, sizeof(items[0]), compare_scoped_section_item);

   int section_start = pos;
   pos += snprintf(buf + pos, cap - pos, "\n## %s\n", header);

   int emitted = 0;
   int section_len = 0;
   for (int i = 0; i < item_count && emitted < max_items; i++)
   {
      const char *text = items[i].text;
      char truncated[MAX_MEM_CONTENT_LEN + 8];
      if ((int)strlen(text) > MAX_MEM_CONTENT_LEN)
      {
         snprintf(truncated, sizeof(truncated), "%.*s...", MAX_MEM_CONTENT_LEN, text);
         text = truncated;
      }

      int line_len = (int)strlen(text) + 4;
      if (section_len + line_len > max_chars)
         break;

      pos += snprintf(buf + pos, cap - pos, "- %s\n", text);
      section_len += line_len;
      emitted++;
   }

   if (emitted == 0)
      pos = section_start;
   return pos;
}

/* Emit a candidate into the buffer, respecting section budget.
 * Returns number of chars written, or 0 if budget exceeded. */
static int emit_candidate(char *buf, int pos, int cap, const context_candidate_t *c,
                          const char *xml_tag, int *section_len, int max_chars)
{
   const char *text = (c->content[0]) ? c->content : c->key;

   char truncated[MAX_MEM_CONTENT_LEN + 8];
   if ((int)strlen(text) > MAX_MEM_CONTENT_LEN)
   {
      snprintf(truncated, sizeof(truncated), "%.*s...", MAX_MEM_CONTENT_LEN, text);
      text = truncated;
   }

   char escaped[MAX_MEM_CONTENT_LEN * 2];
   xml_escape_text(text, escaped, sizeof(escaped));
   int line_len = (int)strlen(escaped) + 96;
   if (*section_len + line_len > max_chars)
      return 0;

   int written = snprintf(buf + pos, cap - pos,
                          "<%s tier=\"%s\" confidence=\"%.2f\" score=\"%.2f\">%s</%s>\n",
                          xml_tag ? xml_tag : "memory_item", c->tier[0] ? c->tier : "?",
                          c->confidence, c->score, escaped, xml_tag ? xml_tag : "memory_item");
   *section_len += line_len;
   return written;
}

static int append_negative_context_block(char *buf, int pos, int cap, const char *task_hint,
                                         const context_candidate_t *candidates, int cand_count)
{
   if (!buf || cap <= pos || !task_hint || !task_hint[0] || !candidates || cand_count <= 0)
      return pos;

   int section_start = pos;
   int items = 0;
   pos += snprintf(buf + pos, cap - pos, "\n<avoid_these_patterns>\n");

   int64_t seen_old[12];
   int seen_old_count = 0;
   int max_scan = cand_count < 10 ? cand_count : 10;
   for (int i = 0; i < max_scan && items < 4; i++)
   {
      db2_memory_supersede_pair_t pair;
      if (!db2_memory_supersede_lookup(candidates[i].id, &pair))
         continue;
      int dup = 0;
      for (int j = 0; j < seen_old_count; j++)
         if (seen_old[j] == pair.old_id)
            dup = 1;
      if (dup || seen_old_count >= (int)(sizeof(seen_old) / sizeof(seen_old[0])))
         continue;
      char old_esc[2048];
      char new_esc[2048];
      xml_escape_text(pair.old_content, old_esc, sizeof(old_esc));
      xml_escape_text(pair.new_content, new_esc, sizeof(new_esc));
      pos += snprintf(buf + pos, cap - pos,
                      "  <outdated_memory confidence=\"%.2f\" expired_at=\"%s\">"
                      "Outdated: %s</outdated_memory>\n"
                      "  <replacement_memory confidence=\"%.2f\">Use instead: %s"
                      "</replacement_memory>\n",
                      pair.old_confidence, pair.valid_until, old_esc, pair.new_confidence, new_esc);
      seen_old[seen_old_count++] = pair.old_id;
      items++;
   }

   char norm_hint[256];
   normalize_key(task_hint, norm_hint, sizeof(norm_hint));
   char tokens[16][64];
   int token_count = context_split_tokens(norm_hint, tokens, 16);

   /* Fetch the full anti-pattern catalog from DB1 once, then match each
    * task-hint token client-side. The catalog is bounded (operator-curated
    * + extracted patterns); a full scan is cheaper than 16 round-trips. */
   anti_pattern_t patterns[64];
   int pattern_count = db2_anti_pattern_list(patterns, 64);
   char seen_pattern[4][256];
   int seen_pattern_count = 0;
   for (int i = 0; i < token_count && items < 6; i++)
   {
      if (strlen(tokens[i]) < 4)
         continue;
      char tok_lower[96];
      size_t tn = 0;
      for (size_t k = 0; tokens[i][k] && tn + 1 < sizeof(tok_lower); k++)
         tok_lower[tn++] = (char)tolower((unsigned char)tokens[i][k]);
      tok_lower[tn] = '\0';

      /* Highest-confidence first; rows already arrive in that order. */
      for (int p = 0; p < pattern_count; p++)
      {
         char pat_lower[512];
         char desc_lower[1024];
         size_t l = 0;
         for (size_t k = 0; patterns[p].pattern[k] && l + 1 < sizeof(pat_lower); k++)
            pat_lower[l++] = (char)tolower((unsigned char)patterns[p].pattern[k]);
         pat_lower[l] = '\0';
         l = 0;
         for (size_t k = 0; patterns[p].description[k] && l + 1 < sizeof(desc_lower); k++)
            desc_lower[l++] = (char)tolower((unsigned char)patterns[p].description[k]);
         desc_lower[l] = '\0';

         if (!strstr(pat_lower, tok_lower) && !strstr(desc_lower, tok_lower))
            continue;

         int dup = 0;
         for (int j = 0; j < seen_pattern_count; j++)
         {
            if (strcmp(seen_pattern[j], patterns[p].pattern) == 0)
            {
               dup = 1;
               break;
            }
         }
         if (dup)
            continue;

         char escaped[2048];
         const char *body =
             patterns[p].description[0] ? patterns[p].description : patterns[p].pattern;
         xml_escape_text(body, escaped, sizeof(escaped));
         pos += snprintf(buf + pos, cap - pos,
                         "  <anti_pattern confidence=\"%.2f\">Avoid: %s</anti_pattern>\n",
                         patterns[p].confidence, escaped);
         snprintf(seen_pattern[seen_pattern_count], sizeof(seen_pattern[seen_pattern_count]), "%s",
                  patterns[p].pattern);
         seen_pattern_count++;
         items++;
         break; /* one match per token */
      }
   }

   if (items == 0)
      return section_start;

   pos += snprintf(buf + pos, cap - pos, "</avoid_these_patterns>\n");
   return pos;
}

/* Compare candidates by score descending */
static int cmp_candidates(const void *a, const void *b)
{
   const context_candidate_t *ca = (const context_candidate_t *)a;
   const context_candidate_t *cb = (const context_candidate_t *)b;
   if (cb->scope_rank != ca->scope_rank)
      return cb->scope_rank - ca->scope_rank;
   if (cb->tier_priority != ca->tier_priority)
      return cb->tier_priority - ca->tier_priority;
   if (cb->score > ca->score)
      return 1;
   if (cb->score < ca->score)
      return -1;
   if (cb->score_per_token > ca->score_per_token)
      return 1;
   if (cb->score_per_token < ca->score_per_token)
      return -1;
   return 0;
}

/* Compare candidates by score_per_token descending (used in token-budget mode) */
static int cmp_candidates_by_density(const void *a, const void *b)
{
   const context_candidate_t *ca = (const context_candidate_t *)a;
   const context_candidate_t *cb = (const context_candidate_t *)b;
   if (cb->scope_rank != ca->scope_rank)
      return cb->scope_rank - ca->scope_rank;
   if (cb->tier_priority != ca->tier_priority)
      return cb->tier_priority - ca->tier_priority;
   if (cb->score_per_token > ca->score_per_token)
      return 1;
   if (cb->score_per_token < ca->score_per_token)
      return -1;
   if (cb->score > ca->score)
      return 1;
   if (cb->score < ca->score)
      return -1;
   return 0;
}

/* Compute term overlap between query terms and a text string.
 * Returns fraction of query terms found in the lowercased text. */
static double compute_term_overlap(char **query_terms, int term_count, const char *text)
{
   if (term_count <= 0 || !text)
      return 0.0;

   /* Lowercase copy for matching */
   char lower[2560];
   int li = 0;
   for (int i = 0; text[i] && li < (int)sizeof(lower) - 1; i++)
      lower[li++] = (char)tolower((unsigned char)text[i]);
   lower[li] = '\0';

   int matched = 0;
   for (int t = 0; t < term_count; t++)
   {
      if (strstr(lower, query_terms[t]))
         matched++;
   }
   return (double)matched / (double)term_count;
}

/* Compute graph boost score for a candidate against the boost map */
static double compute_graph_score(const boost_map_t *bmap, const char *key, const char *content)
{
   if (!bmap || bmap->count == 0)
      return 0.0;

   double boost = 0.0;
   for (int b = 0; b < bmap->count; b++)
   {
      if (strstr(key, bmap->entries[b].entity))
         boost += bmap->entries[b].score * 0.1;
      if (content && strstr(content, bmap->entries[b].entity))
         boost += bmap->entries[b].score * 0.1;
   }
   return boost;
}

static int context_tier_priority(const char *tier)
{
   return memory_tier_priority(tier);
}

static void context_candidate_fill_scope(context_candidate_t *c, const char *workspace,
                                         const char *project)
{
   char scope_value[128];
   memory_scope_level_t scope_level;

   if (!c)
      return;

   scope_value[0] = '\0';
   scope_level = memory_primary_scope(c->id, scope_value, sizeof(scope_value));
   snprintf(c->scope, sizeof(c->scope), "%s", memory_scope_level_name(scope_level));
   c->scope_rank = memory_scope_visibility_rank(c->id, workspace, project);
   c->tier_priority = context_tier_priority(c->tier);
}

/* Task-aware context assembly.
 *
 * When task_hint is NULL, falls back to the original per-section query behavior.
 * When task_hint is provided, uses a unified candidate pool scored by:
 *   relevance = base_score + term_overlap + graph_boost
 * then fills sections from the sorted pool. */

static int append_global_constraints(char *buf, int pos, int cap)
{
   /* Global Constraints: high-priority preferences and policies, injected at
    * the top of the context so they survive budget compression. */
   {
      db2_memory_kv_row_t gc_rows[6];
      int gc_n = db2_memory_list_global_constraints(gc_rows, 6);
      int section_start = pos;
      int has_any = 0;
      int section_len = 0;
      const int section_cap = (MAX_CONTEXT_TOTAL * 12) / 100;

      for (int i = 0; i < gc_n; i++)
      {
         const char *k = gc_rows[i].key;
         const char *v = gc_rows[i].content;
         if (!k[0])
            continue;
         const char *text = v[0] ? v : k;

         char truncated[MAX_MEM_CONTENT_LEN + 8];
         if ((int)strlen(text) > MAX_MEM_CONTENT_LEN)
         {
            snprintf(truncated, sizeof(truncated), "%.*s...", MAX_MEM_CONTENT_LEN, text);
            text = truncated;
         }

         int line_len = (int)strlen(k) + (int)strlen(text) + 8;
         if (section_len + line_len > section_cap)
            break;

         if (!has_any)
         {
            pos += snprintf(buf + pos, (size_t)(cap - pos), "\n## Global Constraints\n");
            has_any = 1;
         }
         int written = snprintf(buf + pos, (size_t)(cap - pos), "- %s: %s\n", k, text);
         if (written < 0 || written >= cap - pos)
            break;
         pos += written;
         section_len += written;
      }
      if (!has_any)
         pos = section_start;
   }
   return pos;
}

static int append_untasked_context(char *buf, int pos, int cap)
{
   /* --- Original behavior: per-section queries --- */

   /* Key Facts — L2 consolidated facts plus L3 slow-changing project
    * facts.  L3 carries stable environment/config facts once promoted from
    * L2 (see memory_promote_stable_l2_to_l3). */
   {
      db2_memory_key_fact_row_t kf_rows[MAX_CONTEXT_MEMS];
      int kf_n = db2_memory_list_key_facts_with_provenance(kf_rows, MAX_CONTEXT_MEMS);
      int section_start = pos;
      pos += snprintf(buf + pos, cap - pos, "\n## Key Facts\n");
      int items = 0;
      int section_len = 0;

      char *seed_keys[MAX_CONTEXT_MEMS];
      int seed_count = 0;

      for (int i = 0; i < kf_n && items < MAX_CONTEXT_MEMS; i++)
      {
         const char *key = kf_rows[i].key;
         const char *content = kf_rows[i].content;
         const char *supersede_ts = kf_rows[i].supersede_date;
         const char *provenance = kf_rows[i].provenance;
         const char *text = content[0] ? content : (key[0] ? key : "");

         if (key[0])
            seed_keys[seed_count++] = strdup(key);

         char dep_hint[128] = "";
         db2_memory_key_row_t deps[3];
         int dn = db2_memory_list_depends_on_keys(kf_rows[i].id, deps, 3);
         int dpos = 0;
         for (int d = 0; d < dn; d++)
         {
            if (!deps[d].key[0])
               continue;
            if (dpos == 0)
               dpos += snprintf(dep_hint + dpos, sizeof(dep_hint) - dpos, " (depends on: %s",
                                deps[d].key);
            else
               dpos += snprintf(dep_hint + dpos, sizeof(dep_hint) - dpos, ", %s", deps[d].key);
         }
         if (dpos > 0)
            snprintf(dep_hint + dpos, sizeof(dep_hint) - dpos, ")");

         char line[MAX_MEM_CONTENT_LEN + 256];
         char prov_anchor[128] = "";
         if (provenance[0])
            snprintf(prov_anchor, sizeof(prov_anchor), " [Source: %s]", provenance);

         if (supersede_ts[0])
         {
            char date[11] = {0};
            snprintf(date, sizeof(date), "%.10s", supersede_ts);
            snprintf(line, sizeof(line), "- %.*s (updated %s)%s%s", MAX_MEM_CONTENT_LEN, text, date,
                     prov_anchor, dep_hint);
         }
         else
         {
            snprintf(line, sizeof(line), "- %.*s%s%s", MAX_MEM_CONTENT_LEN, text, prov_anchor,
                     dep_hint);
         }

         int line_len = (int)strlen(line) + 1;
         if (section_len + line_len > 2000)
            break;
         pos += snprintf(buf + pos, cap - pos, "%s\n", line);
         section_len += line_len;
         items++;
      }

      if (seed_count > 0)
      {
         graph_related_t related[10];
         int rcount = memory_graph_related(seed_keys, seed_count, related, 10);
         if (rcount > 0)
         {
            pos += snprintf(buf + pos, cap - pos, "\n## Related Context\n");
            for (int r = 0; r < rcount; r++)
            {
               pos += snprintf(buf + pos, cap - pos, "- %s\n", related[r].content);
            }
         }
         for (int i = 0; i < seed_count; i++)
            free(seed_keys[i]);
      }

      if (items == 0)
         pos = section_start;
   }

   db2_memory_kv_row_t rows[MAX_CONTEXT_MEMS];
   int rn;

   rn = db2_memory_list_kv_section(DB2_MEM_SECTION_ACTIVE_TASKS, rows, MAX_CONTEXT_MEMS);
   pos = append_section(buf, pos, cap, "Active Tasks", rows, rn, 1500, MAX_CONTEXT_MEMS);

   rn = db2_memory_list_kv_section(DB2_MEM_SECTION_RECENT_CONTEXT, rows, MAX_CONTEXT_MEMS);
   pos = append_section(buf, pos, cap, "Recent Context", rows, rn, 1200, MAX_CONTEXT_MEMS);

   rn = db2_memory_list_kv_section(DB2_MEM_SECTION_CONSTRAINTS, rows, MAX_CONTEXT_MEMS);
   pos = append_section(buf, pos, cap, "Constraints", rows, rn, 800, MAX_CONTEXT_MEMS);

   rn = db2_memory_list_kv_section(DB2_MEM_SECTION_PROCEDURES, rows, MAX_CONTEXT_MEMS);
   pos = append_section(buf, pos, cap, "Procedures", rows, rn, 600, MAX_CONTEXT_MEMS);

   rn = db2_memory_list_kv_section(DB2_MEM_SECTION_FAILURE_WARNINGS, rows, MAX_CONTEXT_MEMS);
   pos = append_section(buf, pos, cap, "Failure Warnings", rows, rn, 600, 3);
   return pos;
}

static int append_task_aware_context(char *buf, int pos, int cap, const char *task_hint)
{
   /* --- Task-aware: unified candidate pool with relevance scoring --- */

   /* 1. Tokenize task_hint */
   char *query_terms[64];
   int term_count = tokenize_for_search(task_hint, query_terms, 64);
   char workspace_label[MAX_PATH_LEN];
   char project_label[MAX_PATH_LEN];
   memory_scope_labels_for_cwd(NULL, workspace_label, sizeof(workspace_label), project_label,
                               sizeof(project_label));

   /* Detect temporal / recency-anchored queries. When present, candidates
    * whose key or content mentions a temporal marker get a scoring bonus
    * and the plan's recency_weight is overridden so recent memories float
    * to the top (LoCoMo Temporal). */
   int temporal_query = has_temporal_markers(task_hint);

   /* Session-shaped queries: inject episode cards at the top of the context
    * so the LLM sees the high-level session summary before individual turns. */
   if (memory_is_session_query(task_hint))
   {
      db2_memory_episode_card_row_t cards[4];
      int card_n = db2_memory_list_episode_cards(cards, 4);
      int has_cards = 0;
      int cards_cap = (MAX_CONTEXT_TOTAL * 20) / 100;
      int cards_len = 0;
      for (int i = 0; i < card_n; i++)
      {
         const char *ct = cards[i].content;
         if (!ct[0])
            continue;
         int clen = (int)strlen(ct);
         if (cards_len + clen > cards_cap)
            break;
         if (!has_cards)
         {
            int written = snprintf(buf + pos, (size_t)(cap - pos), "\n## Episode Summaries\n");
            if (written > 0 && written < cap - pos)
               pos += written;
            has_cards = 1;
         }
         int written = snprintf(buf + pos, (size_t)(cap - pos), "%s\n\n", ct);
         if (written < 0 || written >= cap - pos)
            break;
         pos += written;
         cards_len += clen;
      }
   }

   /* 2. Compute graph boost map */
   boost_map_t bmap;
   bmap.count = 0;
   if (term_count > 0)
      memory_graph_boost(query_terms, term_count, &bmap);

   /* 3. Load all L1/L2/L3/L5 candidates.  L3 = stable project/env facts,
    * L5 = synthesised cross-session patterns.  Both are eligible for dense
    * pgvector recall through the shared ranking path, weighted via
    * confidence/use_count + graph signals. */
   context_candidate_t candidates[MAX_CANDIDATES];
   int cand_count = 0;

   db2_memory_cand_row_t cand_rows[MAX_CANDIDATES];
   int cand_n = db2_memory_list_candidates(DB2_MEM_CAND_PRIMARY, cand_rows, MAX_CANDIDATES);
   for (int ci = 0; ci < cand_n && cand_count < MAX_CANDIDATES; ci++)
   {
      context_candidate_t *c = &candidates[cand_count];
      c->id = cand_rows[ci].id;
      snprintf(c->tier, sizeof(c->tier), "%s", cand_rows[ci].tier);
      snprintf(c->key, sizeof(c->key), "%s", cand_rows[ci].key);
      snprintf(c->content, sizeof(c->content), "%s", cand_rows[ci].content);
      snprintf(c->kind, sizeof(c->kind), "%s", cand_rows[ci].kind);
      c->confidence = cand_rows[ci].confidence;
      c->use_count = cand_rows[ci].use_count;
      context_candidate_fill_scope(c, workspace_label, project_label);

      double base = c->confidence * 0.3 + (c->use_count / 10.0) * 0.2;
      if (base > 0.5)
         base = 0.5;

      double key_overlap = compute_term_overlap(query_terms, term_count, c->key);
      double content_overlap = compute_term_overlap(query_terms, term_count, c->content);

      double overlap = (key_overlap > content_overlap ? key_overlap : content_overlap) * 0.3;
      double graph = compute_graph_score(&bmap, c->key, c->content) * 0.2;
      c->score = base + overlap + graph;

      if (temporal_query && has_temporal_markers(c->content))
         c->score += 0.15;
      else if (temporal_query && has_temporal_markers(c->key))
         c->score += 0.10;

      c->score += c->scope_rank * 0.05;
      c->score += c->tier_priority * 0.03;

      int key_len = (int)strlen(c->key);
      int content_len = (int)strlen(c->content);
      c->estimated_tokens = (key_len + content_len) / 4 + 1;
      c->score_per_token = c->score / (double)c->estimated_tokens;

      cand_count++;
   }

   /* 4. Sort by score descending (default) or score_per_token (budget mode) */

   /* Check whether token-budget assembly is enabled in config. */
   config_t assemble_cfg;
   config_load(&assemble_cfg);
   int budget_mode = assemble_cfg.memory_context_budget_enabled;
   int token_budget = assemble_cfg.memory_context_budget_tokens > 0
                          ? assemble_cfg.memory_context_budget_tokens
                          : 2048; /* default budget */

   if (cand_count > 1)
      qsort(candidates, cand_count, sizeof(context_candidate_t),
            budget_mode ? cmp_candidates_by_density : cmp_candidates);

   /* 4b. Retrieval-failure detection: compute confidence and optionally
    * attempt a fallback before assembling context. */
   int low_confidence_flag = 0; /* 1 = inject LOW marker into context */
   int answerability_withheld = 0;
   if (assemble_cfg.memory_failure_detection_enabled || assemble_cfg.memory_abstain_enabled)
   {
      double threshold =
          assemble_cfg.memory_abstain_enabled
              ? (assemble_cfg.memory_abstain_gate > 0.0 ? assemble_cfg.memory_abstain_gate : 0.40)
              : (assemble_cfg.memory_failure_detection_threshold > 0.0
                     ? assemble_cfg.memory_failure_detection_threshold
                     : 0.35);

      retrieval_confidence_t rconf;
      memory_retrieval_confidence((const char **)query_terms, term_count, candidates, cand_count,
                                  threshold, &rconf);

      if (rconf.below_threshold && cand_count < MAX_CANDIDATES)
      {
         /* Fallback: attempt a broader fetch by relaxing the tier filter
          * and extending to L0 memories as well. */
         db2_memory_cand_row_t fb_rows[MAX_CANDIDATES];
         int fb_n = db2_memory_list_candidates(DB2_MEM_CAND_FALLBACK, fb_rows, MAX_CANDIDATES);
         int new_count = cand_count;
         for (int fi = 0; fi < fb_n && new_count < MAX_CANDIDATES; fi++)
         {
            int64_t fid = fb_rows[fi].id;
            int dup = 0;
            for (int j = 0; j < cand_count; j++)
               if (candidates[j].id == fid)
               {
                  dup = 1;
                  break;
               }
            if (dup)
               continue;

            context_candidate_t *c = &candidates[new_count];
            c->id = fid;
            snprintf(c->tier, sizeof(c->tier), "%s", fb_rows[fi].tier);
            snprintf(c->key, sizeof(c->key), "%s", fb_rows[fi].key);
            snprintf(c->content, sizeof(c->content), "%s", fb_rows[fi].content);
            snprintf(c->kind, sizeof(c->kind), "%s", fb_rows[fi].kind);
            c->confidence = fb_rows[fi].confidence;
            c->use_count = fb_rows[fi].use_count;
            context_candidate_fill_scope(c, workspace_label, project_label);
            double key_ov = compute_term_overlap(query_terms, term_count, c->key);
            double cnt_ov = compute_term_overlap(query_terms, term_count, c->content);
            c->score = c->confidence * 0.3 + (key_ov > cnt_ov ? key_ov : cnt_ov) * 0.3 +
                       (c->use_count / 10.0) * 0.2;
            c->score += c->scope_rank * 0.05;
            c->score += c->tier_priority * 0.03;
            c->estimated_tokens = ((int)strlen(c->key) + (int)strlen(c->content)) / 4 + 1;
            c->score_per_token = c->score / (double)c->estimated_tokens;
            new_count++;
         }
         if (new_count > cand_count)
         {
            cand_count = new_count;
            if (cand_count > 1)
               qsort(candidates, cand_count, sizeof(context_candidate_t),
                     budget_mode ? cmp_candidates_by_density : cmp_candidates);
            memory_retrieval_confidence((const char **)query_terms, term_count, candidates,
                                        cand_count, threshold, &rconf);
            rconf.fallback_triggered = 1;
         }
      }

      if (rconf.below_threshold)
      {
         low_confidence_flag = 1;
         answerability_withheld = assemble_cfg.memory_abstain_enabled ? 1 : 0;
         /* Record the failure; after threshold crossings, the
          * directives subsystem auto-creates a retrieval_failure
          * directive so future turns can ask the user to fill the
          * gap instead of silently flailing. Gated by the directives
          * toggle (default on); manually-created directives still
          * surface when it is off. */
         if (assemble_cfg.memory_directives_enabled)
         {
            char norm_hint[256];
            normalize_key(task_hint, norm_hint, sizeof(norm_hint));
            if (norm_hint[0])
               memory_directive_record_retrieval_failure(norm_hint, 0, "");
         }
      }
   }

   /* Free tokenized terms — must happen after confidence check uses them */
   for (int t = 0; t < term_count; t++)
      free(query_terms[t]);

   context_candidate_t negative_candidates[MAX_CANDIDATES];
   for (int i = 0; i < cand_count; i++)
      negative_candidates[i] = candidates[i];
   if (answerability_withheld)
      cand_count = 0;

   /* 5. Classify intent and build retrieval plan for dynamic budgets */
   retrieval_plan_t rplan;
   {
      task_intent_t intent = classify_intent(task_hint);
      retrieval_plan_for_intent(intent, &rplan);
      if (temporal_query)
         rplan.recency_weight = 0.85; /* contextual recency anchoring */
   }

   /* Compute per-section char-budgets from plan (total = MAX_CONTEXT_TOTAL - overhead) */
   int section_budget = MAX_CONTEXT_TOTAL - 500; /* reserve for headers/structure */

   struct
   {
      const char *header;
      const char *kind;
      const char *kind2;
      const char *tier;
      int max_chars;
      int priority_band; /* lower = earlier in budget-mode pass */
   } sections[] = {
       /* Band 1: L4 directives assembled first, ahead of episodic context. */
       {"Mental Models", "policy", "workflow", "L4", (int)(0.16 * section_budget), 0},
       /* Band 2: L2 observations and constraints. */
       {"Key Facts", "fact", "preference", NULL,
        (int)((rplan.kind_budget[0] + rplan.kind_budget[1]) * section_budget), 2},
       {"Constraints", "decision", "policy", NULL,
        (int)((rplan.kind_budget[2] + rplan.kind_budget[7]) * section_budget), 1},
       /* Band 3: L1 experiences and procedures. */
       {"Procedures", "procedure", NULL, NULL, (int)(rplan.kind_budget[6] * section_budget), 3},
       {"Active Tasks", "task", NULL, NULL, (int)(rplan.kind_budget[4] * section_budget), 2},
       {"Recent Context", "episode", NULL, NULL, (int)(rplan.kind_budget[3] * section_budget), 3},
   };
   int nsections = 6;

   /* In budget mode track overall token usage across sections. */
   int tokens_used = 0;

   for (int s = 0; s < nsections; s++)
   {
      int section_start = pos;
      pos += snprintf(buf + pos, cap - pos, "\n## %s\n", sections[s].header);
      int section_len = 0;
      int items = 0;
      int selected[MAX_CONTEXT_MEMS];
      int selected_count = 0;
      int section_tokens = 0;
      const char *xml_tag = context_xml_tag_for_header(sections[s].header);

      for (int i = 0; i < cand_count && items < MAX_CONTEXT_MEMS; i++)
      {
         context_candidate_t *c = &candidates[i];
         if (c->id == 0)
            continue; /* already consumed */

         int match = (strcmp(c->kind, sections[s].kind) == 0);
         if (!match && sections[s].kind2)
            match = (strcmp(c->kind, sections[s].kind2) == 0);
         if (match && sections[s].tier)
            match = (strcmp(c->tier, sections[s].tier) == 0);
         if (!match)
            continue;

         /* In budget mode: skip if remaining token budget is too small */
         if (budget_mode && tokens_used + section_tokens + c->estimated_tokens > token_budget)
            continue;

         selected[selected_count++] = i;
         section_tokens += c->estimated_tokens;
         items++;
      }

      if (selected_count == 0)
      {
         pos = section_start;
         continue;
      }

      items = 0;
      /* Emit lowest-scored first so the highest-scored candidate lands at
       * the bottom of the section — closest to the prompt appended after
       * the context block.  `selected[]` was populated in descending-score
       * order, so reverse iteration gives ascending-score emit order. */
      for (int i = selected_count - 1; i >= 0; i--)
      {
         context_candidate_t *c = &candidates[selected[i]];
         int written =
             emit_candidate(buf, pos, cap, c, xml_tag, &section_len, sections[s].max_chars);
         if (written == 0)
            continue;
         {
            const char *sid = session_id();
            if (sid && sid[0] && db1_context_snapshot_insert)
               (void)db1_context_snapshot_insert(sid, c->id, c->score);
         }
         pos += written;
         if (budget_mode)
            tokens_used += c->estimated_tokens;
         c->id = 0;
         items++;
      }
      if (items == 0)
         pos = section_start;
   }

   pos = append_negative_context_block(buf, pos, cap, task_hint, negative_candidates, cand_count);

   /* Failure Warnings (L3 episodes) - only include when plan says so */
   if (rplan.include_l3)
   {
      db2_memory_kv_row_t fw_rows[MAX_CONTEXT_MEMS];
      int fw_n =
          db2_memory_list_kv_section(DB2_MEM_SECTION_FAILURE_WARNINGS, fw_rows, MAX_CONTEXT_MEMS);
      pos = append_section(buf, pos, cap, "Failure Warnings", fw_rows, fw_n, 600, 3);
   }

   /* Quantitative / date-arithmetic derived facts.
    * Run the deriver when the query is quantitative or temporal-interval
    * and the feature is enabled.  Results are appended as a small JSON
    * block that the answer prompt is taught to consult. */
   {
      config_t derive_cfg;
      config_load(&derive_cfg);
      if (derive_cfg.memory_derive_facts_enabled)
      {
         memory_query_shape_t dshape = memory_classify_deriver_shape(task_hint);
         if (dshape == MEM_SHAPE_QUANTITATIVE || dshape == MEM_SHAPE_TEMPORAL_INTERVAL)
         {
            /* Collect candidate IDs */
            int64_t cids[MAX_CANDIDATES];
            for (int i = 0; i < cand_count; i++)
               cids[i] = candidates[i].id;

            memory_derived_facts_t dfacts;
            int nfacts = memory_derive_facts(task_hint, cids, cand_count, &derive_cfg, &dfacts);
            if (nfacts > 0)
            {
               char *json = memory_derived_facts_to_json(&dfacts);
               if (json)
               {
                  int written =
                      snprintf(buf + pos, (size_t)(cap - pos), "\n## Derived Facts\n%s\n", json);
                  if (written > 0 && written < cap - pos)
                     pos += written;
                  free(json);
               }
            }
         }
      }
   }

   /* Retrieval-failure LOW marker.
    * Appended last so the LLM sees it near the end of context, immediately
    * before it begins composing its answer. */
   if (answerability_withheld)
   {
      int written = snprintf(buf + pos, (size_t)(cap - pos),
                             "\n## Memory Answerability\n"
                             "No reliable memory evidence for this query.\n");
      if (written > 0 && written < cap - pos)
         pos += written;
   }
   else if (low_confidence_flag)
   {
      int written = snprintf(buf + pos, (size_t)(cap - pos),
                             "\n## Retrieval Confidence: LOW\n"
                             "The retrieved memories may not contain reliable information "
                             "for this query.  If you cannot find a confident answer in "
                             "the context above, say so explicitly rather than speculating.\n");
      if (written > 0 && written < cap - pos)
         pos += written;
   }
   return pos;
}

static int append_entity_relationships(char *buf, int pos, int cap)
{
   /* Entity relationships from graph (top distinct triples by weight). */
   {
      edge_t edges[10];
      int n = db2_entity_edge_top_distinct_triples(edges, 10);
      int has_edges = 0;
      for (int i = 0; i < n && pos < cap - 128; i++)
      {
         if (!has_edges)
         {
            pos += snprintf(buf + pos, (size_t)(cap - pos), "\n## Entity Relationships\n");
            has_edges = 1;
         }
         pos += snprintf(buf + pos, (size_t)(cap - pos), "- %s -[%s]-> %s\n",
                         edges[i].source[0] ? edges[i].source : "?",
                         edges[i].relation[0] ? edges[i].relation : "?",
                         edges[i].target[0] ? edges[i].target : "?");
      }
   }
   return pos;
}

static int append_graph_expansion(char *buf, int pos, int cap, const char *task_hint)
{
   /* Graph expansion: pull in supporting memories using typed S-R-O triples
    * (memory_relations) when available, falling back to co_discussed edges.
    * Only runs when task_hint is provided (task-aware path).
    *
    * Typed expansion: for each entity term from the task_hint, query
    * memory_relations for triples that involve that entity as either subject
    * or object.  For each triple found, include the associated fact_text as
    * a supporting node rather than naively adding all co-discussed neighbors. */
   if (task_hint && task_hint[0])
   {
      /* Tokenize task_hint into entity seeds */
      char *seeds[16];
      int seed_count = 0;
      char hint_copy[512];
      snprintf(hint_copy, sizeof(hint_copy), "%s", task_hint);
      char *svp = NULL;
      char *t = strtok_r(hint_copy, " \t\n", &svp);
      while (t && seed_count < 16)
      {
         if (strlen(t) >= 3)
            seeds[seed_count++] = t;
         t = strtok_r(NULL, " \t\n", &svp);
      }

      if (seed_count > 0)
      {
         /* Typed S-R-O expansion: fetch fact_text triples where the query
          * entity appears as src_entity or dst_entity. */
         int sro_added = 0;
         for (int si = 0; si < seed_count && sro_added < 4; si++)
         {
            memory_relation_t rels[6];
            int rn = db2_memory_relations_supporting(seeds[si], 6, rels, 6);
            for (int i = 0; i < rn && sro_added < 4; i++)
            {
               if (!rels[i].fact_text[0])
                  continue;
               if (sro_added == 0)
                  pos += snprintf(buf + pos, (size_t)(cap - pos),
                                  "\n## Supporting Relations (graph)\n");
               pos += snprintf(buf + pos, (size_t)(cap - pos), "- [%s -[%s]-> %s] %.*s\n",
                               rels[i].src_entity[0] ? rels[i].src_entity : "?",
                               rels[i].relation[0] ? rels[i].relation : "?",
                               rels[i].dst_entity[0] ? rels[i].dst_entity : "?",
                               MAX_MEM_CONTENT_LEN, rels[i].fact_text);
               sro_added++;
            }
         }

         /* Fall back to co_discussed edge expansion when no S-R-O triples found */
         if (sro_added == 0)
         {
            graph_related_t related[8];
            int rcount = memory_graph_related(seeds, seed_count, related, 8);
            if (rcount > 0)
            {
               pos += snprintf(buf + pos, (size_t)(cap - pos), "\n## Related Memories (graph)\n");
               for (int i = 0; i < rcount && pos < cap - 128; i++)
               {
                  const char *text = (related[i].content[0]) ? related[i].content : related[i].key;
                  pos += snprintf(buf + pos, (size_t)(cap - pos), "- %.*s\n", MAX_MEM_CONTENT_LEN,
                                  text);
               }
            }
         }
      }
   }
   return pos;
}

static void check_artifact_staleness(void)
{
   /* Artifact staleness check (Feature 9) */
   {
      db2_memory_artifact_row_t arts[50];
      int an = db2_memory_list_artifact_hashed(arts, 50);
      for (int i = 0; i < an; i++)
      {
         if (strcmp(arts[i].artifact_type, "file") != 0)
            continue;
         FILE *f = fopen(arts[i].artifact_ref, "r");
         if (!f)
            continue;
         unsigned long h = 0;
         int c;
         while ((c = fgetc(f)) != EOF)
            h = h * 31 + (unsigned long)c;
         fclose(f);
         char current_hash[65];
         snprintf(current_hash, sizeof(current_hash), "%016lx", h);
         if (strcmp(current_hash, arts[i].artifact_hash) != 0)
            db2_memory_decay_confidence(arts[i].id);
      }
   }
}

char *memory_assemble_context(const char *task_hint)
{
   int cap = MAX_CONTEXT_TOTAL + 256;

   /* Check context cache */
   if (!getenv("AIMEE_NO_CACHE"))
   {
      char hash[32];
      cache_input_hash(hash, sizeof(hash));
      char *cached = malloc(cap);
      if (cached && db1_context_cache_get && db1_context_cache_get(hash, cached, cap) == 0)
         return cached;
      free(cached);
   }

   char *buf = malloc(cap);
   if (!buf)
      return NULL;

   int pos = snprintf(buf, cap, "# Memory Context");

   pos = append_global_constraints(buf, pos, cap);

   if (!task_hint || !task_hint[0])
      pos = append_untasked_context(buf, pos, cap);
   else
      pos = append_task_aware_context(buf, pos, cap, task_hint);

   pos = append_entity_relationships(buf, pos, cap);
   pos = append_graph_expansion(buf, pos, cap, task_hint);
   check_artifact_staleness();

   buf[pos] = '\0';

   /* Store in context cache */
   if (!getenv("AIMEE_NO_CACHE"))
   {
      char hash[32];
      cache_input_hash(hash, sizeof(hash));
      if (db1_context_cache_put)
         db1_context_cache_put(hash, buf);
   }

   return buf;
}

/* Explain-mode context assembly.
 *
 * Runs a candidate scoring pass (identical to memory_assemble_context) but
 * additionally populates `explain[]` with per-candidate selection details so
 * callers can audit which memories were included or rejected and why.
 *
 * explain_count is set to the number of entries written.  metrics (optional)
 * receives budget usage counters.  Returns the assembled context string. */
char *memory_assemble_context_explain(const char *task_hint,
                                      context_assemble_explain_entry_t *explain, int *explain_count,
                                      int explain_max, context_budget_metrics_t *metrics)
{
   if (explain_count)
      *explain_count = 0;

   /* Load config to know budget settings */
   config_t ecfg;
   config_load(&ecfg);
   int budget_mode = ecfg.memory_context_budget_enabled;
   int token_budget =
       ecfg.memory_context_budget_tokens > 0 ? ecfg.memory_context_budget_tokens : 2048;

   if (metrics)
   {
      metrics->budget_tokens = budget_mode ? token_budget : 0;
      metrics->used_tokens = 0;
      metrics->rejected_for_budget = 0;
   }

   /* Run the normal assembly first */
   char *ctx = memory_assemble_context(task_hint);

   /* No explain data requested — just return */
   if (!explain || explain_max <= 0 || !explain_count)
      return ctx;

   /* Re-run candidate scoring to populate explain entries. */
   db2_memory_cand_row_t cand_rows[200];
   int cand_n = db2_memory_list_candidates(DB2_MEM_CAND_PRIMARY, cand_rows, 200);

   char *qterms[64];
   int qterm_count = task_hint ? tokenize_for_search(task_hint, qterms, 64) : 0;
   char workspace_label[MAX_PATH_LEN];
   char project_label[MAX_PATH_LEN];
   memory_scope_labels_for_cwd(NULL, workspace_label, sizeof(workspace_label), project_label,
                               sizeof(project_label));
   context_candidate_t scored[200];
   int scored_count = 0;

   for (int ci = 0; ci < cand_n && scored_count < (int)(sizeof(scored) / sizeof(scored[0])); ci++)
   {
      context_candidate_t *c = &scored[scored_count];
      memset(c, 0, sizeof(*c));

      c->id = cand_rows[ci].id;
      snprintf(c->tier, sizeof(c->tier), "%s", cand_rows[ci].tier);
      snprintf(c->key, sizeof(c->key), "%s", cand_rows[ci].key);
      snprintf(c->content, sizeof(c->content), "%s", cand_rows[ci].content);
      snprintf(c->kind, sizeof(c->kind), "%s", cand_rows[ci].kind);
      c->confidence = cand_rows[ci].confidence;
      c->use_count = cand_rows[ci].use_count;
      context_candidate_fill_scope(c, workspace_label, project_label);

      int klen = (int)strlen(c->key);
      int clen = (int)strlen(c->content);
      c->estimated_tokens = (klen + clen) / 4 + 1;

      double base = c->confidence * 0.3 + (c->use_count / 10.0) * 0.2;
      if (base > 0.5)
         base = 0.5;
      boost_map_t bmap;
      bmap.count = 0;
      if (qterm_count > 0)
         memory_graph_boost(qterms, qterm_count, &bmap);
      double ko = qterm_count > 0 ? compute_term_overlap(qterms, qterm_count, c->key) : 0.0;
      double co = qterm_count > 0 ? compute_term_overlap(qterms, qterm_count, c->content) : 0.0;
      double overlap = (ko > co ? ko : co) * 0.3;
      double graph = compute_graph_score(&bmap, c->key, c->content) * 0.2;
      c->score = base + overlap + graph + c->scope_rank * 0.05 + c->tier_priority * 0.03;
      c->score_per_token = c->score / (double)c->estimated_tokens;
      scored_count++;
   }

   if (scored_count > 1)
      qsort(scored, scored_count, sizeof(scored[0]),
            budget_mode ? cmp_candidates_by_density : cmp_candidates);

   int ecount = 0;
   int tokens_used = 0;
   for (int i = 0; i < scored_count && ecount < explain_max; i++)
   {
      context_assemble_explain_entry_t *e = &explain[ecount];
      const context_candidate_t *c = &scored[i];
      memset(e, 0, sizeof(*e));
      e->id = c->id;
      snprintf(e->tier, sizeof(e->tier), "%s", c->tier);
      snprintf(e->key, sizeof(e->key), "%.*s", (int)(sizeof(e->key) - 1), c->key);
      snprintf(e->kind, sizeof(e->kind), "%s", c->kind);
      snprintf(e->scope, sizeof(e->scope), "%s", c->scope[0] ? c->scope : "global");
      e->tokens = c->estimated_tokens;
      e->score = c->score;
      e->score_per_token = c->score_per_token;

      if (budget_mode)
      {
         if (tokens_used + e->tokens <= token_budget)
         {
            e->selected = 1;
            tokens_used += e->tokens;
            if (metrics)
               metrics->used_tokens = tokens_used;
         }
         else
         {
            e->selected = 0;
            snprintf(e->rejection_reason, sizeof(e->rejection_reason),
                     "budget_exhausted (used=%d budget=%d)", tokens_used, token_budget);
            if (metrics)
               metrics->rejected_for_budget++;
         }
      }
      else
      {
         e->selected = (ecount < MAX_CONTEXT_MEMS) ? 1 : 0;
         if (!e->selected)
            snprintf(e->rejection_reason, sizeof(e->rejection_reason), "count_cap (MAX=%d)",
                     MAX_CONTEXT_MEMS);
      }
      ecount++;
   }

   for (int t = 0; t < qterm_count; t++)
      free(qterms[t]);

   *explain_count = ecount;
   return ctx;
}

/* Workspace-aware context assembly.
 *
 * Two-pass approach:
 * Pass 1 (70% budget): memories tagged with current workspace or _shared.
 * Pass 2 (30% budget): high-confidence memories from other workspaces.
 * Untagged memories are treated as _shared. */
char *memory_assemble_context_ws(const char *task_hint, const char *workspace)
{
   if (!workspace || !workspace[0])
      return memory_assemble_context(task_hint);

   int cap = MAX_CONTEXT_TOTAL + 256;
   char *buf = malloc(cap);
   if (!buf)
      return NULL;

   int pos = snprintf(buf, cap, "# Memory Context");

   /* Budget split: 70% for workspace-scoped, 30% for cross-workspace */
   int ws_budget = (MAX_CONTEXT_TOTAL * 70) / 100;
   int cross_budget = MAX_CONTEXT_TOTAL - ws_budget;
   char project[MAX_PATH_LEN];
   memory_scope_labels_for_cwd(workspace, NULL, 0, project, sizeof(project));

   /* --- Pass 1: visible memories (project > workspace > global) --- */

   db2_memory_id_kv_row_t ws_rows[64];
   int ws_n;
   const char *proj_arg = project[0] ? project : NULL;

   ws_n = db2_memory_list_id_kv_ws_section(DB2_MEM_WS_KEY_FACTS, ws_rows, 64);
   pos = append_scoped_section(buf, pos, cap, "Key Facts", ws_rows, ws_n, workspace, proj_arg, 1,
                               (ws_budget * 36) / 100, MAX_CONTEXT_MEMS);

   ws_n = db2_memory_list_id_kv_ws_section(DB2_MEM_WS_ACTIVE_TASKS, ws_rows, 64);
   pos = append_scoped_section(buf, pos, cap, "Active Tasks", ws_rows, ws_n, workspace, proj_arg, 1,
                               (ws_budget * 27) / 100, MAX_CONTEXT_MEMS);

   ws_n = db2_memory_list_id_kv_ws_section(DB2_MEM_WS_RECENT_CONTEXT, ws_rows, 64);
   pos = append_scoped_section(buf, pos, cap, "Recent Context", ws_rows, ws_n, workspace, proj_arg,
                               1, (ws_budget * 22) / 100, MAX_CONTEXT_MEMS);

   ws_n = db2_memory_list_id_kv_ws_section(DB2_MEM_WS_CONSTRAINTS, ws_rows, 64);
   pos = append_scoped_section(buf, pos, cap, "Constraints", ws_rows, ws_n, workspace, proj_arg, 1,
                               (ws_budget * 12) / 100, MAX_CONTEXT_MEMS);

   ws_n = db2_memory_list_id_kv_ws_section(DB2_MEM_WS_PROCEDURES, ws_rows, 64);
   pos = append_scoped_section(buf, pos, cap, "Procedures", ws_rows, ws_n, workspace, proj_arg, 1,
                               (ws_budget * 10) / 100, MAX_CONTEXT_MEMS);

   /* --- Pass 2: high-confidence cross-workspace memories --- */
   ws_n = db2_memory_list_id_kv_ws_section(DB2_MEM_WS_CROSS_WORKSPACE, ws_rows, 64);
   pos = append_scoped_section(buf, pos, cap, "Cross-Workspace", ws_rows, ws_n, workspace, proj_arg,
                               0, cross_budget, MAX_CONTEXT_MEMS / 2);

   /* Entity relationships from graph (top distinct triples by weight). */
   {
      edge_t edges[10];
      int n = db2_entity_edge_top_distinct_triples(edges, 10);
      int has_edges = 0;
      for (int i = 0; i < n && pos < cap - 128; i++)
      {
         if (!has_edges)
         {
            pos += snprintf(buf + pos, (size_t)(cap - pos), "\n## Entity Relationships\n");
            has_edges = 1;
         }
         pos += snprintf(buf + pos, (size_t)(cap - pos), "- %s -[%s]-> %s\n",
                         edges[i].source[0] ? edges[i].source : "?",
                         edges[i].relation[0] ? edges[i].relation : "?",
                         edges[i].target[0] ? edges[i].target : "?");
      }
   }

   buf[pos] = '\0';
   return buf;
}

/* Context cache storage moved to src/db1/caches.c (db1_context_cache_*).
 * The hash-input computation below still reads DB2 state (memories, rules,
 * entity_edges), so it stays on the DB2 side; only the cache read/write/
 * invalidate primitives live in DB1. */

/* Simple hash: djb2 on the concatenation of counts and timestamps */
char *cache_input_hash(char *buf, size_t buf_len)
{
   char raw[512];
   int pos = 0;

   /* Memory count + max updated_at */
   {
      int cnt = 0;
      char ts[32] = "";
      if (db2_memory_count_and_max_updated(&cnt, ts, sizeof(ts)))
         pos += snprintf(raw + pos, sizeof(raw) - pos, "m:%d:%s;", cnt, ts);
   }

   /* Rule count + max updated_at, sourced from the typed DB1 surface
    * so this fingerprint never speaks SQL against the rules table. */
   {
      char rule_sig[32];
      db2_rules_signature(rule_sig, sizeof(rule_sig));
      pos += snprintf(raw + pos, sizeof(raw) - pos, "r:%s;", rule_sig);
   }

   raw[pos] = '\0';

   /* djb2 hash */
   unsigned long hash = 5381;
   for (int i = 0; raw[i]; i++)
      hash = ((hash << 5) + hash) + (unsigned char)raw[i];

   snprintf(buf, buf_len, "%016lx", hash);
   return buf;
}

#endif

/* memory_citation_gate_check: returns 1 if the answer string contains at
 * least one citation marker of the form [#<digits>], 0 otherwise.
 * Used by callers to verify that post-hoc citation enforcement succeeded. */
int memory_citation_gate_check(const char *answer)
{
   if (!answer)
      return 0;
   /* Scan for the pattern '[' '#' <one or more digits> */
   for (const char *p = answer; *p; p++)
   {
      if (*p == '[' && *(p + 1) == '#')
      {
         const char *q = p + 2;
         if (*q >= '0' && *q <= '9')
            return 1;
      }
   }
   return 0;
}
