#if defined(AIMEE_DB2_DISABLED)
#error "memory_core KB-real TU must not be compiled into the AIMEE_DB2_DISABLED (server) build"
#endif
#ifndef _GNU_SOURCE /* strcasestr/memmem are GNU extensions (container gcc) */
#define _GNU_SOURCE
#endif
#include "memory_core_internal.h"
/* memory_core_helpers.c: split from memory_core.c into a real translation unit
 * (was memory_core_helpers.inc, textually included only to stay under the
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

void memory_refresh_coref_entities(int64_t memory_id, const char *content)
{
   if (memory_id <= 0 || !content || !content[0] || !memory_coref_has_pronoun(content))
      return;

   config_t cfg;
   config_load(&cfg);
   const char *mode = memory_coref_mode_effective(&cfg);
   if (strcmp(mode, "heuristic") != 0 && strcmp(mode, "llm") != 0)
      return;

   char session_buf[128];
   if (db2_memory_get_source_session(memory_id, session_buf, (int)sizeof(session_buf)) != 0)
      return;

   /* LLM mode: delegate to cognifier for higher-recall resolution */
   if (strcmp(mode, "llm") == 0)
   {
      int bound = memory_coref_llm_resolve(memory_id, content, session_buf, &cfg);
      memory_coref_audit_record(memory_id, session_buf, bound ? "bound" : "unbound", "", "llm",
                                bound ? 1.0 : 0.0);
      return;
   }

   /* Heuristic mode: scan prior memories for unambiguous named entity */
   db2_memory_prior_row_t prior[64];
   int prior_max = (int)(sizeof(prior) / sizeof(prior[0]));
   int prior_n = db2_memory_list_prior_in_session(
       session_buf, memory_id, memory_coref_window_effective(&cfg), prior, prior_max);

   char chosen[128] = "";
   int ambiguous = 0;
   for (int i = 0; i < prior_n; i++)
   {
      char names[8][128];
      int count = memory_extract_named_entities(prior[i].content, names, 8);
      if (count == 0)
         count = memory_extract_named_entities(prior[i].key, names, 8);
      if (count == 1)
      {
         snprintf(chosen, sizeof(chosen), "%s", names[0]);
         break;
      }
      if (count > 1)
      {
         chosen[0] = '\0';
         ambiguous = 1;
         break;
      }
   }

   if (chosen[0])
   {
      memory_entity_insert(memory_id, chosen, "coref", 2.8);
      memory_coref_audit_record(memory_id, session_buf, "bound", chosen, "heuristic", 1.0);
   }
   else
   {
      memory_coref_audit_record(memory_id, session_buf, ambiguous ? "ambiguous" : "unbound", "",
                                "heuristic", 0.0);
   }
}

static void memory_refresh_temporal_refs(int64_t memory_id, const char *key, const char *content)
{
   db2_memory_temporal_refs_delete_for_memory(memory_id);

   char norm[2048];
   char tokens[24][64];
   char phrase[128];
   normalize_key(key ? key : "", norm, sizeof(norm));
   int key_count = memory_split_tokens(norm, tokens, 24);
   int created_year = 0, created_month = 0, created_day = 0;
   int has_created =
       memory_parse_created_date(memory_id, &created_year, &created_month, &created_day);
   for (int i = 0; i < key_count; i++)
   {
      if (memory_is_month_token(tokens[i]))
         memory_temporal_insert(memory_id, tokens[i], "month", 1.8);
      else if (memory_is_weekday_token(tokens[i]))
         memory_temporal_insert(memory_id, tokens[i], "weekday", 1.5);
      else if (strlen(tokens[i]) == 4 && isdigit((unsigned char)tokens[i][0]) &&
               isdigit((unsigned char)tokens[i][1]) && isdigit((unsigned char)tokens[i][2]) &&
               isdigit((unsigned char)tokens[i][3]))
         memory_temporal_insert(memory_id, tokens[i], "year", 2.0);
   }

   normalize_key(content ? content : "", norm, sizeof(norm));
   int token_count = memory_split_tokens(norm, tokens, 24);
   for (int i = 0; i < token_count; i++)
   {
      if (strcmp(tokens[i], "today") == 0 || strcmp(tokens[i], "yesterday") == 0 ||
          strcmp(tokens[i], "tomorrow") == 0 || strcmp(tokens[i], "tonight") == 0 ||
          strcmp(tokens[i], "morning") == 0 || strcmp(tokens[i], "afternoon") == 0 ||
          strcmp(tokens[i], "evening") == 0)
      {
         memory_temporal_insert(memory_id, tokens[i], "relative", 1.8);
         if (has_created &&
             (strcmp(tokens[i], "today") == 0 || strcmp(tokens[i], "yesterday") == 0 ||
              strcmp(tokens[i], "tomorrow") == 0))
         {
            int y = created_year, m = created_month, d = created_day;
            if (strcmp(tokens[i], "yesterday") == 0)
               memory_shift_day(&y, &m, &d, -1);
            else if (strcmp(tokens[i], "tomorrow") == 0)
               memory_shift_day(&y, &m, &d, 1);
            memory_format_date(phrase, sizeof(phrase), y, m, d);
            memory_temporal_insert(memory_id, phrase, "absolute_day", 2.6);
         }
      }
      else if (memory_is_month_token(tokens[i]))
      {
         memory_temporal_insert(memory_id, tokens[i], "month", 2.0);
         if (i + 1 < token_count && strlen(tokens[i + 1]) <= 4)
         {
            memory_alias_join_tokens(phrase, sizeof(phrase), tokens, i, 2);
            if (phrase[0])
               memory_temporal_insert(memory_id, phrase, "date_phrase", 2.3);
            if (has_created)
            {
               int month_num = memory_month_number(tokens[i]);
               int day_num = atoi(tokens[i + 1]);
               if (month_num > 0 && day_num > 0 && day_num <= 31)
               {
                  memory_format_date(phrase, sizeof(phrase), created_year, month_num, day_num);
                  memory_temporal_insert(memory_id, phrase, "absolute_day", 2.8);
               }
            }
         }
      }
      else if (memory_is_weekday_token(tokens[i]))
         memory_temporal_insert(memory_id, tokens[i], "weekday", 1.7);
      else if (strlen(tokens[i]) == 4 && isdigit((unsigned char)tokens[i][0]) &&
               isdigit((unsigned char)tokens[i][1]) && isdigit((unsigned char)tokens[i][2]) &&
               isdigit((unsigned char)tokens[i][3]))
         memory_temporal_insert(memory_id, tokens[i], "year", 2.2);
      else if (strcmp(tokens[i], "before") == 0 || strcmp(tokens[i], "after") == 0 ||
               strcmp(tokens[i], "during") == 0 || strcmp(tokens[i], "first") == 0 ||
               strcmp(tokens[i], "last") == 0 || strcmp(tokens[i], "recently") == 0)
      {
         memory_temporal_insert(memory_id, tokens[i], "relative", 1.2);
         /* "last week", "last month", "last year" => resolve to approximate date range */
         if (strcmp(tokens[i], "last") == 0 && i + 1 < token_count && has_created)
         {
            int y = created_year, m = created_month, d = created_day;
            if (strcmp(tokens[i + 1], "week") == 0)
            {
               memory_shift_day(&y, &m, &d, -7);
               memory_format_date(phrase, sizeof(phrase), y, m, d);
               memory_temporal_insert(memory_id, phrase, "absolute_day", 2.2);
               memory_temporal_insert(memory_id, "last:week", "relative_phrase", 2.0);
            }
            else if (strcmp(tokens[i + 1], "month") == 0)
            {
               memory_temporal_insert(memory_id, "last:month", "relative_phrase", 2.0);
               /* Insert anchor for the first of last month */
               int pm = m - 1, py = y;
               if (pm < 1)
               {
                  pm = 12;
                  py--;
               }
               memory_format_date(phrase, sizeof(phrase), py, pm, 1);
               memory_temporal_insert(memory_id, phrase, "absolute_day", 2.0);
            }
            else if (strcmp(tokens[i + 1], "year") == 0)
            {
               memory_temporal_insert(memory_id, "last:year", "relative_phrase", 2.0);
               char yr_buf[8];
               snprintf(yr_buf, sizeof(yr_buf), "%04d", y - 1);
               memory_temporal_insert(memory_id, yr_buf, "year", 2.2);
            }
            else if (memory_is_weekday_token(tokens[i + 1]))
            {
               /* "last Tuesday" => resolve to previous occurrence */
               static const char *wd[] = {"monday", "tuesday",  "wednesday", "thursday",
                                          "friday", "saturday", "sunday",    NULL};
               int target_wd = -1;
               for (int w = 0; wd[w]; w++)
                  if (strcmp(tokens[i + 1], wd[w]) == 0)
                  {
                     target_wd = w;
                     break;
                  }
               if (target_wd >= 0)
               {
                  struct tm tmv;
                  memset(&tmv, 0, sizeof(tmv));
                  tmv.tm_year = created_year - 1900;
                  tmv.tm_mon = created_month - 1;
                  tmv.tm_mday = created_day;
                  timegm(&tmv); /* normalise */
                  mktime(&tmv);
                  int cur_wd = tmv.tm_wday ? tmv.tm_wday - 1 : 6; /* 0=Mon */
                  int delta = cur_wd - target_wd;
                  if (delta <= 0)
                     delta += 7;
                  int ly = created_year, lm = created_month, ld = created_day;
                  memory_shift_day(&ly, &lm, &ld, -delta);
                  memory_format_date(phrase, sizeof(phrase), ly, lm, ld);
                  memory_temporal_insert(memory_id, phrase, "absolute_day", 2.4);
               }
            }
         }
         if ((strcmp(tokens[i], "before") == 0 || strcmp(tokens[i], "after") == 0 ||
              strcmp(tokens[i], "during") == 0) &&
             i + 1 < token_count)
         {
            char relation_ref[128];
            snprintf(relation_ref, sizeof(relation_ref), "%s:%s", tokens[i], tokens[i + 1]);
            memory_temporal_insert(memory_id, relation_ref, "ordering", 1.8);
         }
         if (strcmp(tokens[i], "between") == 0 && i + 2 < token_count)
         {
            char range_ref[128];
            snprintf(range_ref, sizeof(range_ref), "between:%s:%s", tokens[i + 1], tokens[i + 2]);
            memory_temporal_insert(memory_id, range_ref, "range", 2.0);
         }
      }
      /* "N days/weeks/months ago" */
      else if (i + 2 < token_count && strcmp(tokens[i + 2], "ago") == 0 && tokens[i][0] >= '1' &&
               tokens[i][0] <= '9' && has_created)
      {
         int n = atoi(tokens[i]);
         if (n > 0)
         {
            int y = created_year, m = created_month, d = created_day;
            if (strcmp(tokens[i + 1], "days") == 0 || strcmp(tokens[i + 1], "day") == 0)
            {
               memory_shift_day(&y, &m, &d, -n);
               memory_format_date(phrase, sizeof(phrase), y, m, d);
               memory_temporal_insert(memory_id, phrase, "absolute_day", 2.5);
               memory_temporal_insert(memory_id, "ago", "relative", 1.0);
            }
            else if (strcmp(tokens[i + 1], "weeks") == 0 || strcmp(tokens[i + 1], "week") == 0)
            {
               memory_shift_day(&y, &m, &d, -(n * 7));
               memory_format_date(phrase, sizeof(phrase), y, m, d);
               memory_temporal_insert(memory_id, phrase, "absolute_day", 2.4);
               memory_temporal_insert(memory_id, "ago", "relative", 1.0);
            }
            else if (strcmp(tokens[i + 1], "months") == 0 || strcmp(tokens[i + 1], "month") == 0)
            {
               int pm = m - n, py = y;
               while (pm < 1)
               {
                  pm += 12;
                  py--;
               }
               memory_format_date(phrase, sizeof(phrase), py, pm, d > 28 ? 28 : d);
               memory_temporal_insert(memory_id, phrase, "absolute_day", 2.4);
               memory_temporal_insert(memory_id, "ago", "relative", 1.0);
            }
         }
      }
   }
}

