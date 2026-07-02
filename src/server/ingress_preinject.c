/* server/ingress_preinject.c: see ingress_preinject.h.
 *
 * The envelope is a compact, model-readable block. Its `explore-with` line
 * names Aimee's own retrieval tools so a co-registered agent fills any gap
 * THROUGH Aimee (symbol-scoped, graph-aware) instead of raw-grepping the tree.
 */
#include "ingress_preinject.h"
#include "config.h"
#include "kb_client.h"
#include "dstr.h"
#include "log.h"
#include "request_context.h"
#include "platform_random.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* The standing exploration policy carried in every envelope. Kept short — it is
 * advice the model weighs, not a contract we can enforce over the wire. */
#define INGRESS_EXPLORE_WITH                                                                       \
   "find_symbol, lsp_references, ast_grep_search, search_graph, get_context_block, memory_get"

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

const char *ingress_preinject_confidence(double top_score)
{
   /* Thresholds chosen so a clear top hit is "high", a plausible-but-thin match
    * is "medium", and a weak match is "low". Clamped to the documented tiers. */
   if (top_score >= 0.66)
      return "high";
   if (top_score >= 0.33)
      return "medium";
   return "low";
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

   if (!confidence || !confidence[0])
      confidence = "low";

   dstr_t d;
   dstr_init(&d);
   dstr_appendf(&d, "<aimee-context confidence=\"%s\">\n", confidence);
   dstr_append_str(&d, context_block);
   if (context_block[strlen(context_block) - 1] != '\n')
      dstr_append_str(&d, "\n");
   dstr_append_str(&d, "explore-with: " INGRESS_EXPLORE_WITH "\n");
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

char *ingress_preinject_build(const char *query, int request_disabled)
{
   if (request_disabled || g_request_disabled)
      return NULL;
   if (!query || !query[0])
      return NULL;

   config_t cfg;
   config_load(&cfg);
   /* The envelope carries two independently-gated layers: the code/memory
    * preview block (ingress_preinject_enabled, aimed at coding agents) and the
    * typed-fact block (typed_facts_enabled). Build if EITHER is on, so typed
    * facts surface in turns without requiring the heavier preview machinery. */
   int preview_on = cfg.ingress_preinject_enabled;
   int facts_on = cfg.typed_facts_enabled;
   if (!preview_on && !facts_on)
      return NULL;
   int configured_budget = cfg.ingress_preinject_assembly_budget > 0
                               ? cfg.ingress_preinject_assembly_budget
                               : INGRESS_DEFAULT_ASSEMBLY_BUDGET;
   size_t envelope_budget = (size_t)configured_budget;
   if (envelope_budget <= INGRESS_FOOTER_RESERVE_BYTES)
      return NULL;
   /* P1b lossy code fold (ingress-compression §1.2/§1.3): when enabled, a code
    * hit's snippet is replaced by a compact `file:line` reference recovered via
    * code_span_get. Gated by config and a per-request X-Aimee-Compress:0 opt-out
    * (request context, §1.4/B1 — never a thread-local, never forced on). Folding
    * a hit additionally requires a known matched line (PR1b enrichment) and a
    * snippet long enough to be worth folding — so it is fail-closed: no line ->
    * keep the snippet. */
   int compress = cfg.ingress_compress_enabled;
   const request_context_t *rctx = request_context_get();
   if (rctx && rctx->compress_disabled)
      compress = 0;
   int compress_min = cfg.ingress_compress_min_chars > 0 ? cfg.ingress_compress_min_chars : 80;
   const char *code_header =
       compress ? "recommended (code — expand via code_span_get):\n" : "recommended (code):\n";

   /* P0 Envelope IR: gather each source into a typed entry, then render the block
    * from the list. CAP = 6 code + 5 memory + 1 facts + 1 audit. */
   ingress_entry_t entries[6 + 5 + 1 + 1];
   int k = 0;
   double score = 0.0;
   int headline_missing_count = 0;
   int folded_count = 0;
   long folded_saved = 0;

   /* Primary signal: code search over the turn query. The code index is the
    * richest source, so recommended code files lead the envelope; the agent
    * sees which files matter before it explores. Confidence scales with how
    * many relevant files came back (no [0,1] rank is exposed by the search
    * path, so map the hit count into the tiering primitive). */
   code_search_hit_t hits[6];
   int n = preview_on ? kb_client_index_code_search(query, NULL, hits,
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
   int mem_n = preview_on ? kb_client_memory_diagnose(query, 5, mems, 5) : 0;
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
   if (cfg.kb_evidence_emit_enabled && (mem_n > 0 || n > 0))
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
      int n_ids = 0;
      for (int i = 0; i < mem_n && n_ids < (int)(sizeof(ids) / sizeof(ids[0])); i++)
         if (mems[i].memory.id > 0)
            ids[n_ids++] = mems[i].memory.id;
      if (n_ids > 0)
         (void)kb_client_evidence_emit_retrieval_event(tid, "Recall", fp, ids, n_ids);

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

   char *audit = preview_on ? ingress_preinject_read_audit_context() : NULL;
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
   char *env = ingress_preinject_format_envelope(blk, ingress_preinject_confidence(score));
   free(blk);
   return env;
}

char *ingress_preinject_apply(const char *instructions, const char *envelope)
{
   /* Cache-prefix placement (§2): when the lever is on, place the volatile
    * envelope AFTER the stable instructions prefix (append) instead of before
    * (prepend), so the provider's automatic prefix cache survives the per-turn
    * envelope. The choice lives here — not in the caller — so the gateway stage
    * stays config-free and every consumer links unchanged. Default off => prepend
    * (byte-identical to before). */
   config_t cfg;
   if (config_load(&cfg) == 0 && cfg.ingress_cache_placement_enabled)
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
