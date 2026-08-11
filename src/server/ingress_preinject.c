/* server/ingress_preinject.c: see ingress_preinject.h.
 *
 * The envelope is a compact, model-readable block. Its `explore-with` line
 * names Aimee's own retrieval tools so a co-registered agent fills any gap
 * THROUGH Aimee (symbol-scoped, graph-aware) instead of raw-grepping the tree.
 */
#include "ingress_preinject.h"
#include "config.h"
#include "kb_client.h"
#include "retrieval_outcome_bridge.h"
#include "dstr.h"
#include "log.h"
#include "request_context.h"
#include "platform_random.h"
#include "agent_code_capabilities.h"
#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* The standing guidance is NOT defined here any more -- see
 * headers/aimee_session_guidance.h. It was written out here AND in
 * cli_session_start.c, and the two copies drifted: the CLI one lacked memory_get
 * and the whole fix-scope line. One policy, one definition, every transport. */

#define INGRESS_AUDIT_CONTEXT_FILE            "audit_context.txt"
#define INGRESS_AUDIT_CONTEXT_MAX_AGE_SECONDS (6 * 60 * 60)
#define INGRESS_DEFAULT_ASSEMBLY_BUDGET       6144
#define INGRESS_FOOTER_RESERVE_BYTES          384

/* Per-request disable, set by the HTTP layer from the `x-aimee-preinject: 0`
 * header. Thread-local: the ingress runs the turn synchronously on the request
 * thread, so this is read by ingress_preinject_build() during the same request. */
static __thread int g_request_disabled = 0;

void ingress_preinject_set_request_disabled(int disabled)
{
   g_request_disabled = disabled ? 1 : 0;
}

/* Per-turn retrieval-event id (auditable-correctness P1). Thread-local for the
 * same reason as the disable override: the ingress runs synchronously on the
 * request thread. A UUID is 36 chars; 40 leaves room for the NUL. */
static __thread char g_turn_id[40] = "";

void ingress_preinject_mint_turn_id(char *buf, size_t len)
{
   if (!buf || len == 0)
      return;
   unsigned char raw[16];
   if (platform_random_bytes(raw, sizeof(raw)) != 0)
      memset(raw, 0, sizeof(raw));
   snprintf(buf, len, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10],
            raw[11], raw[12], raw[13], raw[14], raw[15]);
}

void ingress_preinject_set_turn_id(const char *turn_id)
{
   if (turn_id && turn_id[0])
      snprintf(g_turn_id, sizeof(g_turn_id), "%s", turn_id);
   else
      g_turn_id[0] = '\0';
}

const char *ingress_preinject_turn_id(void)
{
   return g_turn_id;
}

static __thread char g_session_id[64] = "";

#define INGRESS_TASK_SESSION_SLOTS 64

typedef struct
{
   char session[64];
   char project[256];
   uint64_t token_bits;
   uint64_t used_at;
} ingress_task_session_t;

static ingress_task_session_t g_task_sessions[INGRESS_TASK_SESSION_SLOTS];
static pthread_mutex_t g_task_sessions_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_task_sessions_clock;

void ingress_preinject_set_session_id(const char *session_id)
{
   if (session_id && session_id[0])
      snprintf(g_session_id, sizeof(g_session_id), "%s", session_id);
   else
      g_session_id[0] = '\0';
}

const char *ingress_preinject_session_id(void)
{
   return g_session_id;
}

void ingress_preinject_task_state_reset(void)
{
   pthread_mutex_lock(&g_task_sessions_mu);
   memset(g_task_sessions, 0, sizeof(g_task_sessions));
   g_task_sessions_clock = 0;
   pthread_mutex_unlock(&g_task_sessions_mu);
}

static uint64_t ingress_task_token_bits(const char *query)
{
   uint64_t bits = 0;
   const unsigned char *p = (const unsigned char *)(query ? query : "");
   while (*p)
   {
      while (*p && !isalnum(*p) && *p != '_')
         p++;
      uint64_t h = 1469598103934665603ULL;
      int n = 0;
      while (*p && (isalnum(*p) || *p == '_'))
      {
         h ^= (uint64_t)tolower(*p++);
         h *= 1099511628211ULL;
         n++;
      }
      if (n >= 2)
         bits |= 1ULL << (h & 63U);
   }
   return bits ? bits : 1ULL;
}

static int ingress_preinject_first_task_turn(const char *session, const char *project,
                                             const char *query)
{
   if (!session || !session[0] || !project || !project[0])
      return 0;
   uint64_t bits = ingress_task_token_bits(query);
   int fetch = 0;
   pthread_mutex_lock(&g_task_sessions_mu);
   int slot = -1;
   int oldest = 0;
   for (int i = 0; i < INGRESS_TASK_SESSION_SLOTS; i++)
   {
      if (!g_task_sessions[i].session[0])
      {
         if (slot < 0)
            slot = i;
         continue;
      }
      if (strcmp(g_task_sessions[i].session, session) == 0 &&
          strcmp(g_task_sessions[i].project, project) == 0)
      {
         slot = i;
         break;
      }
      if (g_task_sessions[i].used_at < g_task_sessions[oldest].used_at)
         oldest = i;
   }
   if (slot < 0)
      slot = oldest;
   ingress_task_session_t *state = &g_task_sessions[slot];
   if (!state->session[0] || strcmp(state->session, session) != 0 ||
       strcmp(state->project, project) != 0)
      fetch = 1;
   else
   {
      unsigned common = (unsigned)__builtin_popcountll(state->token_bits & bits);
      unsigned total = (unsigned)__builtin_popcountll(state->token_bits | bits);
      /* A low-overlap turn is a new task. Follow-ups usually retain at least a
       * third of their salient vocabulary; the bounded bitset intentionally
       * errs toward observing rather than injecting on every wording change. */
      fetch = total == 0 || (common * 3U < total);
   }
   snprintf(state->session, sizeof(state->session), "%s", session);
   snprintf(state->project, sizeof(state->project), "%s", project);
   state->token_bits = bits;
   state->used_at = ++g_task_sessions_clock;
   pthread_mutex_unlock(&g_task_sessions_mu);
   return fetch;
}