static void memory_refresh_negation_tokens(int64_t memory_id, const char *key, const char *content)
{
   config_t neg_cfg;
   if (config_load(&neg_cfg) != 0 || !neg_cfg.memory_negation_enabled)
      return;

   /* Combine key + content for tokenisation */
   char combined[3072];
   snprintf(combined, sizeof(combined), "%s %s", key ? key : "", content ? content : "");

   char neg_tokens[2048];
   int count = extract_negation_tokens(combined, neg_tokens, sizeof(neg_tokens));
   if (count <= 0)
      neg_tokens[0] = '\0';

   db2_memory_negation_tokens_update(memory_id, neg_tokens);
}

void memory_refresh_derived_metadata(int64_t memory_id, const char *key, const char *content)
{
   memory_refresh_aliases(memory_id, key, content);
   memory_refresh_entities(memory_id, key, content);
   memory_refresh_coref_entities(memory_id, content);
   memory_refresh_temporal_refs(memory_id, key, content);
   memory_refresh_summaries(memory_id, key, content);
   memory_refresh_event_frames(memory_id, key, content);
   memory_refresh_chunks(memory_id, content);
   memory_refresh_units_graph(memory_id, key, content);
   memory_refresh_episode_relations(memory_id, key, content);
   memory_refresh_unit_embeddings(memory_id);
   memory_refresh_negation_tokens(memory_id, key, content);
}

int memory_sync_vec_row(int64_t memory_id, const float *vec, int dim)
{
   if (!vec || dim <= 0)
      return 0;
   if (db2_vector_index_sync_suppressed())
      return 0;

   char *payload_json = db2_memory_build_memory_payload(memory_id);
   if (!payload_json)
   {
      db2_vector_index_op_record(memory_id, pgvec_memory_vector_collection_name(), memory_id, 0,
                                 "payload build failed");
      return -1;
   }
   int rc = pgvec_memory_vector_upsert_memory(memory_id, vec, dim, payload_json);
   free(payload_json);
   db2_vector_index_op_record(memory_id, pgvec_memory_vector_collection_name(), memory_id, rc == 0,
                              rc ? "upsert failed" : NULL);
   return rc;
}

static int memory_sync_unit_vec_row(int64_t unit_id, const float *vec, int dim)
{
   if (!vec || dim <= 0)
      return 0;
   if (db2_vector_index_sync_suppressed())
      return 0;

   int64_t memory_id = 0;
   char *payload_json = db2_memory_build_unit_payload(unit_id, &memory_id);
   int64_t point_id = PGVEC_MEMORY_VECTOR_UNIT_ID_OFFSET + unit_id;
   if (!payload_json)
   {
      db2_vector_index_op_record(point_id, pgvec_memory_vector_collection_name(), memory_id, 0,
                                 "payload build failed");
      return -1;
   }
   int rc = pgvec_memory_vector_upsert_unit(unit_id, vec, dim, payload_json);
   free(payload_json);
   db2_vector_index_op_record(point_id, pgvec_memory_vector_collection_name(), memory_id, rc == 0,
                              rc ? "upsert failed" : NULL);
   return rc;
}

