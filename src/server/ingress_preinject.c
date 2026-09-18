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
#include "integrity.h"
#include "module_commands.h"
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
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

int ingress_preinject_mint_turn_id(char *buf, size_t len)
{
   if (!buf || len == 0)
      return -1;
   unsigned char raw[16];
   if (platform_random_bytes(raw, sizeof(raw)) != 0)
   {
      buf[0] = '\0';
      return -1;
   }
   snprintf(buf, len, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10],
            raw[11], raw[12], raw[13], raw[14], raw[15]);
   return 0;
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

/* Transitional host transport for the remaining ingress caller. Task tracking,
 * validation and rendering all execute in the same supervised Go owner. */
static cJSON *ingress_runtime_request(const char *operation, const char *session,
                                      const char *project, const char *query)
{
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "operation", operation);
   cJSON_AddStringToObject(request, "session", session ? session : "");
   cJSON_AddStringToObject(request, "project", project ? project : "");
   cJSON_AddStringToObject(request, "query", query ? query : "");
   cJSON *response = NULL;
   int rc =
       aimee_module_commands_dispatch_internal_timeout("memory.runtime", request, 500, &response);
   cJSON_Delete(request);
   const char *status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "status"));
   if (rc != 1 || !status || strcmp(status, "ok") != 0)
   {
      cJSON_Delete(response);
      return NULL;
   }
   return response;
}

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
   cJSON_Delete(ingress_runtime_request("ingress-task-reset", NULL, NULL, NULL));
}

static int ingress_preinject_first_task_turn(const char *session, const char *project,
                                             const char *query)
{
   cJSON *response = ingress_runtime_request("ingress-task-claim", session, project, query);
   int fetch = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "fetch"));
   cJSON_Delete(response);
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
   cJSON_Delete(ingress_runtime_request("ingress-task-rearm", session, project, NULL));
}