/* Turns whose memory recall could not reach the knowledge service, as opposed to
 * turns that legitimately recalled nothing. Process-local and monotonic; read
 * through ingress_preinject_recall_unavailable_total(). A non-zero and climbing
 * value means agents are being handed envelopes with no memory previews because
 * the dependency is down -- which reads identically to "nothing to recall" at
 * every surface unless something counts it. */
static long long ingress_recall_unavailable_total = 0;

long long ingress_preinject_recall_unavailable_total(void)
{
   return ingress_recall_unavailable_total;
}

/* A first/new-task marker is claimed before retrieval so concurrent turns do
 * not duplicate packets. If that one retrieval never reached the dependency,
 * remove only its exact session/project marker: a related follow-up may then
 * use the breaker's single recovery probe without restarting the client. */
static void ingress_preinject_rearm_unavailable(const char *session, const char *project)
{
   if (!session || !session[0] || !project || !project[0])
      return;
   pthread_mutex_lock(&g_task_sessions_mu);
   for (int i = 0; i < INGRESS_TASK_SESSION_SLOTS; i++)
      if (strcmp(g_task_sessions[i].session, session) == 0 &&
          strcmp(g_task_sessions[i].project, project) == 0)
      {
         memset(&g_task_sessions[i], 0, sizeof(g_task_sessions[i]));
         break;
      }
   pthread_mutex_unlock(&g_task_sessions_mu);
}

static long ingress_elapsed_ms(const struct timespec *start, const struct timespec *end)
{
   return (long)(end->tv_sec - start->tv_sec) * 1000L +
          (long)(end->tv_nsec - start->tv_nsec) / 1000000L;
}

static ingress_confidence_provider_fn g_confidence_provider;

void ingress_preinject_register_confidence_provider(ingress_confidence_provider_fn provider)
{
   g_confidence_provider = provider;
}

/* A stable, non-reversible fingerprint of the turn query (FNV-1a 64-bit, hex).
 * Recorded on the retrieval_event instead of the raw prompt so the audit row
 * correlates turns (same query → same fingerprint) without persisting user
 * prompt text. The /v1/audit/trace read never surfaces the query, so a hash
 * loses nothing for reconstructibility. */
static void ingress_query_fingerprint(const char *q, char *out, size_t len)
{
   uint64_t h = 1469598103934665603ULL; /* FNV-1a offset basis */
   for (const unsigned char *p = (const unsigned char *)(q ? q : ""); *p; p++)
   {
      h ^= (uint64_t)*p;
      h *= 1099511628211ULL; /* FNV-1a prime */
   }
   snprintf(out, len, "q:%016llx", (unsigned long long)h);
}

static int ingress_confidence_valid(const char *confidence)
{
   return confidence && (strcmp(confidence, "high") == 0 || strcmp(confidence, "medium") == 0 ||
                         strcmp(confidence, "low") == 0);
}

int ingress_preinject_confidence(double top_score, const char **confidence)
{
   if (!confidence)
      return -1;
   *confidence = NULL;
   const char *value = NULL;
   if (!g_confidence_provider || g_confidence_provider(top_score, &value) != 0 ||
       !ingress_confidence_valid(value))
      return -1;
   *confidence = value;
   return 0;
}

char *ingress_preinject_format_envelope(const char *context_block, const char *confidence)
{
   if (!context_block)
      return NULL;
   /* Treat a whitespace-only block as empty → no envelope. */
   const char *p = context_block;
   while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
      p++;
   if (*p == '\0')
      return NULL;

   if (!ingress_confidence_valid(confidence))
      return NULL;

   dstr_t d;
   dstr_init(&d);
   dstr_appendf(&d, "<aimee-context confidence=\"%s\">\n", confidence);
   dstr_append_str(&d, context_block);
   if (context_block[strlen(context_block) - 1] != '\n')
      dstr_append_str(&d, "\n");
   dstr_append_str(&d, "</aimee-context>");
   char *out = dstr_steal(&d);
   return out;
}

char *ingress_preinject_format_code_block(const code_search_hit_t *hits, int n)
{
   if (!hits || n <= 0)
      return NULL;
   dstr_t d;
   dstr_init(&d);
   dstr_append_str(&d, "recommended (code):\n");
   for (int i = 0; i < n; i++)
   {
      dstr_appendf(&d, "  - %s\n", hits[i].file_path);
      /* One trimmed, single-line snippet so the agent sees why the file matched
       * without paying for the whole match. Collapse whitespace runs. */
      const char *s = hits[i].snippet;
      if (s && s[0])
      {
         char line[160];
         int j = 0;
         for (const char *p = s; *p && j < (int)sizeof(line) - 1; p++)
         {
            char c = (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
            if (c == ' ' && (j == 0 || line[j - 1] == ' '))
               continue; /* skip leading / collapsed spaces */
            line[j++] = c;
         }
         while (j > 0 && line[j - 1] == ' ')
            j--;
         line[j] = '\0';
         if (line[0])
            dstr_appendf(&d, "    > %s\n", line);
      }
   }
   return dstr_steal(&d);
}

static void append_single_line_escaped(dstr_t *d, const char *s, size_t max_chars)
{
   size_t written = 0;
   int prev_space = 0;
   for (const char *p = s ? s : ""; *p && written < max_chars; p++)
   {
      unsigned char uc = (unsigned char)*p;
      char c = (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
      if (uc < 32 && c != ' ')
         continue;
      if (c == ' ' && prev_space)
         continue;
      if (c == '<')
      {
         dstr_append_str(d, "&lt;");
         written += 4;
      }
      else if (c == '>')
      {
         dstr_append_str(d, "&gt;");
         written += 4;
      }
      else if (c == '&')
      {
         dstr_append_str(d, "&amp;");
         written += 5;
      }
      else
      {
         dstr_append_char(d, c);
         written++;
      }
      prev_space = (c == ' ');
   }
}

static int context_string_eq(const cJSON *object, const char *field, const char *expected)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, field);
   return cJSON_IsString(value) && value->valuestring && strcmp(value->valuestring, expected) == 0;
}

