#include <errno.h>
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
#include "aimee_sha256.h"
#include <aimee/audit/audit_worm.h>
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

/* Host transport only. The supplied request is consumed; all ingress policy
 * and state live in the shared Go owner. */
static cJSON *ingress_command(cJSON *request, int required_context)
{
   cJSON *response = NULL;
   int rc =
       aimee_module_commands_dispatch_internal_timeout("memory.runtime", request, 500, &response);
   cJSON_Delete(request);
   const char *status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "status"));
   if (rc != 1 || !status || strcmp(status, "ok") != 0)
   {
      if (required_context)
      {
         const char *kind =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "kind"));
         (void)request_context_refuse_assembly(kind);
      }
      cJSON_Delete(response);
      return NULL;
   }
   return response;
}

static void ingress_release_context(cJSON *request, const request_context_t *context)
{
   cJSON_AddStringToObject(request, "source_release_ticket", context->memory_source_release);
   cJSON_AddStringToObject(request, "request_id", context->request_id);
   cJSON_AddStringToObject(request, "principal", context->principal);
   cJSON_AddStringToObject(request, "caller_subject", context->caller_subject);
}

/* Forward accepted Go projection metadata without interpreting its source policy. */
int ingress_preinject_accept_native_projection(const cJSON *projection)
{
   const request_context_t *context = request_context_get();
   if (!context)
      return 0;
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "operation", "native-source-release");
   ingress_release_context(request, context);
   kb_client_memory_scope_context_apply(request);
   cJSON_AddItemToObject(request, "native_projection", cJSON_Duplicate(projection, 1));
   cJSON *response = ingress_command(request, 1);
   const char *ticket =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "source_release_ticket"));
   int rc = ticket ? request_context_set_source_release(ticket) : -1;
   cJSON_Delete(response);
   if (rc != 0)
      (void)request_context_refuse_assembly("unavailable");
   return rc;
}

/* The host carries opaque owner requests and responses. Source policy, version
 * comparison, scope selection and response validation all remain in Go. */
static int ingress_revalidate_sources(void **guard_state)
{
   if (guard_state)
      *guard_state = NULL;
   const request_context_t *context = request_context_get();
   if (!context || !context->memory_source_release[0])
      return 0;
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "operation", "source-release-plan");
   if (guard_state)
      cJSON_AddBoolToObject(request, "send_guard", 1);
   ingress_release_context(request, context);
   cJSON *plan = ingress_command(request, 1);
   const cJSON *owner_request = cJSON_GetObjectItemCaseSensitive(plan, "request");
   const cJSON *local_request = cJSON_GetObjectItemCaseSensitive(plan, "local_request");
   if ((!cJSON_IsObject(owner_request) && !cJSON_IsObject(local_request)) ||
       (owner_request && !cJSON_IsObject(owner_request)) ||
       (local_request && !cJSON_IsObject(local_request)))
   {
      (void)request_context_refuse_assembly("unavailable");
      cJSON_Delete(plan);
      return -1;
   }
   cJSON *local_response =
       local_request ? ingress_command(cJSON_Duplicate(local_request, 1), 1) : NULL;
   char *raw = owner_request ? kb_v1_action_request("memory.revalidate_sources",
                                                    cJSON_Duplicate(owner_request, 1))
                             : NULL;
   if (guard_state)
      *guard_state = plan;
   else
      cJSON_Delete(plan);
   cJSON *owner_response = raw ? cJSON_Parse(raw) : NULL;
   free(raw);
   request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "operation", "source-release-result");
   ingress_release_context(request, context);
   cJSON_AddItemToObject(request, "owner_response",
                         owner_response ? owner_response : cJSON_CreateNull());
   cJSON_AddItemToObject(request, "local_response",
                         local_response ? local_response : cJSON_CreateNull());
   cJSON *response = ingress_command(request, 1);
   int admitted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "admitted"));
   cJSON_Delete(response);
   if (!admitted)
      (void)request_context_refuse_assembly("unavailable");
   return admitted ? 0 : -1;
}