int memory_fetch_row_by_id(int64_t memory_id, memory_t *out)
{
   return db2_memory_get(memory_id, out);
}

int memory_fetch_row_by_unit_id(int64_t unit_id, memory_t *out)
{
   return db2_memory_get_by_unit_id(unit_id, out);
}

const char *memory_effective_embedding_cmd(const char *command)
{
   /* Single policy point: config_embedding_command (config_save.c). The caller
    * has already resolved config -> command upstream, so pass it as the request
    * override; this keeps memory_core consistent with the kb-side resolution. */
   return config_embedding_command(NULL, command);
}

/* Per-recall query-embedding memo.
 *
 * A single memory_find_facts() embeds the *same* query text once per retrieval
 * lane (unit-semantic, memory-semantic, summary/fact lanes), and the whole
 * candidate generator re-runs per HyDE pass and per decomposition sub-query.
 * Every embed forks a subprocess and round-trips to the embedder, so embedding
 * identical text repeatedly dominates recall latency. This thread-local memo
 * embeds each distinct (command,text) once for the duration of a recall and
 * serves the cached vector to the other lanes. Embedding is deterministic for a
 * given (command,text), so a cache hit is byte-identical to a fresh embed.
 *
 * Reset at each top-level recall entry point. Bounded so a pathological query
 * (many distinct sub-queries) can't grow it without limit; an overflow or an
 * over-long key simply falls through to a fresh embed (correct, just no reuse). */
enum
{
   MEMORY_QEMBED_CAP = 32,
   MEMORY_QEMBED_TEXT_MAX = 1024,
   MEMORY_QEMBED_CMD_MAX = 512,
   MEMORY_EMBED_HTTP_TIMEOUT_MS = 30000
};

/* In-process HTTP embedding. When embedding_command is an http(s):// URL we POST
 * to the embedder directly instead of forking an interpreter, so a query embed is
 * a ~10ms round-trip rather than a process spawn. Under host CPU load (curator,
 * indexing) the spawn dominates memory-search latency, so this is the difference
 * between a fast search and a multi-second one. Non-URL commands keep the exec
 * path. Endpoints match the aimee-llm gateway: POST {base}/embed (raw text)
 * and {base}/embed_batch (JSON array of strings); the server ignores Content-Type
 * and reads the raw body, so agent_http_post's default header is fine. */
int memory_embed_command_is_http(const char *cmd)
{
   return cmd && (strncmp(cmd, "http://", 7) == 0 || strncmp(cmd, "https://", 8) == 0);
}

/* Join an embedder base URL and an endpoint path, dropping a trailing slash on
 * the base so "http://e:8080/" + "/embed" -> "http://e:8080/embed". */
static void memory_embed_http_url(const char *base, const char *path, char *out, size_t out_len)
{
   size_t n = strlen(base);
   while (n > 0 && base[n - 1] == '/')
      n--;
   snprintf(out, out_len, "%.*s%s", (int)n, base, path);
}

/* POST body to {base}{path}; on success returns 0 with the response body in
 * *resp (caller frees). Any transport error or non-2xx leaves *resp NULL. */
int memory_embed_http_post(const char *base, const char *path, const char *body, char **resp)
{
   char url[1024];
   memory_embed_http_url(base, path, url, sizeof(url));
   *resp = NULL;
   int status = agent_http_post(url, NULL, body, resp, MEMORY_EMBED_HTTP_TIMEOUT_MS, NULL);
   if (status < 200 || status >= 300 || !*resp)
   {
      free(*resp);
      *resp = NULL;
      return -1;
   }
   return 0;
}

typedef struct
{
   char command[MEMORY_QEMBED_CMD_MAX];
   char text[MEMORY_QEMBED_TEXT_MAX];
   float vec[EMBED_MAX_DIM];
   int dim;
} memory_qembed_entry_t;

static __thread int s_qembed_count = 0;
static __thread memory_qembed_entry_t s_qembed[MEMORY_QEMBED_CAP];

/* Per-recall counters: total runtime embed requests vs cache misses (= real
 * embedder invocations). requests-minus-misses is the work the memo saved. */
static __thread int s_qembed_requests = 0;
static __thread int s_qembed_misses = 0;
/* Per-search stage-timing probe (read+reset by the memory_search wrapper): wall
 * time spent in actual embed spawns (cache misses) and how many ran. */
__thread long long s_qembed_ms = 0;
__thread int s_qembed_spawns = 0;
/* Cross-encoder rerank spawn time + count, same per-recall probe. */
__thread long long s_rerank_ms = 0;
__thread int s_rerank_calls = 0;

/* Begin a fresh per-recall memo window. Call at every top-level recall entry
 * point before any lane embeds the query. */
void memory_query_embed_cache_reset(void)
{
   s_qembed_count = 0;
   s_qembed_requests = 0;
   s_qembed_misses = 0;
}

int memory_embed_text_runtime(const char *text, const char *command, float *out, int max_dim)
{
   const char *effective_cmd = memory_effective_embedding_cmd(command);
   s_qembed_requests++;

   /* Cacheable only when both keys fit their buffers (so a stored key is exact,
    * never a truncated prefix that could false-match a different query). */
   int cacheable = text && command && strlen(text) < MEMORY_QEMBED_TEXT_MAX &&
                   strlen(effective_cmd) < MEMORY_QEMBED_CMD_MAX;

   if (cacheable)
   {
      for (int i = 0; i < s_qembed_count; i++)
      {
         if (s_qembed[i].dim > 0 && s_qembed[i].dim <= max_dim &&
             strcmp(s_qembed[i].command, effective_cmd) == 0 && strcmp(s_qembed[i].text, text) == 0)
         {
            memcpy(out, s_qembed[i].vec, (size_t)s_qembed[i].dim * sizeof(float));
            return s_qembed[i].dim;
         }
      }
   }

   s_qembed_misses++;
   long long _emb_t0 = util_now_ms();
   int dim = memory_embed_text(text, effective_cmd, out, max_dim);
   if (dim <= 0 && strcmp(effective_cmd, "builtin") != 0)
   {
      aimee_log(LOG_WARN, "memory",
                "query embedding failed for configured command; retrying with builtin embeddings");
      dim = memory_embed_text(text, "builtin", out, max_dim);
   }
   s_qembed_ms += util_now_ms() - _emb_t0;
   s_qembed_spawns++;

   /* Store on the way out so the other lanes / sub-queries of this same recall
    * reuse the vector instead of re-spawning the embedder. */
   if (cacheable && dim > 0 && dim <= EMBED_MAX_DIM && s_qembed_count < MEMORY_QEMBED_CAP)
   {
      memory_qembed_entry_t *e = &s_qembed[s_qembed_count++];
      snprintf(e->command, sizeof(e->command), "%s", effective_cmd);
      snprintf(e->text, sizeof(e->text), "%s", text);
      memcpy(e->vec, out, (size_t)dim * sizeof(float));
      e->dim = dim;
   }
   return dim;
}

/* Store one (command,text)->vector entry into the per-recall memo (used by the
 * batch prewarm to seed vectors it embedded in bulk). */