static int context_number_positive(const cJSON *object, const char *field, long long *out)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, field);
   if (!cJSON_IsNumber(value) || value->valuedouble <= 0)
      return 0;
   if (out)
      *out = (long long)value->valuedouble;
   return 1;
}

static int context_number_eq(const cJSON *object, const char *field, int expected)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, field);
   return cJSON_IsNumber(value) && value->valuedouble == (double)expected;
}

static int context_provenance_allowed(const char *value)
{
   return value && (strcmp(value, "code") == 0 || strcmp(value, "graph") == 0 ||
                    strcmp(value, "vector") == 0 || strcmp(value, "memory") == 0);
}

char *ingress_preinject_format_task_context(const char *json, const char *active_project,
                                            int *item_count_out, double *confidence_out)
{
   if (item_count_out)
      *item_count_out = 0;
   if (confidence_out)
      *confidence_out = 0.0;
   if (!json || !active_project || !active_project[0])
      return NULL;

   cJSON *root = cJSON_Parse(json);
   long long generation = 0;
   const cJSON *answerability =
       root ? cJSON_GetObjectItemCaseSensitive(root, "answerability") : NULL;
   const cJSON *results = root ? cJSON_GetObjectItemCaseSensitive(root, "results") : NULL;
   const cJSON *why = root ? cJSON_GetObjectItemCaseSensitive(root, "why") : NULL;
   if (!root || !context_string_eq(root, "status", "ok") ||
       !context_string_eq(root, "project", active_project) ||
       !context_string_eq(root, "freshness", "current") ||
       !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "resolved")) ||
       !context_number_positive(root, "generation", &generation) ||
       !cJSON_IsObject(answerability) ||
       !context_string_eq(answerability, "decision", "answerable") || !cJSON_IsArray(results) ||
       !cJSON_IsArray(why) || cJSON_GetArraySize(results) < 1 || cJSON_GetArraySize(results) > 4 ||
       cJSON_GetArraySize(results) + cJSON_GetArraySize(why) > 4 ||
       !context_number_eq(root, "max_results", 4) || !context_number_eq(root, "max_tokens", 1200) ||
       !context_number_eq(root, "item_count",
                          cJSON_GetArraySize(results) + cJSON_GetArraySize(why)))
   {
      cJSON_Delete(root);
      return NULL;
   }

   dstr_t block;
   dstr_init(&block);
   dstr_append_str(&block, "recommended (task-conditioned code; project=");
   append_single_line_escaped(&block, active_project, 512);
   dstr_appendf(&block, "; generation=%lld):\n", generation);
   int kept = 0;
   double top = 0.0;
   const cJSON *row = NULL;
   cJSON_ArrayForEach(row, results)
   {
      const cJSON *path = cJSON_GetObjectItemCaseSensitive(row, "file_path");
      const cJSON *confidence = cJSON_GetObjectItemCaseSensitive(row, "confidence");
      const cJSON *accepted = cJSON_GetObjectItemCaseSensitive(row, "accepted");
      const cJSON *provenance = cJSON_GetObjectItemCaseSensitive(row, "provenance");
      const cJSON *span = cJSON_GetObjectItemCaseSensitive(row, "span");
      long long row_generation = 0;
      const cJSON *line_start =
          cJSON_IsObject(span) ? cJSON_GetObjectItemCaseSensitive(span, "line_start") : NULL;
      const cJSON *line_end =
          cJSON_IsObject(span) ? cJSON_GetObjectItemCaseSensitive(span, "line_end") : NULL;
      int line_span = cJSON_IsNumber(line_start) && line_start->valueint > 0;
      if (!cJSON_IsString(path) || !path->valuestring[0] ||
          !context_string_eq(row, "project", active_project) ||
          !context_string_eq(row, "freshness", "current") ||
          !context_number_positive(row, "generation", &row_generation) ||
          row_generation != generation || !cJSON_IsTrue(accepted) || !cJSON_IsNumber(confidence) ||
          confidence->valuedouble <= 0.0 || confidence->valuedouble > 1.0 ||
          !cJSON_IsArray(provenance) || cJSON_GetArraySize(provenance) < 1 ||
          !cJSON_IsObject(span) || !cJSON_IsNumber(line_start) || !cJSON_IsNumber(line_end) ||
          !context_string_eq(span, "kind", line_span ? "line" : "file") ||
          (line_span && line_end->valueint < line_start->valueint) ||
          (!line_span && (line_start->valueint != 0 || line_end->valueint != 0)))
      {
         dstr_free(&block);
         cJSON_Delete(root);
         return NULL;
      }

      dstr_t item;
      dstr_init(&item);
      dstr_append_str(&item, "  - ");
      append_single_line_escaped(&item, path->valuestring, 512);
      if (line_span)
         dstr_appendf(&item, ":%d", line_start->valueint);
      dstr_appendf(&item, " [confidence=%.2f; provenance=", confidence->valuedouble);
      const cJSON *signal = NULL;
      int signal_i = 0;
      cJSON_ArrayForEach(signal, provenance)
      {
         if (!cJSON_IsString(signal) || !context_provenance_allowed(signal->valuestring))
         {
            dstr_free(&item);
            dstr_free(&block);
            cJSON_Delete(root);
            return NULL;
         }
         if (signal_i++)
            dstr_append_char(&item, ',');
         append_single_line_escaped(&item, signal->valuestring, 32);
      }
      dstr_append_str(&item, "]\n");
      const cJSON *snippet = cJSON_GetObjectItemCaseSensitive(row, "snippet");
      if (cJSON_IsString(snippet) && snippet->valuestring[0])
      {
         dstr_append_str(&item, "    > ");
         append_single_line_escaped(&item, snippet->valuestring, 480);
         dstr_append_char(&item, '\n');
      }
      if (dstr_len(&block) + dstr_len(&item) > 4800)
      {
         dstr_free(&item);
         break;
      }
      dstr_append(&block, dstr_cstr(&item), dstr_len(&item));
      dstr_free(&item);
      kept++;
      if (confidence->valuedouble > top)
         top = confidence->valuedouble;
   }

   const cJSON *memory = NULL;
   cJSON_ArrayForEach(memory, why)
   {
      if (kept >= 4)
         break;
      const cJSON *anchor = cJSON_GetObjectItemCaseSensitive(memory, "anchor");
      const cJSON *content = cJSON_GetObjectItemCaseSensitive(memory, "content");
      const cJSON *confidence = cJSON_GetObjectItemCaseSensitive(memory, "confidence");
      const cJSON *memory_id = cJSON_GetObjectItemCaseSensitive(memory, "memory_id");
      long long anchor_generation = 0;
      if (!cJSON_IsObject(anchor) || !context_string_eq(anchor, "project", active_project) ||
          !context_string_eq(anchor, "freshness", "current") ||
          !context_number_positive(anchor, "generation", &anchor_generation) ||
          anchor_generation != generation || !cJSON_IsString(content) || !content->valuestring[0] ||
          !context_string_eq(memory, "scope", "project") ||
          !context_string_eq(memory, "provenance", "memory") || !cJSON_IsNumber(memory_id) ||
          memory_id->valuedouble <= 0 || !cJSON_IsNumber(confidence) ||
          confidence->valuedouble <= 0 || confidence->valuedouble > 1.0)
      {
         dstr_free(&block);
         cJSON_Delete(root);
         return NULL;
      }
      dstr_t item;
      dstr_init(&item);
      dstr_appendf(&item, "  - memory[project; confidence=%.2f] anchored to ",
                   confidence->valuedouble);
      const cJSON *anchor_path = cJSON_GetObjectItemCaseSensitive(anchor, "file_path");
      if (!cJSON_IsString(anchor_path) || !anchor_path->valuestring[0])
      {
         dstr_free(&item);
         dstr_free(&block);
         cJSON_Delete(root);
         return NULL;
      }
      append_single_line_escaped(&item, anchor_path->valuestring, 256);
      dstr_append_str(&item, ": ");
      append_single_line_escaped(&item, content->valuestring, 320);
      dstr_append_char(&item, '\n');
      if (dstr_len(&block) + dstr_len(&item) > 4800)
      {
         dstr_free(&item);
         break;
      }
      dstr_append(&block, dstr_cstr(&item), dstr_len(&item));
      dstr_free(&item);
      kept++;
   }
   cJSON_Delete(root);
   if (kept == 0)
   {
      dstr_free(&block);
      return NULL;
   }
   if (item_count_out)
      *item_count_out = kept;
   if (confidence_out)
      *confidence_out = top;
   return dstr_steal(&block);
}