int ingress_preinject_revalidate_sources(void)
{
   return ingress_revalidate_sources(NULL);
}

int ingress_preinject_acquire_send_guard(void **state)
{
   if (!state)
      return -1;
   return ingress_revalidate_sources(state);
}

void ingress_preinject_release_send_guard(void *state)
{
   cJSON *plan = state;
   if (!plan)
      return;
   const cJSON *local = cJSON_GetObjectItemCaseSensitive(plan, "local_release_request");
   const cJSON *shared = cJSON_GetObjectItemCaseSensitive(plan, "release_request");
   if (cJSON_IsObject(local))
      cJSON_Delete(ingress_command(cJSON_Duplicate(local, 1), 0));
   if (cJSON_IsObject(shared))
      free(kb_v1_action_request("memory.revalidate_sources", cJSON_Duplicate(shared, 1)));
   cJSON_Delete(plan);
}

/* The Go owner supplies canonical metadata. This adapter cannot interpret or
 * rewrite source references; it only waits for the existing durable audit owner.
 * A failed append is not replaced by the asynchronous observability queue. */
static int ingress_append_receipt(const char *attempt, const char *stage, const char *at,
                                  const char *detail, const request_context_t *context)
{
   if (!attempt || strlen(attempt) != 32 || !stage || !at || !*at || !detail ||
       strlen(detail) > AUDIT_WORM_DETAIL_MAX)
      return -1;
   char event_id[128], action[96];
   snprintf(event_id, sizeof(event_id), "memory.provider.%s.%s", attempt, stage);
   snprintf(action, sizeof(action), "memory.provider.%s", stage);
   long long sequence = 0;
   return audit_worm_append_idempotent(event_id, at, "host", context->principal, action, attempt,
                                       "record", detail, &sequence);
}

int ingress_preinject_prepare_attempt(const void *body, size_t body_len, const char *route,
                                      const char *provider, const char *model, char attempt[33])
{
   if (!attempt)
      return -1;
   attempt[0] = '\0';
   const request_context_t *context = request_context_get();
   if (!context)
      return 0;
   if (context->context_refused)
      return -1;
   if (!context->memory_source_release[0] && !context->memory_receipt_required)
      return 0;
   if ((!body && body_len) || ingress_preinject_revalidate_sources() != 0)
      return -1;
   char digest[65], count[32];
   if (aimee_sha256_hex(body, body_len, digest) != 0)
      return -1;
   snprintf(count, sizeof(count), "%zu", body_len);
   cJSON *request = cJSON_CreateObject();
   ingress_release_context(request, context);
   cJSON_AddStringToObject(request, "operation", "provider-receipt-plan");
   cJSON_AddStringToObject(request, "route", route ? route : "");
   cJSON_AddStringToObject(request, "provider", provider ? provider : "");
   cJSON_AddStringToObject(request, "model", model ? model : "");
   cJSON_AddStringToObject(request, "payload_sha256", digest);
   cJSON_AddStringToObject(request, "payload_bytes", count);
   cJSON_AddStringToObject(request, "turn_id", ingress_preinject_turn_id());
   cJSON_AddStringToObject(request, "producer_build", AIMEE_VERSION);
   const char *caller_limits =
       context->request_budget_present ? context->request_budget_limits : "";
   const char *operator_limits = getenv("AIMEE_PROVIDER_CONTEXT_LIMITS");
   char caller_digest[65], operator_digest[65];
   if (!operator_limits)
      operator_limits = "";
   if (aimee_sha256_hex(caller_limits, strlen(caller_limits), caller_digest) != 0 ||
       aimee_sha256_hex(operator_limits, strlen(operator_limits), operator_digest) != 0)
   {
      cJSON_Delete(request);
      return -1;
   }
   cJSON_AddStringToObject(request, "caller_limits_sha256", caller_digest);
   cJSON_AddStringToObject(request, "operator_limits_sha256", operator_digest);
   cJSON *plan = ingress_command(request, 1);
   const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(plan, "attempt_id"));
   const char *at = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(plan, "at"));
   const char *prepared =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(plan, "prepared_detail"));
   const char *admitted =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(plan, "admitted_detail"));
   int rc = -1;
   if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(plan, "requires_durable_acceptance")) &&
       ingress_append_receipt(id, "prepared", at, prepared, context) == 0 &&
       ingress_append_receipt(id, "dispatch_admitted", at, admitted, context) == 0)
   {
      memcpy(attempt, id, 33);
      rc = 0;
   }
   cJSON_Delete(plan);
   if (rc != 0)
      (void)request_context_refuse_assembly("unavailable");
   return rc;
}