static void memory_query_embed_cache_put(const char *command, const char *text, const float *vec,
                                         int dim)
{
   if (!command || !text || !vec || dim <= 0 || dim > EMBED_MAX_DIM)
      return;
   if (strlen(text) >= MEMORY_QEMBED_TEXT_MAX || strlen(command) >= MEMORY_QEMBED_CMD_MAX)
      return;
   for (int i = 0; i < s_qembed_count; i++)
      if (strcmp(s_qembed[i].command, command) == 0 && strcmp(s_qembed[i].text, text) == 0)
         return; /* already cached */
   if (s_qembed_count >= MEMORY_QEMBED_CAP)
      return;
   memory_qembed_entry_t *e = &s_qembed[s_qembed_count++];
   snprintf(e->command, sizeof(e->command), "%s", command);
   snprintf(e->text, sizeof(e->text), "%s", text);
   memcpy(e->vec, vec, (size_t)dim * sizeof(float));
   e->dim = dim;
}

/* Embed-batching: embed up to `n` query texts in ONE batched embedder call and
 * seed them into the per-recall memo, so the retrieval lanes hit the memo
 * instead of making one HTTP round-trip per distinct text. Best-effort: skips
 * builtin (in-process, nothing to batch) and texts already cached / too long;
 * any failure (command error, count mismatch) leaves the memo untouched so the
 * lanes simply embed individually. The embedding command must accept a JSON
 * array of strings on stdin and return a JSON array of float arrays
 * (scripts/embed-remote.py batch mode → embedder /embed_batch). */
void memory_query_embed_prewarm(const char *const *texts, int n, const char *command)
{
   const char *cmd = memory_effective_embedding_cmd(command);
   if (n <= 0 || !texts || strcmp(cmd, "builtin") == 0)
      return;

   const char *picked[MEMORY_QEMBED_CAP];
   int np = 0;
   cJSON *arr = cJSON_CreateArray();
   if (!arr)
      return;
   for (int i = 0; i < n && np < MEMORY_QEMBED_CAP; i++)
   {
      const char *t = texts[i];
      if (!t || !t[0] || strlen(t) >= MEMORY_QEMBED_TEXT_MAX)
         continue;
      int dup = 0;
      for (int j = 0; j < s_qembed_count && !dup; j++)
         if (strcmp(s_qembed[j].command, cmd) == 0 && strcmp(s_qembed[j].text, t) == 0)
            dup = 1;
      for (int j = 0; j < np && !dup; j++)
         if (strcmp(picked[j], t) == 0)
            dup = 1;
      if (dup)
         continue;
      picked[np++] = t;
      cJSON_AddItemToArray(arr, cJSON_CreateString(t));
   }
   if (np < 2) /* one text: a single /embed is no slower than a batch of one */
   {
      cJSON_Delete(arr);
      return;
   }

   char *input = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   if (!input)
      return;
   char *buf = NULL;
   int rc;
   if (memory_embed_command_is_http(cmd))
   {
      rc = memory_embed_http_post(cmd, "/embed_batch", input, &buf);
   }
   else
   {
      size_t buf_len = 0;
      rc = platform_exec_pipe(cmd, input, strlen(input), &buf, &buf_len);
      if (rc == 0 && (!buf || buf_len == 0))
         rc = -1;
   }
   free(input);
   if (rc != 0 || !buf)
   {
      free(buf);
      return; /* lanes fall back to individual embeds */
   }
   cJSON *resp = cJSON_Parse(buf);
   free(buf);
   if (!cJSON_IsArray(resp) || cJSON_GetArraySize(resp) != np)
   {
      cJSON_Delete(resp);
      return;
   }
   int idx = 0;
   cJSON *vecj;
   cJSON_ArrayForEach(vecj, resp)
   {
      if (cJSON_IsArray(vecj))
      {
         float vec[EMBED_MAX_DIM];
         int dim = 0;
         cJSON *el;
         cJSON_ArrayForEach(el, vecj)
         {
            if (dim >= EMBED_MAX_DIM)
               break;
            if (cJSON_IsNumber(el))
               vec[dim++] = (float)el->valuedouble;
         }
         if (dim > 0)
            memory_query_embed_cache_put(cmd, picked[idx], vec, dim);
      }
      idx++;
   }
   cJSON_Delete(resp);
}

/* Test hooks (declared in memory.h) — expose the static memo to unit tests. */
int memory_query_embed_runtime_test(const char *text, const char *command, float *out, int max_dim)
{
   return memory_embed_text_runtime(text, command, out, max_dim);
}

void memory_query_embed_cache_stats_test(int *requests, int *misses)
{
   if (requests)
      *requests = s_qembed_requests;
   if (misses)
      *misses = s_qembed_misses;
}

void memory_query_embed_cache_reset_test(void)
{
   memory_query_embed_cache_reset();
}

void memory_query_embed_prewarm_test(const char *const *texts, int n, const char *command)
{
   memory_query_embed_prewarm(texts, n, command);
}

int memory_semantic_query_unavailable(void)
{
   aimee_log(LOG_WARN, "memory", "pgvector semantic query path unavailable");
   return -1;
}

int memory_vector_ready(void)
{
   return pgvec_memory_vector_collection_exists() > 0;
}

static void memory_embed_unit_row(int64_t unit_id, const char *unit_type, const char *unit_key,
                                  const char *unit_text, double weight, const char *command)
{
   if (unit_id <= 0 || !unit_text || !unit_text[0])
      return;

   char text[2048];
   if (unit_type && strcmp(unit_type, "event") == 0)
      snprintf(text, sizeof(text), "event actor_action %s details %s weight %.2f",
               unit_key ? unit_key : "", unit_text, weight);
   else if (unit_type && strcmp(unit_type, "temporal") == 0)
      snprintf(text, sizeof(text), "time reference %s granularity %s weight %.2f", unit_text,
               unit_key ? unit_key : "", weight);
   else if (unit_type && strcmp(unit_type, "entity") == 0)
      snprintf(text, sizeof(text), "entity role %s value %s weight %.2f", unit_key ? unit_key : "",
               unit_text, weight);
   else if (unit_type && strcmp(unit_type, "summary") == 0)
      snprintf(text, sizeof(text), "summary scope %s text %s weight %.2f", unit_key ? unit_key : "",
               unit_text, weight);
   else if (unit_type && strcmp(unit_type, "chunk") == 0)
      snprintf(text, sizeof(text), "evidence chunk %s text %s weight %.2f",
               unit_key ? unit_key : "", unit_text, weight);
   else
      snprintf(text, sizeof(text), "%s %s %s %.2f", unit_type ? unit_type : "",
               unit_key ? unit_key : "", unit_text, weight);

   float vec[EMBED_MAX_DIM];
   const char *model = (command && command[0]) ? command : "builtin";
   int dim = memory_embed_text(text, model, vec, EMBED_MAX_DIM);
   if (dim <= 0)
      return;

   (void)model;
   memory_sync_unit_vec_row(unit_id, vec, dim);
}

void memory_refresh_unit_embeddings(int64_t memory_id)
{
   if (memory_id <= 0)
      return;
   config_t cfg;
   config_load(&cfg);
   const char *embed_command = config_embedding_command(&cfg, NULL);

   db2_memory_unit_row_t units[64];
   int unit_max = (int)(sizeof(units) / sizeof(units[0]));
   int n = db2_memory_units_list(memory_id, units, unit_max);
   for (int i = 0; i < n; i++)
   {
      memory_embed_unit_row(units[i].id, units[i].unit_type, units[i].unit_key, units[i].unit_text,
                            units[i].weight, embed_command);
   }
}