char *ingress_render_block(const ingress_entry_t *entries, int count, size_t envelope_budget,
                           int headline_missing_count, int *omitted_count_out)
{
   /* Reserve the same footer headroom the inline builder did; the per-candidate
    * budget gate, group headers, separators, footer, and truncation note below
    * reproduce the old rendering byte for byte. */
   size_t block_budget = envelope_budget > INGRESS_FOOTER_RESERVE_BYTES
                             ? envelope_budget - INGRESS_FOOTER_RESERVE_BYTES
                             : 0;
   dstr_t block;
   dstr_init(&block);
   int omitted = 0;
   /* have_prev guards prev_kind, so its seed is never read before the first entry
    * sets it; group_first tracks whether this group's header still needs to land. */
   bool have_prev = false;
   ingress_source_kind_t prev_kind = ING_SRC_CODE;
   bool group_first = true;

   for (int i = 0; i < count; i++)
   {
      const ingress_entry_t *e = &entries[i];
      if (!have_prev || e->kind != prev_kind)
      {
         /* A new group: one blank line separates it from a non-empty block,
          * exactly the old per-group `if (block.len) "\n"`. */
         if (dstr_len(&block) > 0)
            dstr_append_str(&block, "\n");
         group_first = true;
         prev_kind = e->kind;
         have_prev = true;
      }

      dstr_t cand;
      dstr_init(&cand);
      if (group_first && e->header)
         dstr_append_str(&cand, e->header);
      if (e->preview)
         dstr_append_str(&cand, e->preview);
      char *c = dstr_steal(&cand);

      if (c && c[0])
      {
         /* The header rides the first candidate that actually fits — the old
          * `wrote_header`, set only on a successful append. */
         if (dstr_len(&block) + strlen(c) <= block_budget)
         {
            dstr_append_str(&block, c);
            group_first = false;
         }
         else
         {
            omitted++;
         }
      }
      free(c);
   }

   if (dstr_len(&block) > 0)
   {
      char footer[256];
      snprintf(footer, sizeof(footer),
               "context-budget: used_bytes=%zu budget_bytes=%zu omitted_count=%d "
               "headline_missing_count=%d\n",
               dstr_len(&block), envelope_budget, omitted, headline_missing_count);
      if (dstr_len(&block) + strlen(footer) <= block_budget)
         dstr_append_str(&block, footer);
      if (omitted > 0)
      {
         char trunc[128];
         snprintf(trunc, sizeof(trunc),
                  "... (%d more available via get_context_block or memory_get)\n", omitted);
         if (dstr_len(&block) + strlen(trunc) <= block_budget)
            dstr_append_str(&block, trunc);
      }
   }

   if (omitted_count_out)
      *omitted_count_out = omitted;
   return dstr_steal(&block);
}