int ingress_preinject_observe_attempt(const char *attempt, int http_status, const char *response,
                                      size_t response_len)
{
   char digest[65];
   if ((!response && response_len) || aimee_sha256_hex(response, response_len, digest) != 0)
      return -1;
   return ingress_preinject_observe_commitment(attempt, http_status, digest, response_len,
                                               "host_buffered_response_string");
}

int ingress_preinject_observe_commitment(const char *attempt, int http_status, const char *digest,
                                         size_t response_len, const char *representation)
{
   if (!attempt || !*attempt)
      return 0;
   const request_context_t *context = request_context_get();
   if (!context || !digest || !representation)
      return -1;
   char count[32];
   snprintf(count, sizeof(count), "%zu", response_len);
   cJSON *request = cJSON_CreateObject();
   ingress_release_context(request, context);
   cJSON_AddStringToObject(request, "operation", "provider-receipt-observe");
   cJSON_AddStringToObject(request, "attempt_id", attempt);
   cJSON_AddNumberToObject(request, "http_status", http_status < 0 ? -1 : http_status);
   cJSON_AddStringToObject(request, "response_sha256", digest);
   cJSON_AddStringToObject(request, "response_bytes", count);
   cJSON_AddStringToObject(request, "response_representation", representation);
   cJSON *plan = ingress_command(request, 0);
   const char *detail =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(plan, "observation_detail"));
   cJSON *event = detail ? cJSON_Parse(detail) : NULL;
   const char *at = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "at"));
   const char *stage = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "stage"));
   int rc = ingress_append_receipt(attempt, stage, at, detail, context);
   if (rc == 0)
   {
      char stored_digest[65];
      if (aimee_sha256_hex(detail, strlen(detail), stored_digest) == 0)
      {
         cJSON *stored = cJSON_CreateObject();
         ingress_release_context(stored, context);
         cJSON_AddStringToObject(stored, "operation", "provider-receipt-stored");
         cJSON_AddStringToObject(stored, "attempt_id", attempt);
         cJSON_AddStringToObject(stored, "observation_sha256", stored_digest);
         /* The append already succeeded. An unavailable cache confirmation
          * leaves the owner's entry retained; it never resends the provider. */
         cJSON_Delete(ingress_command(stored, 0));
      }
   }
   cJSON_Delete(event);
   cJSON_Delete(plan);
   return rc;
}

void ingress_preinject_finish_sources(void)
{
   const request_context_t *context = request_context_get();
   if (!context || !context->memory_source_release[0])
      return;
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "operation", "source-release-finish");
   ingress_release_context(request, context);
   cJSON_Delete(ingress_command(request, 0));
   (void)request_context_set_source_release("");
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

/* Forward only owner-selected references after the assembled envelope has
 * passed the host integrity gate. Reference interpretation stays in Go. */
static void ingress_emit_projection_refs(const cJSON *rows, const char *turn_id,
                                         const char *fingerprint)
{
   int count = cJSON_GetArraySize(rows);
   if (count <= 0)
      return;
   const char **types = calloc((size_t)count, sizeof(*types));
   const char **refs = calloc((size_t)count, sizeof(*refs));
   if (!types || !refs)
   {
      free(types);
      free(refs);
      return;
   }
   int retained = 0;
   const cJSON *row;
   cJSON_ArrayForEach(row, rows)
   {
      const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "type"));
      const char *ref = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "ref"));
      if (!type || !type[0] || !ref || !ref[0])
         continue;
      types[retained] = type;
      refs[retained++] = ref;
   }
   if (retained > 0)
      (void)kb_client_evidence_merge_retrieval_event(turn_id, "Recall", fingerprint, types, refs,
                                                     NULL, retained);
   free(types);
   free(refs);
}