static const char *memory_unit_kind_name(memory_unit_kind_t kind)
{
   switch (kind)
   {
   case MEMORY_UNIT_KIND_SEMANTIC:
      return MEMORY_UNIT_KIND_SEMANTIC_STR;
   case MEMORY_UNIT_KIND_PROCEDURAL:
      return MEMORY_UNIT_KIND_PROCEDURAL_STR;
   case MEMORY_UNIT_KIND_EPISODIC:
   default:
      return MEMORY_UNIT_KIND_EPISODIC_STR;
   }
}

static memory_unit_kind_t memory_unit_kind_for_memory(const char *memory_kind,
                                                      const char *unit_type, int has_events,
                                                      int has_temporal)
{
   if (memory_kind &&
       (!strcmp(memory_kind, KIND_PREFERENCE) || !strcmp(memory_kind, KIND_PROCEDURE) ||
        !strcmp(memory_kind, KIND_POLICY) || !strcmp(memory_kind, KIND_WORKFLOW)))
      return MEMORY_UNIT_KIND_PROCEDURAL;
   if (unit_type && (!strcmp(unit_type, "event") || !strcmp(unit_type, "temporal") ||
                     !strcmp(unit_type, "episode")))
      return MEMORY_UNIT_KIND_EPISODIC;
   if (memory_kind && !strcmp(memory_kind, KIND_EPISODE))
      return MEMORY_UNIT_KIND_EPISODIC;
   if (has_events || has_temporal)
      return MEMORY_UNIT_KIND_EPISODIC;
   return MEMORY_UNIT_KIND_SEMANTIC;
}

/* When the cognifier emitted an explicit episodic/semantic/procedural label
 * for the whole memory, that overrides the per-unit inference above. Returns
 * -1 if no explicit kind was emitted so callers fall back to the heuristic. */
static int memory_unit_kind_from_explicit(const char *explicit_kind, memory_unit_kind_t *out)
{
   if (!explicit_kind || !explicit_kind[0] || !out)
      return -1;
   if (strcmp(explicit_kind, MEMORY_UNIT_KIND_EPISODIC_STR) == 0)
   {
      *out = MEMORY_UNIT_KIND_EPISODIC;
      return 0;
   }
   if (strcmp(explicit_kind, MEMORY_UNIT_KIND_SEMANTIC_STR) == 0)
   {
      *out = MEMORY_UNIT_KIND_SEMANTIC;
      return 0;
   }
   if (strcmp(explicit_kind, MEMORY_UNIT_KIND_PROCEDURAL_STR) == 0)
   {
      *out = MEMORY_UNIT_KIND_PROCEDURAL;
      return 0;
   }
   return -1;
}

static memory_unit_kind_t memory_unit_kind_resolve(const char *explicit_kind,
                                                   const char *memory_kind, const char *unit_type,
                                                   int has_events, int has_temporal)
{
   memory_unit_kind_t explicit_out;
   if (memory_unit_kind_from_explicit(explicit_kind, &explicit_out) == 0)
      return explicit_out;
   return memory_unit_kind_for_memory(memory_kind, unit_type, has_events, has_temporal);
}

double memory_unit_kind_intent_boost(const char *memory_kind, memory_query_intent_t intent)
{
   if (!memory_kind)
      return 0.0;
   if (intent == MEM_QUERY_TEMPORAL)
   {
      if (strcmp(memory_kind, MEMORY_UNIT_KIND_EPISODIC_STR) == 0)
         return 0.18;
      if (strcmp(memory_kind, MEMORY_UNIT_KIND_SEMANTIC_STR) == 0)
         return -0.06;
   }
   else if (intent == MEM_QUERY_ENTITY || intent == MEM_QUERY_GENERAL)
   {
      if (strcmp(memory_kind, MEMORY_UNIT_KIND_SEMANTIC_STR) == 0)
         return 0.12;
      if (strcmp(memory_kind, MEMORY_UNIT_KIND_EPISODIC_STR) == 0)
         return -0.03;
   }
   else if (intent == MEM_QUERY_PROCEDURAL)
   {
      if (strcmp(memory_kind, MEMORY_UNIT_KIND_PROCEDURAL_STR) == 0)
         return 0.22;
      return -0.08;
   }
   return 0.0;
}

static int64_t memory_unit_insert(int64_t memory_id, const char *unit_type,
                                  memory_unit_kind_t memory_kind, const char *unit_key,
                                  const char *unit_text, double weight)
{
   if (memory_id <= 0 || !unit_type || !unit_type[0] || !unit_text || !unit_text[0])
      return 0;

   char norm_key[256];
   char norm_text[512];
   normalize_key(unit_key ? unit_key : "", norm_key, sizeof(norm_key));
   normalize_key(unit_text, norm_text, sizeof(norm_text));
   if (!norm_text[0])
      return 0;

   return db2_memory_unit_insert(memory_id, unit_type, memory_unit_kind_name(memory_kind), norm_key,
                                 norm_text, weight);
}

static void memory_unit_edge_insert(int64_t src_unit_id, int64_t dst_unit_id, const char *edge_type,
                                    double weight)
{
   db2_memory_unit_edge_insert(src_unit_id, dst_unit_id, edge_type, weight);
}