/* Per-record body for a code hit (no group header — the IR entry carries that). */
static char *format_code_body(const code_search_hit_t *hit)
{
   dstr_t d;
   dstr_init(&d);
   dstr_appendf(&d, "  - %s\n", hit->file_path);
   if (hit->snippet[0])
   {
      dstr_append_str(&d, "    > ");
      append_single_line_escaped(&d, hit->snippet, 150);
      dstr_append_str(&d, "\n");
   }
   return dstr_steal(&d);
}

/* Folded (lossy) body for a code hit (ingress-compression P1b): the snippet is
 * dropped in favour of a compact `file:line` reference the model recovers in full
 * via the code_span_get resolver named in the envelope's explore-with line. */
static char *format_code_fold(const code_search_hit_t *hit)
{
   dstr_t d;
   dstr_init(&d);
   dstr_appendf(&d, "  - %s:%d\n", hit->file_path, hit->line);
   return dstr_steal(&d);
}

/* Per-record body for a memory preview (no group header). Returns NULL for an
 * empty row (id <= 0) — the old NULL candidate, which produced no output and was
 * not counted. Bumps *headline_missing when the row carries no headline. */
static char *format_memory_preview_body(const memory_diagnostic_t *diag, int *headline_missing)
{
   const memory_t *m = &diag->memory;
   if (m->id <= 0)
      return NULL;

   const char *preview = m->headline[0] ? m->headline : m->content;
   int missing = m->headline[0] ? 0 : 1;
   if (missing && headline_missing)
      (*headline_missing)++;

   dstr_t d;
   dstr_init(&d);
   dstr_appendf(&d, "  - memory:%lld", (long long)m->id);
   if (m->key[0])
   {
      dstr_append_str(&d, " ");
      append_single_line_escaped(&d, m->key, 80);
   }
   dstr_appendf(&d, " [%s/%s score=%.3f headline_missing=%s]\n", m->tier[0] ? m->tier : "?",
                m->kind[0] ? m->kind : "memory", diag->parts.total, missing ? "true" : "false");
   if (preview && preview[0])
   {
      dstr_append_str(&d, "    > ");
      append_single_line_escaped(&d, preview, 220);
      dstr_append_str(&d, "\n");
   }
   return dstr_steal(&d);
}

static char *ingress_preinject_read_audit_context(void)
{
   const char *dir = config_default_dir();
   if (!dir || !dir[0])
      return NULL;
   char path[4096];
   int n = snprintf(path, sizeof(path), "%s/%s", dir, INGRESS_AUDIT_CONTEXT_FILE);
   if (n < 0 || (size_t)n >= sizeof(path))
      return NULL;
   struct stat st;
   if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
      return NULL;
   time_t now = time(NULL);
   if (now == (time_t)-1 || st.st_mtime > now ||
       now - st.st_mtime > INGRESS_AUDIT_CONTEXT_MAX_AGE_SECONDS)
      return NULL;

   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   char *buf = malloc(2048);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t got = fread(buf, 1, 2047, f);
   fclose(f);
   buf[got] = '\0';
   if (got == 0)
   {
      free(buf);
      return NULL;
   }
   return buf;
}

/* Pull the text out of a message `content` that is either a JSON string or an
 * array of {type, text} parts (the Responses content shape). Appends into d. */
static void append_content_text(dstr_t *d, const cJSON *content)
{
   if (cJSON_IsString(content))
   {
      dstr_append_str(d, content->valuestring);
      return;
   }
   if (cJSON_IsArray(content))
   {
      const cJSON *part = NULL;
      cJSON_ArrayForEach(part, content)
      {
         const cJSON *t = cJSON_GetObjectItemCaseSensitive(part, "text");
         if (cJSON_IsString(t))
            dstr_append_str(d, t->valuestring);
      }
   }
}

char *ingress_preinject_query_from_messages(const cJSON *messages)
{
   if (!cJSON_IsArray(messages))
      return NULL;

   /* Walk to the LAST user-role message — that is the current turn's ask. */
   const cJSON *msg = NULL;
   const cJSON *last_user = NULL;
   cJSON_ArrayForEach(msg, messages)
   {
      const cJSON *role = cJSON_GetObjectItemCaseSensitive(msg, "role");
      if (cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0)
         last_user = msg;
   }
   if (!last_user)
      return NULL;

   dstr_t d;
   dstr_init(&d);
   append_content_text(&d, cJSON_GetObjectItemCaseSensitive(last_user, "content"));
   char *out = dstr_steal(&d);
   if (out && out[0] == '\0')
   {
      free(out);
      return NULL;
   }
   return out;
}

char *ingress_preinject_last_assistant_from_messages(const cJSON *messages)
{
   if (!cJSON_IsArray(messages))
      return NULL;

   /* The LAST assistant-role message is the PRIOR turn's answer (the current
    * turn's answer does not exist yet). Used by the retrieval-outcome bridge to
    * attribute per-document overlap. */
   const cJSON *msg = NULL;
   const cJSON *last_assistant = NULL;
   cJSON_ArrayForEach(msg, messages)
   {
      const cJSON *role = cJSON_GetObjectItemCaseSensitive(msg, "role");
      if (cJSON_IsString(role) && strcmp(role->valuestring, "assistant") == 0)
         last_assistant = msg;
   }
   if (!last_assistant)
      return NULL;

   dstr_t d;
   dstr_init(&d);
   append_content_text(&d, cJSON_GetObjectItemCaseSensitive(last_assistant, "content"));
   char *out = dstr_steal(&d);
   if (out && out[0] == '\0')
   {
      free(out);
      return NULL;
   }
   return out;
}