char *ingress_preinject_build(const char *query, int request_disabled)
{
   char active_workspace[512] = "";
   char active_project[512] = "";
   int active_scope =
       ingress_preinject_resolve_active_scope(active_workspace, sizeof(active_workspace),
                                              active_project, sizeof(active_project)) == 0;
   const request_context_t *rctx = request_context_get();
   const char *mode = config_code_context_mode();
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "operation", "ingress-begin");
   cJSON_AddStringToObject(request, "query", query ? query : "");
   cJSON_AddStringToObject(request, "session", g_session_id);
   cJSON_AddStringToObject(request, "project", active_project);
   cJSON_AddBoolToObject(request, "active_scope", active_scope);
   cJSON_AddBoolToObject(request, "disabled", request_disabled || g_request_disabled);
   cJSON_AddBoolToObject(request, "preview_enabled", config_ingress_preinject_enabled());
   cJSON_AddStringToObject(request, "mode", mode ? mode : "");
   cJSON_AddNumberToObject(request, "budget", config_ingress_preinject_assembly_budget());
   cJSON_AddBoolToObject(request, "compress", config_ingress_compress_enabled());
   cJSON_AddBoolToObject(request, "compress_disabled", rctx && rctx->compress_disabled);
   cJSON_AddNumberToObject(request, "compress_min", config_ingress_compress_min_chars());
   cJSON *plan = ingress_command(request, 1);
   if (!plan)
   {
      LOG_WARN("memory", "Go ingress plan failed");
      return NULL;
   }
   const char *warning = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(plan, "warning"));
   if (warning)
      LOG_WARN("ingress-context", "%s", warning);
   if (!cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(plan, "active")))
   {
      (void)request_context_refuse_assembly("unavailable");
      cJSON_Delete(plan);
      return NULL;
   }
   if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(plan, "active")))
   {
      cJSON_Delete(plan);
      return NULL;
   }
   const cJSON *assembly_plan = cJSON_GetObjectItemCaseSensitive(plan, "assembly");
   const char *planned_mode = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(plan, "mode"));
   if (!cJSON_IsObject(assembly_plan) || !planned_mode)
   {
      (void)request_context_refuse_assembly("unavailable");
      cJSON_Delete(plan);
      return NULL;
   }
   cJSON *assembly = cJSON_Duplicate(assembly_plan, 1);
   int legacy_preview_on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(plan, "legacy_preview"));
   int facts_on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(plan, "facts"));
   int temporal_on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(plan, "temporal"));
   kb_client_memory_scope_context_set(active_workspace, active_project, 0);
   if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(plan, "task")))
   {
      int context_status = -1;
      struct timespec started, finished;
      clock_gettime(CLOCK_MONOTONIC, &started);
      char *raw = kb_client_code_context(query, NULL, active_project, &context_status);
      clock_gettime(CLOCK_MONOTONIC, &finished);
      cJSON *task = cJSON_CreateObject();
      cJSON_AddStringToObject(task, "operation", "ingress-task-result");
      cJSON_AddStringToObject(task, "session", g_session_id);
      cJSON_AddStringToObject(task, "project", active_project);
      cJSON_AddStringToObject(task, "mode", planned_mode);
      cJSON_AddNumberToObject(task, "http_status", context_status);
      cJSON_AddNumberToObject(task, "elapsed_ms", ingress_elapsed_ms(&started, &finished));
      cJSON_AddBoolToObject(task, "unavailable",
                            kb_client_last_result_status() == KB_CLIENT_RESULT_UNAVAILABLE);
      cJSON *packet = raw ? cJSON_Parse(raw) : NULL;
      free(raw);
      cJSON_AddItemToObject(task, "packet", packet ? packet : cJSON_CreateNull());
      cJSON *result = ingress_command(task, 0);
      const char *block = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "block"));
      const cJSON *confidence = cJSON_GetObjectItemCaseSensitive(result, "confidence");
      if (block && cJSON_IsNumber(confidence))
      {
         cJSON_AddStringToObject(assembly, "task_block", block);
         cJSON_AddNumberToObject(assembly, "task_confidence", confidence->valuedouble);
      }
      const char *message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "log"));
      if (message)
         LOG_INFO("ingress-context", "%s", message);
      cJSON_Delete(result);
   }
   cJSON_Delete(plan);

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
   cJSON *memories = NULL;
   if (legacy_preview_on)
   {
      cJSON *request = cJSON_CreateObject();
      kb_client_memory_scope_context_apply(request);
      cJSON_AddStringToObject(request, "query", query);
      cJSON_AddStringToObject(request, "format", "ingress");
      cJSON_AddNumberToObject(request, "limit", 5);
      char *raw = kb_v1_action_request("memory.diagnose_scoped", request);
      cJSON *reply = raw ? cJSON_ParseWithOpts(raw, NULL, 1) : NULL;
      free(raw);
      const cJSON *status = cJSON_GetObjectItemCaseSensitive(reply, "status");
      const cJSON *rows = cJSON_GetObjectItemCaseSensitive(reply, "memories");
      if (cJSON_IsString(status) && !strcmp(status->valuestring, "ok") && cJSON_IsArray(rows) &&
          cJSON_GetArraySize(rows) <= 5)
      {
         memories = cJSON_DetachItemFromObjectCaseSensitive(reply, "memories");
         cJSON *projection = cJSON_DetachItemFromObjectCaseSensitive(reply, "memory_projection");
         if (projection)
            cJSON_AddItemToObject(assembly, "memory_projection", projection);
      }
      cJSON_Delete(reply);
   }
   int mem_n = memories ? cJSON_GetArraySize(memories) : 0;
   int memory_unavailable = legacy_preview_on && !memories;
   if (!memories)
      memories = cJSON_CreateArray();
   cJSON_AddItemToObject(assembly, "memories", memories);
   if (legacy_preview_on)
   {
      cJSON *outcome = cJSON_CreateObject();
      cJSON_AddStringToObject(outcome, "operation", "ingress-recall-result");
      cJSON_AddStringToObject(outcome, "project", active_project);
      cJSON_AddNumberToObject(outcome, "count", mem_n);
      cJSON_AddBoolToObject(outcome, "unavailable", memory_unavailable);
      cJSON *result = ingress_command(outcome, 0);
      const char *message =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "warning"));
      if (message)
         LOG_WARN("ingress-memory", "%s", message);
      cJSON_Delete(result);
   }
   /* Typed-fact layer (§7): current facts about entities named in this turn,
    * recalled and injected automatically so the agent grounds on them without
    * having to call the get_context_block tool. Gated kb-side on
    * the typed-fact layer (returns NULL when there are none), so this is a no-op
    * then. User-asserted facts are high-signal, so they lift confidence. */
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
   char *temporal = temporal_on
                        ? kb_client_memory_assemble_typed_context_json(
                              query, cJSON_GetObjectItemCaseSensitive(assembly, "context_limits"))
                        : NULL;
   cJSON_AddStringToObject(assembly, "typed_context_json", temporal ? temporal : "");
   free(temporal);

   char *audit = legacy_preview_on ? ingress_preinject_read_audit_context() : NULL;
   cJSON_AddStringToObject(assembly, "audit", audit ? audit : "");
   free(audit);

   if (rctx)
   {
      cJSON_AddBoolToObject(assembly, "prepare_source_release", 1);
      cJSON_AddStringToObject(assembly, "workspace", active_workspace);
      cJSON_AddStringToObject(assembly, "project", active_project);
      ingress_release_context(assembly, rctx);
   }
   cJSON *response = ingress_command(assembly, 1);
   const char *envelope =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "envelope"));
   if (!envelope)
   {
      (void)request_context_refuse_assembly("unavailable");
      kb_client_memory_scope_context_clear();
      cJSON_Delete(response);
      LOG_WARN("memory", "Go ingress assembly failed");
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
   if (result && rctx)
   {
      (void)request_context_require_memory_receipt();
      const char *ticket =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "source_release_ticket"));
      if (request_context_set_source_release(ticket) != 0)
      {
         (void)request_context_refuse_assembly("unavailable");
         free(result);
         result = NULL;
      }
   }
   if (!result && rctx)
   {
      const char *ticket =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "source_release_ticket"));
      if (ticket && ticket[0] && strcmp(ticket, rctx->memory_source_release) != 0)
      {
         cJSON *discard = cJSON_CreateObject();
         ingress_release_context(discard, rctx);
         cJSON_ReplaceItemInObjectCaseSensitive(discard, "source_release_ticket",
                                                cJSON_CreateString(ticket));
         cJSON_AddStringToObject(discard, "operation", "source-release-discard");
         cJSON_Delete(ingress_command(discard, 0));
      }
   }
   /* Assembly evidence is emitted only after packing and integrity acceptance.
    * It does not assert provider admission, dispatch, or acknowledgement. */
   const cJSON *retained_memories = cJSON_GetObjectItemCaseSensitive(response, "retained_memories");
   const cJSON *retained_code = cJSON_GetObjectItemCaseSensitive(response, "retained_code_indices");
   const cJSON *retained_typed = cJSON_GetObjectItemCaseSensitive(response, "retained_typed_refs");
   const cJSON *retained_facts = cJSON_GetObjectItemCaseSensitive(response, "retained_fact_refs");
   const cJSON *retained_memory_sources =
       cJSON_GetObjectItemCaseSensitive(response, "retained_memory_source_refs");
   if (result && config_kb_evidence_emit_enabled() &&
       (cJSON_GetArraySize(retained_memories) > 0 || cJSON_GetArraySize(retained_code) > 0 ||
        cJSON_GetArraySize(retained_typed) > 0 || cJSON_GetArraySize(retained_facts) > 0))
   {
      const char *tid = ingress_preinject_turn_id();
      char minted[40];
      if (!tid || !tid[0])
      {
         if (ingress_preinject_mint_turn_id(minted, sizeof(minted)) != 0)
         {
            free(result);
            cJSON_Delete(response);
            kb_client_memory_scope_context_clear();
            return NULL;
         }
         tid = minted;
      }
      char fp[32];
      ingress_query_fingerprint(query, fp, sizeof(fp));

      /* Copy only previews retained by the Go packer. */
      int64_t ids[5];
      const char *snips[5];
      int n_ids = 0;
      for (int i = 0;
           i < cJSON_GetArraySize(retained_memories) && n_ids < (int)(sizeof(ids) / sizeof(ids[0]));
           i++)
      {
         const cJSON *row = cJSON_GetArrayItem(retained_memories, i);
         const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "id"));
         const char *preview =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "preview"));
         char *end = NULL;
         errno = 0;
         int64_t value = id ? strtoll(id, &end, 10) : 0;
         if (id && id[0] >= '1' && id[0] <= '9' && end && !*end && !errno && value > 0 && preview)
         {
            ids[n_ids] = value;
            snips[n_ids] = preview;
            n_ids++;
         }
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
      if (cJSON_GetArraySize(retained_code) > 0)
      {
         char refbuf[6][MAX_PATH_LEN + 160];
         const char *types[6], *refs[6], *versions[6];
         int cn = 0;
         for (int j = 0;
              j < cJSON_GetArraySize(retained_code) && cn < (int)(sizeof(types) / sizeof(types[0]));
              j++)
         {
            const cJSON *index = cJSON_GetArrayItem(retained_code, j);
            if (!cJSON_IsNumber(index) || index->valuedouble != index->valueint ||
                index->valueint < 0 || index->valueint >= n)
               continue;
            int i = index->valueint;
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
      /* Merge after the legacy memory writer, whose turn creation is first-wins. */
      ingress_emit_projection_refs(retained_typed, tid, fp);
      ingress_emit_projection_refs(retained_facts, tid, fp);
      ingress_emit_projection_refs(retained_memory_sources, tid, fp);
   }

   kb_client_memory_scope_context_clear();
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