void memory_refresh_units_graph(int64_t memory_id, const char *key, const char *content)
{
   if (memory_id <= 0)
      return;

   db2_memory_unit_edges_delete_for_memory(memory_id);

   /* Drop the per-unit pgvector points before deleting the rows. */
   {
      int64_t prior_unit_ids[256];
      int prior_n = db2_memory_unit_list_ids(
          memory_id, prior_unit_ids, (int)(sizeof(prior_unit_ids) / sizeof(prior_unit_ids[0])));
      for (int i = 0; i < prior_n; i++)
      {
         int64_t pt = PGVEC_MEMORY_VECTOR_UNIT_ID_OFFSET + prior_unit_ids[i];
         pgvec_memory_vector_delete_point(pt);
         db2_vector_index_op_remove(pt);
      }
   }
   db2_memory_units_delete_for_memory(memory_id);

   int64_t summary_units[8];
   int summary_count = 0;
   int64_t event_units[16];
   int event_count = 0;
   int64_t temporal_units[16];
   int temporal_count = 0;
   int64_t entity_units[16];
   int entity_count = 0;
   int64_t chunk_units[16];
   int chunk_count = 0;
   int64_t episode_unit_id = 0;
   char source_session[128];
   char memory_kind[16];
   char explicit_kind[16];
   db2_memory_get_session_kinds(memory_id, source_session, sizeof(source_session), memory_kind,
                                sizeof(memory_kind), explicit_kind, sizeof(explicit_kind));

   {
      db2_memory_summary_row_t summaries[8];
      int n = db2_memory_summaries_list(memory_id, 8, summaries, 8);
      for (int i = 0; i < n && summary_count < 8; i++)
      {
         summary_units[summary_count] = memory_unit_insert(
             memory_id, "summary",
             memory_unit_kind_resolve(explicit_kind, memory_kind, "summary", event_count > 0,
                                      temporal_count > 0),
             summaries[i].scope[0] ? summaries[i].scope : "headline", summaries[i].summary, 3.0);
         if (summary_units[summary_count] > 0)
            summary_count++;
      }
   }
   if (summary_count == 0 && key && key[0])
   {
      summary_units[summary_count] =
          memory_unit_insert(memory_id, "summary",
                             memory_unit_kind_resolve(explicit_kind, memory_kind, "summary",
                                                      event_count > 0, temporal_count > 0),
                             "fallback", key, 2.0);
      if (summary_units[summary_count] > 0)
         summary_count++;
   }

   {
      db2_memory_event_frame_row_t events[16];
      int n = db2_memory_event_frames_list(memory_id, events, 16);
      for (int i = 0; i < n && event_count < 16; i++)
      {
         char unit_key_buf[256];
         char unit_text_buf[512];
         snprintf(unit_key_buf, sizeof(unit_key_buf), "%s %s", events[i].actor, events[i].action);
         snprintf(unit_text_buf, sizeof(unit_text_buf), "%s %s %s %s %s", events[i].actor,
                  events[i].action, events[i].object, events[i].location, events[i].event_time);
         event_units[event_count] = memory_unit_insert(
             memory_id, "event", MEMORY_UNIT_KIND_EPISODIC, unit_key_buf, unit_text_buf, 2.8);
         if (event_units[event_count] > 0)
            event_count++;
      }
   }

   {
      db2_memory_temporal_ref_row_t temporals[16];
      int n = db2_memory_temporal_refs_list(memory_id, temporals, 16);
      for (int i = 0; i < n && temporal_count < 16; i++)
      {
         temporal_units[temporal_count] =
             memory_unit_insert(memory_id, "temporal", MEMORY_UNIT_KIND_EPISODIC,
                                temporals[i].granularity[0] ? temporals[i].granularity : "time",
                                temporals[i].ref_key, 1.6 + temporals[i].weight * 0.4);
         if (temporal_units[temporal_count] > 0)
            temporal_count++;
      }
   }

   {
      db2_memory_entity_row_t entities[16];
      int n = db2_memory_entities_list_weighted(memory_id, entities, 16);
      for (int i = 0; i < n && entity_count < 16; i++)
      {
         entity_units[entity_count] =
             memory_unit_insert(memory_id, "entity",
                                memory_unit_kind_resolve(explicit_kind, memory_kind, "entity",
                                                         event_count > 0, temporal_count > 0),
                                entities[i].role[0] ? entities[i].role : "mention",
                                entities[i].entity, 1.4 + entities[i].weight * 0.3);
         if (entity_units[entity_count] > 0)
            entity_count++;
      }
   }

   {
      db2_memory_chunk_row_t chunks[16];
      int n = db2_memory_chunks_list(memory_id, chunks, 16);
      for (int i = 0; i < n && chunk_count < 16; i++)
      {
         char ck[32];
         snprintf(ck, sizeof(ck), "chunk_%d", chunks[i].chunk_index);
         chunk_units[chunk_count] =
             memory_unit_insert(memory_id, "chunk",
                                memory_unit_kind_resolve(explicit_kind, memory_kind, "chunk",
                                                         event_count > 0, temporal_count > 0),
                                ck, chunks[i].chunk_text, 1.2);
         if (chunk_units[chunk_count] > 0)
            chunk_count++;
      }
   }

   if (content && content[0])
      episode_unit_id = memory_unit_insert(memory_id, "episode", MEMORY_UNIT_KIND_EPISODIC,
                                           key ? key : "episode", content, 1.8);

   for (int i = 0; i < summary_count; i++)
      for (int j = 0; j < event_count; j++)
         memory_unit_edge_insert(summary_units[i], event_units[j], "contains", 1.0);
   for (int i = 0; i < summary_count; i++)
      for (int j = 0; j < chunk_count; j++)
         memory_unit_edge_insert(summary_units[i], chunk_units[j], "contains", 0.8);
   for (int i = 0; i < event_count; i++)
   {
      for (int j = 0; j < temporal_count; j++)
         memory_unit_edge_insert(event_units[i], temporal_units[j], "anchored_by", 1.1);
      for (int j = 0; j < entity_count; j++)
         memory_unit_edge_insert(event_units[i], entity_units[j], "mentions", 1.0);
      for (int j = 0; j < chunk_count; j++)
         memory_unit_edge_insert(chunk_units[j], event_units[i], "supports", 0.7);
   }
   if (episode_unit_id > 0)
   {
      for (int i = 0; i < summary_count; i++)
         memory_unit_edge_insert(episode_unit_id, summary_units[i], "same_episode", 1.0);
      for (int i = 0; i < event_count; i++)
         memory_unit_edge_insert(episode_unit_id, event_units[i], "same_episode", 1.0);
      for (int i = 0; i < chunk_count; i++)
         memory_unit_edge_insert(episode_unit_id, chunk_units[i], "same_episode", 0.8);
   }

   if (episode_unit_id > 0 && source_session[0])
   {
      int64_t other_eps[64];
      int n = db2_memory_episode_unit_ids_for_session(memory_id, source_session, other_eps, 64);
      for (int i = 0; i < n; i++)
      {
         memory_unit_edge_insert(episode_unit_id, other_eps[i], "same_episode", 0.9);
         memory_unit_edge_insert(other_eps[i], episode_unit_id, "same_episode", 0.9);
      }
   }

   if (episode_unit_id > 0)
   {
      int64_t old_eps[64];
      int n = db2_memory_episode_unit_ids_supersedes(memory_id, old_eps, 64);
      for (int i = 0; i < n; i++)
         memory_unit_edge_insert(episode_unit_id, old_eps[i], "supersedes", 1.2);
   }
}

static int64_t memory_episode_insert(int64_t memory_id, const char *episode_key,
                                     const char *episode_text, const char *source_session,
                                     const char *reference_time)
{
   return db2_memory_episode_insert(memory_id, episode_key, episode_text, source_session,
                                    reference_time);
}

static void memory_relation_insert(int64_t memory_id, int64_t episode_id, const char *src_entity,
                                   const char *relation, const char *dst_entity,
                                   const char *fact_text, const char *valid_at,
                                   const char *invalid_at, double weight)
{
   db2_memory_relation_upsert_full(memory_id, episode_id, src_entity, relation, dst_entity,
                                   fact_text, valid_at, invalid_at, weight);
}

static void memory_lookup_primary_entity(int64_t memory_id, char *buf, size_t buf_len)
{
   db2_memory_lookup_primary_entity(memory_id, buf, (int)buf_len);
}

static void memory_lookup_time_bounds(int64_t memory_id, char *valid_at, size_t valid_len,
                                      char *invalid_at, size_t invalid_len)
{
   db2_memory_lookup_time_bounds(memory_id, valid_at, (int)valid_len, invalid_at, (int)invalid_len);
}