char *ingress_preinject_build(const char *query, int request_disabled)
{
   if (request_disabled || g_request_disabled)
      return NULL;
   if (!query || !query[0])
      return NULL;
   /* The envelope carries two independently-gated layers: the code/memory preview
    * block (ingress_preinject_enabled, aimed at coding agents) and the typed-fact
    * block. Typed facts are KB-OWNED (proposal §8): aimee-server does NOT read its
    * own typed_facts_enabled — it asks the KB (cached capability) so the KB is the
    * single source of truth. Build if EITHER layer is on. */
   int preview_configured = config_ingress_preinject_enabled();
   int facts_configured = kb_client_typed_facts_enabled();
   char active_workspace[512] = "";
   char active_project[512] = "";
   int active_scope =
       ingress_preinject_resolve_active_scope(active_workspace, sizeof(active_workspace),
                                              active_project, sizeof(active_project)) == 0;
   /* Agent ingress is deliberately fail-closed without an active repository:
    * neither code nor memory may silently broaden to global recall. */
   int preview_on = preview_configured && active_scope;
   int facts_on = facts_configured && active_scope;

   const char *mode_name = config_code_context_mode();
   int context_mode = 1; /* invalid/blank values fail safely to observe */
   if (mode_name && strcmp(mode_name, "off") == 0)
      context_mode = 0;
   else if (mode_name && strcmp(mode_name, "on") == 0)
      context_mode = 2;
   else if (mode_name && mode_name[0] && strcmp(mode_name, "observe") != 0)
      LOG_WARN("ingress-context", "invalid code_context_mode=%s; using observe", mode_name);

   /* Strict `on` packets may contain only validated current-project evidence.
    * Typed facts are user/global evidence today, so they remain available in
    * off/observe but cannot become a silent fallback when strict retrieval
    * abstains or is unavailable. */
   if (context_mode == 2)
      facts_on = 0;
   if (!preview_on && !facts_on)
      return NULL;

   kb_client_memory_scope_context_set(active_workspace, active_project, 0);
   int first_task_turn = preview_on && context_mode != 0 &&
                         ingress_preinject_first_task_turn(g_session_id, active_project, query);
   char *task_packet = NULL;
   int task_items = 0;
   double task_confidence = 0.0;
   int context_status = -1;
   if (first_task_turn)
   {
      struct timespec context_started, context_finished;
      clock_gettime(CLOCK_MONOTONIC, &context_started);
      char *context_json = kb_client_code_context(query, NULL, active_project, &context_status);
      clock_gettime(CLOCK_MONOTONIC, &context_finished);
      if (kb_client_last_result_status() == KB_CLIENT_RESULT_UNAVAILABLE)
         ingress_preinject_rearm_unavailable(g_session_id, active_project);
      long context_latency_ms = ingress_elapsed_ms(&context_started, &context_finished);
      if (context_json && context_status == 200)
         task_packet = ingress_preinject_format_task_context(context_json, active_project,
                                                             &task_items, &task_confidence);
      if (context_latency_ms > 2000)
      {
         free(task_packet);
         task_packet = NULL;
         task_items = 0;
      }
      const char *effective_mode = context_mode == 2 && task_packet ? "on" : "observe";
      LOG_INFO("ingress-context",
               "mode=%s effective=%s project=%s status=%d latency_ms=%ld decision=%s items=%d "
               "visible=%d",
               context_mode == 2 ? "on" : "observe", effective_mode, active_project, context_status,
               context_latency_ms, task_packet ? "answerable" : "suppressed", task_items,
               context_mode == 2 && task_packet != NULL);
      free(context_json);
      if (context_mode == 1)
      {
         free(task_packet);
         task_packet = NULL; /* observe changes telemetry, never model-visible bytes */
      }
   }
   /* `on` is task-packet-only: no weak/global legacy substitution on a
    * no_answer/unavailable turn, and no repeated packet on same-task followups. */
   int legacy_preview_on = preview_on && context_mode != 2;
   int configured_budget = config_ingress_preinject_assembly_budget() > 0
                               ? config_ingress_preinject_assembly_budget()
                               : INGRESS_DEFAULT_ASSEMBLY_BUDGET;
   size_t envelope_budget = (size_t)configured_budget;
   if (envelope_budget <= INGRESS_FOOTER_RESERVE_BYTES)
   {
      free(task_packet);
      kb_client_memory_scope_context_clear();
      return NULL;
   }
   /* P1b lossy code fold (ingress-compression §1.2/§1.3): when enabled, a code
    * hit's snippet is replaced by a compact `file:line` reference recovered via
    * code_span_get. Gated by config and a per-request X-Aimee-Compress:0 opt-out
    * (request context, §1.4/B1 — never a thread-local, never forced on). Folding
    * a hit additionally requires a known matched line (PR1b enrichment) and a
    * snippet long enough to be worth folding — so it is fail-closed: no line ->
    * keep the snippet. */
   int compress = config_ingress_compress_enabled();
   const request_context_t *rctx = request_context_get();
   if (rctx && rctx->compress_disabled)
      compress = 0;
   int compress_min =
       config_ingress_compress_min_chars() > 0 ? config_ingress_compress_min_chars() : 80;
   const char *code_header =
       compress ? "recommended (code — expand via code_span_get):\n" : "recommended (code):\n";

   /* P0 Envelope IR: gather each source into a typed entry, then render the block
    * from the list. CAP = 6 code + 5 memory + 1 facts + 1 audit. */
   ingress_entry_t entries[1 + 6 + 5 + 1 + 1];
   int k = 0;
   double score = 0.0;
   int headline_missing_count = 0;
   int folded_count = 0;
   long folded_saved = 0;

   if (task_packet)
   {
      entries[k].kind = ING_SRC_CODE;
      entries[k].transform = ING_XF_NONE;
      entries[k].header = "";
      entries[k].preview = task_packet;
      task_packet = NULL;
      k++;
      score = task_confidence;
   }

   /* Primary signal: code search over the turn query. The code index is the
    * richest source, so recommended code files lead the envelope; the agent
    * sees which files matter before it explores. Confidence scales with how
    * many relevant files came back (no [0,1] rank is exposed by the search
    * path, so map the hit count into the tiering primitive). */
   code_search_hit_t hits[6];
   int n = legacy_preview_on ? kb_client_index_code_search(query, active_project, hits,
                                                           (int)(sizeof(hits) / sizeof(hits[0])))
                             : 0;
   for (int i = 0; i < n; i++)
   {
      int fold = compress && hits[i].line > 0 && (int)strlen(hits[i].snippet) > compress_min;
      entries[k].kind = ING_SRC_CODE;
      entries[k].transform = fold ? ING_XF_CODE_SIGNATURE_SPAN : ING_XF_NONE;
      entries[k].header = code_header;
      entries[k].preview = fold ? format_code_fold(&hits[i]) : format_code_body(&hits[i]);
      if (fold)
      {
         folded_count++;
         /* Resident saving estimate: the snippet bytes the fold dropped (the
          * preview text the unfolded body would have carried), for §6 telemetry. */
         folded_saved += (long)strlen(hits[i].snippet);
      }
      k++;
   }
   if (n > 0)
   {
      double cs = (double)n / 6.0;
      if (cs > score)
         score = cs;
   }

   /* Secondary signal: durable memory previews. Inject enough to decide what to
    * fetch next, not the whole memory body. The full row remains reachable via
    * the advertised memory:<id> handle and the memory_get MCP tool. */
   memory_diagnostic_t mems[5];
   int mem_n = legacy_preview_on ? kb_client_memory_diagnose(query, 5, mems, 5) : 0;
   /* A zero here has two meanings that must not be conflated: this turn had
    * nothing worth recalling, or the knowledge service could not answer. Both
    * produce an envelope with no memory previews, and until now both were
    * silent -- so a memory outage was indistinguishable from a quiet turn, and
    * the agent would state that something does not exist when it merely could
    * not look. session_degraded_notice.c makes exactly this point, but it fires
    * only at SessionStart; every per-turn injection (webchat, the Codex
    * /v1/responses path, the Anthropic proxy) had no equivalent.
    *
    * Recorded, not injected. The envelope bytes are a cache prefix on the
    * Anthropic arm, and adding a line to it on an outage would perturb the
    * cached prefix precisely when the service is already struggling. The
    * counter and this log line separate the two causes without touching the
    * request the provider sees. */
   if (legacy_preview_on && mem_n == 0 &&
       kb_client_last_result_status() == KB_CLIENT_RESULT_UNAVAILABLE)
   {
      ingress_recall_unavailable_total++;
      LOG_WARN("ingress-memory",
               "memory recall UNAVAILABLE (not empty): the knowledge service did not answer; "
               "this turn's envelope carries no memory previews. project=%s total=%lld",
               active_project[0] ? active_project : "-",
               (long long)ingress_recall_unavailable_total);
   }
   for (int i = 0; i < mem_n; i++)
   {
      char *body = format_memory_preview_body(&mems[i], &headline_missing_count);
      if (!body)
         continue; /* empty row (id <= 0): no entry, as the old NULL candidate */
      entries[k].kind = ING_SRC_MEMORY;
      entries[k].transform = ING_XF_NONE;
      entries[k].header = "recommended (memory previews):\n";
      entries[k].preview = body;
      k++;
   }
   if (mem_n > 0)
   {
      double ms = mem_n >= 4 ? 0.7 : (mem_n >= 2 ? 0.4 : 0.1);
      if (ms > score)
         score = ms;
   }

   /* Typed-fact layer (§7): current facts about entities named in this turn,
    * recalled and injected automatically so the agent grounds on them without
    * having to call the get_context_block tool. Gated kb-side on
    * typed_facts_enabled (returns NULL when off or none), so this is a no-op
    * then. User-asserted facts are high-signal, so they lift confidence. */
   char *facts = facts_on ? kb_client_memory_facts(query) : NULL;
   if (facts && facts[0])
   {
      dstr_t f;
      dstr_init(&f);
      dstr_append_str(&f, "## Known facts\n");
      dstr_append_str(&f, facts);
      if (facts[strlen(facts) - 1] != '\n')
         dstr_append_str(&f, "\n");
      entries[k].kind = ING_SRC_FACTS;
      entries[k].transform = ING_XF_NONE;
      entries[k].header = "";
      entries[k].preview = dstr_steal(&f);
      k++;
      if (score < 0.5)
         score = 0.5;
   }
   free(facts);

   /* Auditable-correctness P1: emit a single-writer, turn-keyed retrieval_event
    * recording the memory rows surfaced into this turn's context. Default-off
    * (kb_evidence_emit_enabled). Observation-only — the envelope and the answer
    * are byte-identical whether or not this fires; the only added work is one
    * synchronous KB write. The id is the one the HTTP layer minted (and surfaced
    * to the client as X-Aimee-Retrieval-Event); if none was set (e.g. a direct
    * build call) we mint one here so the event is still reconstructible. This is
    * the dedicated single-writer foundation; P1.5 folds the emit into the
    * retrieval handlers with the idempotent two-writer upsert. */
   if (config_kb_evidence_emit_enabled() && (mem_n > 0 || n > 0))
   {
      const char *tid = ingress_preinject_turn_id();
      char minted[40];
      if (!tid || !tid[0])
      {
         ingress_preinject_mint_turn_id(minted, sizeof(minted));
         tid = minted;
      }
      char fp[32];
      ingress_query_fingerprint(query, fp, sizeof(fp));

      /* Memory surface (single-writer, P1): mems[] holds the full set of memory
       * previews surfaced into this turn (mem_n <= the diagnose cap of 5), so
       * recording all of them is the complete memory evidence, not a truncation. */
      int64_t ids[5];
      const char *snips[5];
      int n_ids = 0;
      for (int i = 0; i < mem_n && n_ids < (int)(sizeof(ids) / sizeof(ids[0])); i++)
         if (mems[i].memory.id > 0)
         {
            /* Snippet for per-doc overlap attribution: the same preview text the
             * turn saw (headline, else content). */
            snips[n_ids] =
                mems[i].memory.headline[0] ? mems[i].memory.headline : mems[i].memory.content;
            ids[n_ids] = mems[i].memory.id;
            n_ids++;
         }
      if (n_ids > 0)
      {
         char ev_id[64] = "";
         /* Capture the event id so the next turn's continuation/repair autolabel
          * can attribute an outcome to these rows (default-off bridge). */
         if (kb_client_evidence_emit_retrieval_event_ex(tid, "Recall", fp, ids, n_ids, ev_id,
                                                        sizeof(ev_id)) == 0 &&
             ev_id[0])
            retrieval_outcome_bridge_note("memory", ev_id, ids, snips, n_ids);
      }

      /* Code surface (P1.5/D3): MERGE the code hits surfaced into this turn into the
       * turn's event as typed refs (code:<project>:<file_path>, v=content_hash).
       * Runs after the memory emit: when memory also surfaced it JOINS that event
       * (idempotent two-writer); on a code-only turn the merge is the first writer
       * and creates the event itself. */
      if (n > 0)
      {
         char refbuf[6][MAX_PATH_LEN + 160];
         const char *types[6], *refs[6], *versions[6];
         int cn = 0;
         for (int i = 0; i < n && cn < (int)(sizeof(types) / sizeof(types[0])); i++)
         {
            if (!hits[i].project[0] || !hits[i].file_path[0])
               continue;
            snprintf(refbuf[cn], sizeof(refbuf[cn]), "code:%s:%s", hits[i].project,
                     hits[i].file_path);
            types[cn] = "code";
            refs[cn] = refbuf[cn];
            versions[cn] = hits[i].content_hash; /* may be "" (no recorded hash) */
            cn++;
         }
         if (cn > 0)
            (void)kb_client_evidence_merge_retrieval_event(tid, "Recall", fp, types, refs, versions,
                                                           cn);
      }
   }

   char *audit = legacy_preview_on ? ingress_preinject_read_audit_context() : NULL;
   if (audit && audit[0])
   {
      dstr_t a;
      dstr_init(&a);
      dstr_append_str(&a, "recommended (audit context):\n");
      dstr_append_str(&a, audit);
      if (audit[strlen(audit) - 1] != '\n')
         dstr_append_str(&a, "\n");
      entries[k].kind = ING_SRC_AUDIT;
      entries[k].transform = ING_XF_NONE;
      entries[k].header = "";
      entries[k].preview = dstr_steal(&a);
      k++;
      if (score < 0.4)
         score = 0.4;
   }
   free(audit);

   /* The request-local scope must never leak through a reused worker thread. */
   kb_client_memory_scope_context_clear();

   /* Render the resident block from the typed entry list, then release the
    * per-entry previews (the renderer copies what it keeps). */
   char *blk = ingress_render_block(entries, k, envelope_budget, headline_missing_count, NULL);
   for (int i = 0; i < k; i++)
      free(entries[i].preview);

   /* §6 telemetry: attribute the fold's resident saving per turn (only when the
    * compression lever is engaged). The bench reads this to compute net token
    * economics; here it is an observable, non-dead record of what was folded. */
   if (compress && folded_count > 0)
      LOG_DEBUG("ingress-compress", "folded %d code %s, dropped ~%ld snippet bytes", folded_count,
                folded_count == 1 ? "hit" : "hits", folded_saved);

   if (!blk || !blk[0])
   {
      free(blk);
      return NULL;
   }
   const char *confidence = NULL;
   if (ingress_preinject_confidence(score, &confidence) != 0)
   {
      LOG_WARN("memory", "rerank confidence unavailable; omitting pre-injection envelope");
      free(blk);
      return NULL;
   }
   char *env = ingress_preinject_format_envelope(blk, confidence);
   free(blk);
   return env;
}