static long ingress_elapsed_ms(const struct timespec *start, const struct timespec *end)
{
   return (long)(end->tv_sec - start->tv_sec) * 1000L +
          (long)(end->tv_nsec - start->tv_nsec) / 1000000L;
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

/* The host supplies the retrieved packet and verified active project. Validation
 * and rendering belong to the shared Go memory owner. No native fallback. */
char *ingress_preinject_format_task_context(const char *json, const char *active_project,
                                            int *item_count_out, double *confidence_out)
{
   if (item_count_out)
      *item_count_out = 0;
   if (confidence_out)
      *confidence_out = 0;
   if (!json || !active_project)
      return NULL;
   cJSON *packet = cJSON_Parse(json);
   if (!packet)
      return NULL;
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "operation", "ingress-task-packet");
   cJSON_AddStringToObject(request, "project", active_project);
   cJSON_AddItemToObject(request, "packet", packet);
   cJSON *response = NULL;
   int rc =
       aimee_module_commands_dispatch_internal_timeout("memory.runtime", request, 500, &response);
   cJSON_Delete(request);
   char *result = NULL;
   const char *status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "status"));
   const char *block = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "block"));
   const cJSON *count = cJSON_GetObjectItemCaseSensitive(response, "item_count");
   const cJSON *confidence = cJSON_GetObjectItemCaseSensitive(response, "confidence");
   if (rc == 1 && status && strcmp(status, "ok") == 0 && block && block[0] &&
       cJSON_IsNumber(count) && cJSON_IsNumber(confidence))
   {
      result = strdup(block);
      if (result && item_count_out)
         *item_count_out = count->valueint;
      if (result && confidence_out)
         *confidence_out = confidence->valuedouble;
   }
   cJSON_Delete(response);
   return result;
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
   /* The envelope carries code/memory preview, typed-fact, and temporal-learning
    * layers. The temporal assembler is default-on and remains governed by the
    * ingress master gate and per-request opt-out.
    *
    * Typed facts used to be KB-OWNED (proposal §8): aimee-server asked the KB
    * through kb_client_typed_facts_enabled() rather than reading a config of its
    * own. That gate is retired and the layer is unconditional, so the question
    * had one answer and the seam is gone with it -- leaving a function that
    * always returns 1 would keep the shape of an option that no longer exists.
    * Facts now depend only on having an active scope. */
   int preview_configured = config_ingress_preinject_enabled();
   int temporal_configured = preview_configured;
   char active_workspace[512] = "";
   char active_project[512] = "";
   int active_scope =
       ingress_preinject_resolve_active_scope(active_workspace, sizeof(active_workspace),
                                              active_project, sizeof(active_project)) == 0;
   /* Agent ingress is deliberately fail-closed without an active repository:
    * neither code nor memory may silently broaden to global recall. */
   int preview_on = preview_configured && active_scope;
   int facts_on = active_scope;
   int temporal_on = temporal_configured && active_scope;

   const char *mode_name = config_code_context_mode();
   int context_mode = 1; /* invalid/blank values fail safely to observe */
   if (mode_name && strcmp(mode_name, "off") == 0)
      context_mode = 0;
   else if (mode_name && strcmp(mode_name, "on") == 0)
      context_mode = 2;
   else if (mode_name && mode_name[0] && strcmp(mode_name, "observe") != 0)
      LOG_WARN("ingress-context", "invalid code_context_mode=%s; using observe", mode_name);

   /* Strict `on` task packets may contain only validated current-project code
    * evidence. Typed facts are user/global evidence today, so they cannot
    * become a silent fallback when strict code retrieval abstains. Temporal
    * learning is a separately labelled, scope-filtered channel with explicit
    * trust/authority boundaries, and remains default-on in every code-context
    * mode. */
   if (context_mode == 2)
      facts_on = 0;
   if (!preview_on && !facts_on && !temporal_on)
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
   cJSON *assembly = cJSON_CreateObject();
   cJSON_AddStringToObject(assembly, "operation", "ingress-assemble");
   cJSON_AddNumberToObject(assembly, "budget", (double)envelope_budget);
   const request_context_t *rctx = request_context_get();
   cJSON_AddBoolToObject(assembly, "compress",
                         config_ingress_compress_enabled() && !(rctx && rctx->compress_disabled));
   cJSON_AddNumberToObject(assembly, "compress_min", config_ingress_compress_min_chars());
   cJSON_AddStringToObject(assembly, "task_block", task_packet ? task_packet : "");
   cJSON_AddNumberToObject(assembly, "task_confidence", task_confidence);
   free(task_packet);

   /* Primary signal: code search over the turn query. The code index is the
    * richest source, so recommended code files lead the envelope; the agent
    * sees which files matter before it explores. Confidence scales with how
    * many relevant files came back (no [0,1] rank is exposed by the search
    * path, so map the hit count into the tiering primitive). */
   code_search_hit_t hits[6];
   int n = legacy_preview_on ? kb_client_index_code_search(query, active_project, hits,
                                                           (int)(sizeof(hits) / sizeof(hits[0])))
                             : 0;
   cJSON *code = cJSON_AddArrayToObject(assembly, "code");
   for (int i = 0; i < n; i++)
   {
      cJSON *hit = cJSON_CreateObject();
      cJSON_AddStringToObject(hit, "file_path", hits[i].file_path);
      cJSON_AddStringToObject(hit, "snippet", hits[i].snippet);
      cJSON_AddNumberToObject(hit, "line", hits[i].line);
      cJSON_AddItemToArray(code, hit);
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
   cJSON *memories = cJSON_AddArrayToObject(assembly, "memories");
   for (int i = 0; i < mem_n; i++)
   {
      cJSON *row = cJSON_CreateObject();
      char id[32];
      snprintf(id, sizeof(id), "%" PRId64, (int64_t)mems[i].memory.id);
      cJSON_AddStringToObject(row, "id", id);
      cJSON_AddStringToObject(row, "key", mems[i].memory.key);
      cJSON_AddStringToObject(row, "tier", mems[i].memory.tier);
      cJSON_AddStringToObject(row, "kind", mems[i].memory.kind);
      cJSON_AddStringToObject(row, "headline", mems[i].memory.headline);
      cJSON_AddStringToObject(row, "content", mems[i].memory.content);
      cJSON_AddNumberToObject(row, "score", mems[i].parts.total);
      cJSON_AddItemToArray(memories, row);
   }

   /* Typed-fact layer (§7): current facts about entities named in this turn,
    * recalled and injected automatically so the agent grounds on them without
    * having to call the get_context_block tool. Gated kb-side on
    * the typed-fact layer (returns NULL when there are none), so this is a no-op
    * then. User-asserted facts are high-signal, so they lift confidence. */
   cJSON_AddBoolToObject(assembly, "facts_requested", facts_on);
   if (facts_on)
   {
      cJSON *request = cJSON_CreateObject();
      kb_client_memory_scope_context_apply(request);
      cJSON_AddStringToObject(request, "query", query);
      char *raw = kb_v1_action_request("memory.facts", request);
      cJSON *response = raw ? cJSON_Parse(raw) : NULL;
      free(raw);
      cJSON_AddItemToObject(assembly, "facts_response", response ? response : cJSON_CreateNull());
   }
   char *temporal = temporal_on ? kb_client_memory_assemble_typed_context(query) : NULL;
   cJSON_AddStringToObject(assembly, "temporal", temporal ? temporal : "");
   free(temporal);

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
         if (ingress_preinject_mint_turn_id(minted, sizeof(minted)) != 0)
         {
            cJSON_Delete(assembly);
            kb_client_memory_scope_context_clear();
            return NULL;
         }
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
   cJSON_AddStringToObject(assembly, "audit", audit ? audit : "");
   free(audit);
   kb_client_memory_scope_context_clear();

   cJSON *response = NULL;
   int rc =
       aimee_module_commands_dispatch_internal_timeout("memory.runtime", assembly, 500, &response);
   cJSON_Delete(assembly);
   const char *status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "status"));
   const char *envelope =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "envelope"));
   if (rc != 1 || !status || strcmp(status, "ok") != 0 || !envelope)
   {
      cJSON_Delete(response);
      LOG_WARN("memory", "Go ingress assembly unavailable; omitting pre-injection envelope");
      return NULL;
   }
   if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "facts_unavailable")))
      LOG_WARN("ingress-memory",
               "typed-fact recall unavailable or invalid; continuing without facts");
   const cJSON *folded = cJSON_GetObjectItemCaseSensitive(response, "folded_count");
   const cJSON *saved = cJSON_GetObjectItemCaseSensitive(response, "folded_saved");
   if (cJSON_IsNumber(folded) && folded->valueint > 0 && cJSON_IsNumber(saved))
      LOG_DEBUG("ingress-compress", "folded %d code %s, dropped ~%.0f snippet bytes",
                folded->valueint, folded->valueint == 1 ? "hit" : "hits", saved->valuedouble);
   char *result = NULL;
   if (envelope[0])
   {
      integrity_result_t gate;
      if (integrity_ingress_decide(envelope, INTEGRITY_SOURCE_DOCUMENT, "retrieval", 1, &gate))
         LOG_WARN("integrity", "automatic retrieval parked (%s): %s",
                  integrity_verdict_name(gate.verdict), gate.match_category);
      else
         result = strdup(envelope);
   }
   cJSON_Delete(response);
   return result;
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