void memory_refresh_episode_relations(int64_t memory_id, const char *key, const char *content)
{
   if (memory_id <= 0)
      return;

   db2_memory_episodes_delete_for_memory(memory_id);
   db2_memory_relations_delete_for_memory(memory_id);

   char source_session[128];
   char valid_at[64];
   char invalid_at[64];
   source_session[0] = '\0';
   memory_lookup_time_bounds(memory_id, valid_at, sizeof(valid_at), invalid_at, sizeof(invalid_at));
   db2_memory_get_source_session(memory_id, source_session, (int)sizeof(source_session));

   int64_t episode_id = memory_episode_insert(memory_id, key ? key : "", content ? content : "",
                                              source_session, valid_at);

   db2_memory_summary_row_t summaries[2];
   int summary_n = db2_memory_summaries_list(memory_id, 2, summaries, 2);
   for (int i = 0; i < summary_n; i++)
   {
      if (summaries[i].summary[0])
         memory_episode_insert(memory_id,
                               summaries[i].scope[0] ? summaries[i].scope : (key ? key : ""),
                               summaries[i].summary, source_session, valid_at);
   }

   char primary_entity[128];
   memory_lookup_primary_entity(memory_id, primary_entity, sizeof(primary_entity));

   db2_memory_event_frame_row_t events[64];
   int event_n =
       db2_memory_event_frames_list(memory_id, events, (int)(sizeof(events) / sizeof(events[0])));
   for (int i = 0; i < event_n; i++)
   {
      const char *actor = events[i].actor;
      const char *action = events[i].action;
      const char *object = events[i].object;
      const char *location = events[i].location;
      const char *event_time = events[i].event_time;
      char fact[1024];
      snprintf(fact, sizeof(fact), "%s %s %s %s %s", actor, action, object, location, event_time);
      if (actor[0] && action[0] && object[0])
         memory_relation_insert(memory_id, episode_id, actor, action, object, fact,
                                event_time[0] ? event_time : valid_at, invalid_at, 2.4);
      if (actor[0] && location[0])
         memory_relation_insert(memory_id, episode_id, actor, "located_at", location, fact,
                                event_time[0] ? event_time : valid_at, invalid_at, 1.8);
      if (actor[0] && event_time[0])
         memory_relation_insert(memory_id, episode_id, actor, "occurred_at", event_time, fact,
                                event_time, invalid_at, 1.9);
   }

   db2_memory_link_target_row_t links[64];
   int link_n =
       db2_memory_links_with_targets(memory_id, links, (int)(sizeof(links) / sizeof(links[0])));
   for (int i = 0; i < link_n; i++)
   {
      const char *relation = links[i].relation;
      const char *target_key = links[i].target_key;
      const char *target_content = links[i].target_content;
      char fact[1024];
      snprintf(fact, sizeof(fact), "%s %s %s",
               primary_entity[0] ? primary_entity : (key ? key : ""), relation,
               target_content[0] ? target_content : target_key);
      memory_relation_insert(
          memory_id, episode_id, primary_entity[0] ? primary_entity : (key ? key : ""),
          relation[0] ? relation : "related_to", target_key[0] ? target_key : target_content, fact,
          valid_at, invalid_at, 1.3);
   }
}

static void memory_summary_insert(int64_t memory_id, const char *scope, const char *summary)
{
   db2_memory_summary_upsert(memory_id, scope, summary);
}

static int memory_is_summary_noise_token(const char *tok)
{
   if (!tok || !tok[0])
      return 1;
   return strcmp(tok, "user") == 0 || strcmp(tok, "assistant") == 0 || strcmp(tok, "system") == 0 ||
          strcmp(tok, "speaker") == 0;
}

static void memory_chunk_insert(int64_t memory_id, int chunk_index, const char *chunk)
{
   db2_memory_chunk_upsert(memory_id, chunk_index, chunk);
}

void memory_refresh_summaries(int64_t memory_id, const char *key, const char *content)
{
   db2_memory_summaries_delete_for_memory(memory_id);

   char norm[2048];
   char tokens[32][64];
   char summary[512];
   normalize_key(content && content[0] ? content : key, norm, sizeof(norm));
   int token_count = memory_split_tokens(norm, tokens, 32);
   if (token_count <= 0)
      return;

   int take = token_count < 20 ? token_count : 20;
   memory_alias_join_tokens(summary, sizeof(summary), tokens, 0, take);
   if (summary[0])
      memory_summary_insert(memory_id, "headline", summary);

   char concepts[512];
   char seen[24][64];
   int seen_count = 0;
   size_t used = 0;
   concepts[0] = '\0';
   int scan = token_count < 96 ? token_count : 96;
   for (int i = 0; i < scan && seen_count < 24; i++)
   {
      if (!memory_is_signal_token(tokens[i]) || memory_is_summary_noise_token(tokens[i]))
         continue;
      char canonical[64];
      memory_canonicalize_term(tokens[i], canonical, sizeof(canonical));
      const char *term = canonical[0] ? canonical : tokens[i];
      if (strlen(term) < 4)
         continue;
      int dup = 0;
      for (int j = 0; j < seen_count; j++)
      {
         if (strcmp(seen[j], term) == 0)
         {
            dup = 1;
            break;
         }
      }
      if (dup)
         continue;
      snprintf(seen[seen_count], sizeof(seen[seen_count]), "%s", term);
      seen_count++;
      int wrote = snprintf(concepts + used, sizeof(concepts) - used, "%s%s", used ? " " : "", term);
      if (wrote < 0 || (size_t)wrote >= sizeof(concepts) - used)
      {
         concepts[sizeof(concepts) - 1] = '\0';
         break;
      }
      used += (size_t)wrote;
   }
   if (concepts[0])
      memory_summary_insert(memory_id, "signals", concepts);
}

static void memory_event_frame_insert(int64_t memory_id, const char *actor, const char *action,
                                      const char *object, const char *location,
                                      const char *event_time, const char *evidence_kind)
{
   db2_memory_event_frame_insert(memory_id, actor, action, object, location, event_time,
                                 evidence_kind);
}

static int memory_extract_event_frame_fields(const char *text, const char *fallback_key,
                                             char *actor, size_t actor_len, char *action,
                                             size_t action_len, char *object, size_t object_len,
                                             char *location, size_t location_len, char *event_time,
                                             size_t event_time_len)
{
   char norm[2048];
   char tokens[32][64];
   int action_idx = -1;
   if (actor && actor_len)
      actor[0] = '\0';
   if (action && action_len)
      action[0] = '\0';
   if (object && object_len)
      object[0] = '\0';
   if (location && location_len)
      location[0] = '\0';
   if (event_time && event_time_len)
      event_time[0] = '\0';

   normalize_key(text && text[0] ? text : fallback_key, norm, sizeof(norm));
   int token_count = memory_split_tokens(norm, tokens, 32);
   if (token_count <= 0)
      return 0;

   for (int i = 0; i < token_count; i++)
   {
      if (action && !action[0] && memory_is_likely_action_token(tokens[i]))
      {
         snprintf(action, action_len, "%s", tokens[i]);
         action_idx = i;
      }

      if (location && !location[0] && memory_is_probable_location_token(tokens[i]))
         snprintf(location, location_len, "%s", tokens[i]);
      if (location && !location[0] && memory_is_relation_token(tokens[i]) && i + 1 < token_count)
      {
         int take = (i + 3 <= token_count && !memory_is_relation_token(tokens[i + 2])) ? 2 : 1;
         memory_alias_join_tokens(location, location_len, tokens, i + 1, take);
      }

      if (event_time && !event_time[0])
      {
         if (memory_is_month_token(tokens[i]) && i + 1 < token_count &&
             strlen(tokens[i + 1]) <= 4 && isdigit((unsigned char)tokens[i + 1][0]))
            memory_alias_join_tokens(event_time, event_time_len, tokens, i, 2);
         else if (memory_is_month_token(tokens[i]) || memory_is_weekday_token(tokens[i]) ||
                  strcmp(tokens[i], "today") == 0 || strcmp(tokens[i], "yesterday") == 0 ||
                  strcmp(tokens[i], "tomorrow") == 0 ||
                  (strlen(tokens[i]) == 4 && isdigit((unsigned char)tokens[i][0])))
            snprintf(event_time, event_time_len, "%s", tokens[i]);
      }
   }

   if (action_idx > 0)
   {
      for (int i = action_idx - 1; i >= 0; i--)
      {
         if (memory_is_stopword_token(tokens[i]) || memory_is_month_token(tokens[i]) ||
             memory_is_weekday_token(tokens[i]) || memory_is_relation_token(tokens[i]))
            continue;
         if (actor && !actor[0])
         {
            if (i > 0 && strlen(tokens[i - 1]) >= 3 && !memory_is_stopword_token(tokens[i - 1]) &&
                !memory_is_relation_token(tokens[i - 1]))
               memory_alias_join_tokens(actor, actor_len, tokens, i - 1, 2);
            else
               snprintf(actor, actor_len, "%s", tokens[i]);
         }
         break;
      }
      if (object && action_idx + 1 < token_count)
      {
         int start = action_idx + 1;
         while (start < token_count && (memory_is_stopword_token(tokens[start]) ||
                                        memory_is_relation_token(tokens[start])))
            start++;
         if (start < token_count && !memory_is_month_token(tokens[start]) &&
             !memory_is_weekday_token(tokens[start]))
         {
            int take = 1;
            if (start + 1 < token_count && !memory_is_stopword_token(tokens[start + 1]) &&
                !memory_is_relation_token(tokens[start + 1]) &&
                !memory_is_month_token(tokens[start + 1]))
               take = 2;
            memory_alias_join_tokens(object, object_len, tokens, start, take);
         }
      }
   }

   if (actor && !actor[0] && fallback_key && fallback_key[0])
      snprintf(actor, actor_len, "%s", fallback_key);
   if (action && !action[0])
      snprintf(action, action_len, "%s", "state");
   if (object && !object[0] && fallback_key && fallback_key[0])
      snprintf(object, object_len, "%s", fallback_key);
   return 1;
}