char *ingress_preinject_apply(const char *instructions, const char *envelope)
{
   /* Cache-prefix placement (§2): when the lever is on, place the volatile
    * envelope AFTER the stable instructions prefix (append) instead of before
    * (prepend), so the provider's automatic prefix cache survives the per-turn
    * envelope. The choice lives here — not in the caller — so the gateway stage
    * stays config-free and every consumer links unchanged.
    *
    * DEFAULT IS ON (config.c: cfg->ingress_cache_placement_enabled = 1), i.e.
    * APPEND. This comment used to say "Default off => prepend"; that was written
    * before the 2026-06-28 operator decision to ship the ingress levers on by
    * default (docs/proposals/done/ingress-compression-and-cache-alignment.md) and
    * was never updated. Read as a statement of current behaviour it inverts the
    * truth, and it cost a later investigation an hour spent chasing a
    * prefix-invalidation theory that the running code had already ruled out.
    * The default lives in config.c, not here — check it there before trusting any
    * prose about which branch is taken. */
   if (config_ingress_cache_placement_enabled())
      return ingress_preinject_append(instructions, envelope);

   int env_blank = 1;
   if (envelope)
      for (const char *p = envelope; *p; p++)
         if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
         {
            env_blank = 0;
            break;
         }

   if (env_blank)
      return instructions ? strdup(instructions) : NULL;

   dstr_t d;
   dstr_init(&d);
   dstr_append_str(&d, envelope);
   dstr_append_str(&d, "\n\n");
   if (instructions && instructions[0])
      dstr_append_str(&d, instructions);
   return dstr_steal(&d);
}

char *ingress_preinject_append(const char *instructions, const char *envelope)
{
   int env_blank = 1;
   if (envelope)
      for (const char *p = envelope; *p; p++)
         if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
         {
            env_blank = 0;
            break;
         }

   if (env_blank)
      return instructions ? strdup(instructions) : NULL;

   /* Cache-prefix placement (§2): the stable instructions prefix stays at the
    * front and the volatile <aimee-context> envelope lands at the tail, so the
    * provider's automatic prefix cache (OpenAI/Codex) is not invalidated by the
    * per-turn envelope. Mirror of ingress_preinject_apply with the order flipped. */
   dstr_t d;
   dstr_init(&d);
   if (instructions && instructions[0])
   {
      dstr_append_str(&d, instructions);
      dstr_append_str(&d, "\n\n");
   }
   dstr_append_str(&d, envelope);
   return dstr_steal(&d);
}