void memory_refresh_event_frames(int64_t memory_id, const char *key, const char *content)
{
   db2_memory_event_frames_delete_for_memory(memory_id);

   if (!content || !content[0])
   {
      char actor[128], action[128], object[256], location[128], event_time[128];
      if (memory_extract_event_frame_fields(key, key, actor, sizeof(actor), action, sizeof(action),
                                            object, sizeof(object), location, sizeof(location),
                                            event_time, sizeof(event_time)))
         memory_event_frame_insert(memory_id, actor, action, object, location, event_time,
                                   "derived");
      return;
   }

   char chunk[512];
   int frame_count = 0;
   size_t used = 0;
   for (size_t i = 0;; i++)
   {
      char ch = content[i];
      if (ch == '\0' || ch == '.' || ch == '!' || ch == '?' || ch == ';' || ch == '\n')
      {
         while (used > 0 && isspace((unsigned char)chunk[used - 1]))
            used--;
         chunk[used] = '\0';
         if (used > 0)
         {
            char actor[128], action[128], object[256], location[128], event_time[128];
            if (memory_extract_event_frame_fields(chunk, key, actor, sizeof(actor), action,
                                                  sizeof(action), object, sizeof(object), location,
                                                  sizeof(location), event_time, sizeof(event_time)))
            {
               memory_event_frame_insert(memory_id, actor, action, object, location, event_time,
                                         frame_count == 0 ? "derived" : "chunk");
               frame_count++;
            }
         }
         used = 0;
         if (ch == '\0' || frame_count >= 8)
            break;
         continue;
      }
      if (used + 1 < sizeof(chunk))
         chunk[used++] = ch;
   }
}

/* Token-window chunker: splits content into ~50-token windows with 10-token
 * overlap.  Punctuation (. ! ? ; \n) is used as a preferred break point but
 * is not required.  Falls back to a hard split at the window boundary when
 * there is no sentence break within the window.  Hard limit: 8 chunks. */
void memory_refresh_chunks(int64_t memory_id, const char *content)
{
   db2_memory_chunks_delete_for_memory(memory_id);

   if (!content || !content[0])
      return;

   enum
   {
      CHUNK_TOKENS = 50,
      OVERLAP_TOKENS = 10,
      MAX_CHUNKS = 8
   };
   /* Rough token estimate: 4 chars per token */
   enum
   {
      CHUNK_CHARS = CHUNK_TOKENS * 4,
      OVERLAP_CHARS = OVERLAP_TOKENS * 4
   };
   char chunk[512];
   int ci = 0;
   size_t clen = strlen(content);
   size_t pos = 0; /* current read position in content */

   while (pos < clen && ci < MAX_CHUNKS)
   {
      /* Fill up to CHUNK_CHARS characters; prefer a sentence break */
      size_t end = pos + CHUNK_CHARS;
      if (end >= clen)
         end = clen;

      /* Look for a preferred break (punctuation) within [pos, end] */
      size_t break_at = end;
      if (end < clen)
      {
         /* Scan backwards from end for punctuation */
         for (size_t k = end; k > pos; k--)
         {
            char c = content[k - 1];
            if (c == '.' || c == '!' || c == '?' || c == ';' || c == '\n')
            {
               break_at = k;
               break;
            }
         }
      }

      /* Extract chunk text */
      size_t chunk_len = break_at - pos;
      if (chunk_len == 0)
         break;
      if (chunk_len > sizeof(chunk) - 1)
         chunk_len = sizeof(chunk) - 1;

      memcpy(chunk, content + pos, chunk_len);
      /* Trim trailing whitespace */
      while (chunk_len > 0 && isspace((unsigned char)chunk[chunk_len - 1]))
         chunk_len--;
      chunk[chunk_len] = '\0';

      if (chunk_len > 0)
         memory_chunk_insert(memory_id, ci++, chunk);

      /* Advance, keeping overlap */
      if (break_at >= clen)
         break;
      pos = break_at > OVERLAP_CHARS ? break_at - OVERLAP_CHARS : 0;
      /* Ensure forward progress */
      if (pos <= (break_at > CHUNK_CHARS ? break_at - CHUNK_CHARS : 0))
         pos = break_at;
   }

   if (ci == 0)
      memory_chunk_insert(memory_id, 0, content);
}

void memory_refresh_aliases(int64_t memory_id, const char *key, const char *content)
{
   if (memory_id <= 0)
      return;
   db2_memory_aliases_delete_for_memory(memory_id);

   if (key && key[0])
      memory_alias_insert(memory_id, key, 4.0);

   if (!content || !content[0])
      return;

   char norm_content[2048];
   char tokens[16][64];
   normalize_key(content, norm_content, sizeof(norm_content));
   int token_count = memory_split_tokens(norm_content, tokens, 16);
   if (token_count <= 0)
      return;

   char alias[256];
   int emitted = 0;

   for (int span = token_count >= 6 ? 6 : token_count; span >= 3 && emitted < 24; span--)
   {
      memory_alias_join_tokens(alias, sizeof(alias), tokens, 0, span);
      if (alias[0])
      {
         memory_alias_insert(memory_id, alias, 3.0 + (double)span * 0.1);
         emitted++;
      }
   }

   for (int i = 0; i < token_count - 1 && emitted < 24; i++)
   {
      if (!memory_alias_is_useful_token(tokens[i]) || !memory_alias_is_useful_token(tokens[i + 1]))
         continue;
      memory_alias_join_tokens(alias, sizeof(alias), tokens, i, 2);
      if (alias[0])
      {
         memory_alias_insert(memory_id, alias, 2.0);
         emitted++;
      }
   }

   for (int i = 0; i < token_count - 2 && emitted < 24; i++)
   {
      if (!memory_alias_is_useful_token(tokens[i]) ||
          !memory_alias_is_useful_token(tokens[i + 1]) ||
          !memory_alias_is_useful_token(tokens[i + 2]))
         continue;
      memory_alias_join_tokens(alias, sizeof(alias), tokens, i, 3);
      if (alias[0])
      {
         memory_alias_insert(memory_id, alias, 2.5);
         emitted++;
      }
   }

   for (int i = 0; i < token_count && emitted < 24; i++)
   {
      if (!memory_alias_is_useful_token(tokens[i]))
         continue;
      memory_alias_insert(memory_id, tokens[i], 1.0);
      emitted++;
   }
}
