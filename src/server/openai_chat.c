#include "module_commands.h"
#include "json_fluent.h"
/* openai_chat.c: inference-backed handlers for the OpenAI-compatible
 * /v1/chat/completions and /v1/completions routes.
 *
 * Registered into the server_http dispatch via server_http_set_*_handler at
 * server startup (openai_chat_register). Keeping the inference here — rather
 * than in server_http.c — keeps the agent/provider dependency closure out of
 * the server_http translation unit and its unit test (which injects a stub
 * handler instead).
 *
 * Native wire formats cross translation at ingress/egress. Streaming routes
 * render canonical responses through the HTTP SSE seam. Async /v1/runs owns
 * the selected registration and complete canonical input; its bounded live
 * records expose the durable session reference. */
#include "server_http.h"
#include "openai_shape.h"
#include "ingress_preinject.h"
#include <aimee/gateway/gateway_pipeline.h> /* gw_request_t + gw_pipeline_run_request — shared seam */
#include "ir_host_bindings.h"
#include "gw_stage_registry.h"                  /* Slice 7: config-driven stage catalog */
#include "gw_stage_governance.h"                /* response seam Slice 2: togglable governance */
#include "gw_stage_completion.h"                /* bounded incomplete-repair continuation */
#include "dogfood.h"                            /* dogfood_autolabel_next_turn_live */
#include "modules/learning/learning_implicit.h" /* learning_implicit_detect_turn */
#include "retrieval_outcome_bridge.h"           /* retrieval_outcome_bridge_on_autolabel */
#include "openai_runs_store.h"                  /* GET /v1/runs/{id} record store */
#include "aimee.h" /* EMBED_MAX_DIM, MAX_PATH_LEN (used by agent_types.h below) */
#include "log.h"   /* LOG_WARN: name the provider's error instead of discarding it */
#include "aimee_errors.h"
#include "config.h" /* legacy_config_record, legacy_config_read */
#include "agent_config.h"
#include "agent_exec.h"
#include "agent_request_build.h"
#include "agent_protocol.h" /* message_history_repair */
#include "agent_response_projection.h"
#include <aimee/ir/session_event.h>
#include <aimee/translation/aimee_frontend.h>
#include <aimee/translation/aimee_backend.h>
#include "model_registry.h"                  /* configured/catalog context for model discovery */
#include <aimee/delegates/delegate_driver.h> /* single provider step for the Codex proxy */
#include "http_retry.h"
#include "wire_fence.h"
#include "gateway_mutate_wire.h"
#include "server_http_identity.h"
#include "cJSON.h"
#include <aimee/tools/agent_tools.h> /* agent_tools_set_tool_event_cb — /v1/runs tool events */
#include "agent_types.h"
#include <aimee/gateway/gateway_policy.h> /* gateway_policy_apply_request — tool-policing stage */
#include "router_advise.h"                /* gw_stage_router — the request->workflow seam */
#include "aimee_ir_serve.h"               /* IR-routed /v1/responses parse */
#include "request_context.h"
#include "db1_client/server_sessions.h"
#include "db1_client/primary_sessions.h"
#include "platform_random.h"
#include "aimee_sha256.h"
#include "response_dedup.h"
#include "token_tracker.h"
#include "token_audit.h" /* db1_token_audit_insert for the avoided-turn row */
#include <pthread.h>
#include <openssl/crypto.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OPENAI_CHAT_MAX_TOKENS  2048
#define OPENAI_CHAT_TEMPERATURE 0.7

/* Record a dedup cache hit as a non-billable usage_kind=avoided audit row: the
 * provider call was skipped, so the avoided estimated cost is logged separately
 * and never added to spend totals (see db1_token_audit_spend_breakdown). Tokens
 * are zero because no real call happened. */
static void record_avoided_turn(const char *model, double avoided_cost)
{
   /* Carry the request-context attribution (#3) so an avoided row is attributed
    * to the same principal/request as a realized one. The avoided cost is a
    * special value (the prior realized cost replayed), so this writes directly
    * rather than via agent_record_token_audit (which recomputes cost from tokens
    * that are zero here); db1_token_audit_insert still applies the idempotency
    * guard on (source, request_id, attempt). */
   const request_context_t *rc = request_context_get();
   db1_token_audit_row_t row = {
       /* Attribute the avoided saving to the same source/session boundary as a
        * realized request: a trusted per-client source when stamped, else the
        * ingress origin; session_id mirrors the realized path's session(). */
       .session_id = session_id(),
       .tool_name = model && model[0] ? model : "aimee",
       .role = "",
       .model = model && model[0] ? model : "aimee",
       .source = (rc && rc->source[0]) ? rc->source : "openai-ingress",
       .requested_model = model ? model : "",
       .usage_kind = "avoided",
       .request_id = rc ? rc->request_id : "",
       .idempotency_key = rc ? rc->idempotency_key : "",
       .principal = rc ? rc->principal : "",
       .estimated_cost_usd = avoided_cost,
   };
   (void)db1_token_audit_insert(&row);
}

static int openai_governance_enabled(void);

/* Dedup eligibility for the buffered completion path (§4): only when the flag is
 * on and the client supplied an explicit Idempotency-Key. The caller has already
 * rejected streaming, and this path runs no tools — both v1 requirements. The key
 * is built from the RESOLVED backend (ag), so it is computed after agent/config
 * resolution and two requests resolving to a different backend never collide.
 * Returns -1 when the required response-composition module cannot produce a key. */
static int dedup_eligible(char *key, size_t key_cap, const agent_t *ag, const char *endpoint,
                          const char *body, const char *context, int *ttl_out)
{
   if (!config_present() || !config_dedup_enabled())
      return 0;
   const char *idem = request_context_idempotency_key();
   if (!idem || !idem[0])
      return 0;
   const request_context_t *rc = request_context_get();
   /* Behaviour-affecting config flags that can change generation for this path. */
   char flags[96];
   snprintf(flags, sizeof(flags), "ir1-sha256 cs%d rc%d pi%d dw%d gv%d sp%d",
            config_cache_shaping_enabled(), config_reasoning_cap_enabled(),
            config_ingress_preinject_enabled(), config_dedup_window_seconds(),
            openai_governance_enabled(), gateway_prevent_subagents_enabled());
   /* Key composition needs commitments, not a second copy of a potentially
    * megabyte-scale prompt in the bounded bus frame. Domain separation keeps
    * this canonical-input key format distinct from legacy raw-body callers. */
   char body_digest[65], context_digest[65];
   if (aimee_sha256_hex(body ? body : "", body ? strlen(body) : 0, body_digest) ||
       aimee_sha256_hex(context ? context : "", context ? strlen(context) : 0, context_digest))
      return -1;
   response_dedup_key_inputs_t in = {
       .principal = request_context_principal(),
       .source = rc ? rc->source : "",
       .provider = ag ? ag->provider : "",
       .model = ag ? ag->model : "",
       .endpoint = endpoint,
       .stream = 0,
       .idempotency_key = idem,
       .body = body_digest,
       .context = context_digest,
       .behavior_flags = flags,
   };
   if (response_dedup_key(&in, key, key_cap) != 0)
      return -1;
   if (ttl_out)
      *ttl_out = config_dedup_window_seconds() > 0 ? config_dedup_window_seconds()
                                                   : RESPONSE_DEDUP_TTL_SECONDS;
   return 1;
}

/* The short-window cache stores canonical responses, never flattened client JSON. */
typedef struct
{
   const char *body, *endpoint;
   char key[288], client_id[64];
   long created;
   int enabled, avoided, ttl;
} openai_ir_cache_t;
static int agent_execute_prepared_ir_request(const agent_t *agent, aimee_request_t *request,
                                             aimee_response_t *out, char *error, size_t capacity,
                                             int preserve, openai_ir_cache_t *cache);
static int openai_prepare_ir_request(aimee_request_t *request, const agent_t *agent, int max_tokens,
                                     double temperature, int preserve);
static int openai_admit_completion(const char *body, int chat, aimee_request_t *request,
                                   char *error, size_t capacity);

/* Owned execution helpers are defined below the client render boundary. */
static int agent_execute_ir_request(const agent_t *agent, aimee_request_t *request, int max_tokens,
                                    double temperature, aimee_response_t *out, char *error,
                                    size_t capacity, int preserve_messages);
static cJSON *openai_ir_history_json(const aimee_request_t *request);
static int openai_ir_history_replace(aimee_request_t *request, const cJSON *history);

/* Shared body for both endpoints. chat != 0 selects the chat.completion shape
 * (and chat-request parse); otherwise the legacy text_completion shape. */
/* A connector that packs/counts chat messages must keep that exact role shape.
 * Opt in explicitly; legacy text callers retain their existing assembly path. */
static int openai_request_preserves_messages(const char *body)
{
   cJSON *root = cJSON_Parse(body ? body : "");
   int preserve = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "preserve_messages"));
   cJSON_Delete(root);
   return preserve;
}

static int openai_admit_completion(const char *body, int chat, aimee_request_t *request,
                                   char *error, size_t capacity)
{
   int status = chat ? aimee_frontend_parse_wire(body, body ? strlen(body) : 0,
                                                 AIMEE_WIRE_OPENAI_CHAT, request, error, capacity)
                     : aimee_frontend_parse_text_completion(body, body ? strlen(body) : 0, request,
                                                            error, capacity);
   if (status || aimee_ir_request_admit(request))
      return -1;
   if (chat && (!cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(request->raw, "messages")) ||
                !cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(request->raw, "messages"))))
   {
      snprintf(error, capacity, "messages must be a non-empty array");
      return -1;
   }
   return 0;
}

static int run_completion(int chat, const char *body, char *resp, int cap)
{
   aimee_request_t request = {0};
   aimee_response_t response = {0};
   char error[512] = "invalid canonical completion request";
   char *model = NULL;
   int status = 400;
   openai_ir_cache_t cache = {.body = body,
                              .endpoint = chat ? "/v1/chat/completions" : "/v1/completions",
                              .created = (long)time(NULL)};
   snprintf(cache.client_id, sizeof cache.client_id, "%s-%ld", chat ? "chatcmpl" : "cmpl",
            cache.created);
   if (openai_admit_completion(body, chat, &request, error, sizeof error))
      goto failure;
   if (request.stream)
   {
      snprintf(error, sizeof error, "streaming request requires the streaming transport");
      goto failure;
   }
   model = strdup(request.model && request.model[0] ? request.model : "aimee");
   if (!model)
      goto failure;
   agent_t agent;
   if (agent_registry_resolve_ingress_model(model, &agent))
   {
      status = strcmp(model, "aimee") ? 404 : 503;
      snprintf(error, sizeof error, "the requested model is not available");
      goto failure;
   }
   int preserve = chat && openai_request_preserves_messages(body);
   if (openai_prepare_ir_request(
           &request, &agent, request.has_max_tokens ? request.max_tokens : OPENAI_CHAT_MAX_TOKENS,
           OPENAI_CHAT_TEMPERATURE, preserve))
   {
      status = 500;
      snprintf(error, sizeof error, "cannot prepare canonical completion request");
      goto failure;
   }
   if (agent_execute_prepared_ir_request(&agent, &request, &response, error, sizeof error, preserve,
                                         &cache))
   {
      status = wire_fence_error_http_status(error);
      goto failure;
   }
   if (!chat && aimee_ir_response_tool_count(&response))
   {
      status = 502;
      snprintf(error, sizeof error, "text completion cannot represent a tool call");
      goto failure;
   }
   if (!cache.avoided && agent_ingress_accounting_enabled())
      agent_ingress_record_cost(
          agent.name, agent.model, model,
          response.raw_stop_reason ? response.raw_stop_reason
                                   : aimee_stop_reason_name(response.stop_reason),
          (int)response.usage_in, (int)response.usage_out, (int)response.usage_cache_write,
          (int)response.usage_cache_read, "openai-ingress", NULL);
   char *text = chat ? NULL : aimee_ir_response_text_alloc(&response);
   int length = chat   ? aimee_openai_buffered_response(&response, cache.client_id, model,
                                                        cache.created, 0, resp, cap)
                : text ? openai_format_text_completion(cache.client_id, model, text, cache.created,
                                                       (int)response.usage_in,
                                                       (int)response.usage_out, resp, cap)
                       : -1;
   free(text);
   status = length >= 0 ? 200 : 500;
   if (status != 200)
      openai_format_error(resp, cap, "server_error", "response did not fit the buffer");
   goto cleanup;
failure:
   openai_format_error(resp, cap,
                       status == 400   ? "invalid_request_error"
                       : status == 404 ? "model_not_found"
                                       : wire_fence_error_type(error),
                       error);
cleanup:
   free(model);
   aimee_request_free(&request);
   aimee_response_free(&response);
   return status;
}

static int chat_completions_handler(const char *body, char *resp, int cap)
{
   return run_completion(1, body, resp, cap);
}

static int completions_handler(const char *body, char *resp, int cap)
{
   return run_completion(0, body, resp, cap);
}

/* GET /v1/models provider: copy each configured agent name (capped at max) so
 * clients can target a specific (provider,model) binding via the `model`
 * field. Names are copied into the caller's slots — no escaping pointers. */
static int models_provider(char ids[][SERVER_HTTP_MODEL_ID_MAX], int max)
{
   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0)
      return 0;
   int k = 0;
   for (int i = 0; i < acfg.agent_count && k < max; i++)
   {
      if (!acfg.agents[i].name[0] || strcmp(acfg.agents[i].name, "aimee") == 0)
         continue; /* "aimee" is already advertised by the route */
      if (!agent_name_valid(acfg.agents[i].name))
         continue; /* never surface junk/over-long names as models (defensive) */
      snprintf(ids[k], SERVER_HTTP_MODEL_ID_MAX, "%s", acfg.agents[i].name);
      k++;
   }
   return k;
}

/* Append one Codex model-discovery entry (the proprietary schema Codex's model
 * manager expects). slug is what Codex echoes in the request `model` field to
 * select this primary-agent model. The full field set mirrors Codex's own
 * models cache so its deserializer accepts the entry. */
static void codex_add_model(cJSON *models, const char *slug, const char *description,
                            int context_window)
{
   cJSON *m = cJSON_CreateObject();
   if (!m)
      return;
   cJSON_AddStringToObject(m, "slug", slug);
   cJSON_AddStringToObject(m, "display_name", slug);
   cJSON_AddStringToObject(m, "description",
                           description && description[0] ? description : "Aimee primary agent.");
   cJSON_AddStringToObject(m, "default_reasoning_level", "medium");
   cJSON *levels = cJSON_AddArrayToObject(m, "supported_reasoning_levels");
   static const char *const effs[] = {"low", "medium", "high"};
   for (int i = 0; levels && i < 3; i++)
   {
      cJSON *lv = cJSON_CreateObject();
      cJSON_AddStringToObject(lv, "effort", effs[i]);
      cJSON_AddStringToObject(lv, "description", effs[i]);
      cJSON_AddItemToArray(levels, lv);
   }
   cJSON_AddStringToObject(m, "shell_type", "shell_command");
   cJSON_AddStringToObject(m, "visibility", "list");
   cJSON_AddBoolToObject(m, "supported_in_api", 1);
   cJSON_AddNumberToObject(m, "priority", 1);
   cJSON_AddItemToObject(m, "additional_speed_tiers", cJSON_CreateArray());
   cJSON_AddItemToObject(m, "service_tiers", cJSON_CreateArray());
   cJSON *nux = cJSON_AddObjectToObject(m, "availability_nux");
   if (nux)
      cJSON_AddStringToObject(nux, "message", "");
   cJSON_AddNullToObject(m, "upgrade");
   cJSON_AddStringToObject(m, "base_instructions", "");
   cJSON *mm = cJSON_AddObjectToObject(m, "model_messages");
   if (mm)
   {
      cJSON_AddStringToObject(mm, "instructions_template", "");
      cJSON_AddObjectToObject(mm, "instructions_variables");
   }
   cJSON_AddBoolToObject(m, "supports_reasoning_summaries", 0);
   cJSON_AddStringToObject(m, "default_reasoning_summary", "none");
   cJSON_AddBoolToObject(m, "support_verbosity", 0);
   cJSON_AddStringToObject(m, "default_verbosity", "low");
   cJSON_AddStringToObject(m, "apply_patch_tool_type", "freeform");
   cJSON_AddStringToObject(m, "web_search_tool_type", "text_and_image");
   cJSON *trunc = cJSON_AddObjectToObject(m, "truncation_policy");
   if (trunc)
   {
      cJSON_AddStringToObject(trunc, "mode", "tokens");
      cJSON_AddNumberToObject(trunc, "limit", 10000);
   }
   cJSON_AddBoolToObject(m, "supports_parallel_tool_calls", 1);
   cJSON_AddBoolToObject(m, "supports_image_detail_original", 0);
   cJSON_AddNumberToObject(m, "context_window", context_window);
   cJSON_AddNumberToObject(m, "max_context_window", context_window);
   cJSON_AddNumberToObject(m, "effective_context_window_percent", 95);
   cJSON_AddItemToObject(m, "experimental_supported_tools", cJSON_CreateArray());
   cJSON *mods = cJSON_AddArrayToObject(m, "input_modalities");
   if (mods)
      cJSON_AddItemToArray(mods, cJSON_CreateString("text"));
   cJSON_AddBoolToObject(m, "supports_search_tool", 0);
   cJSON_AddItemToArray(models, m);
}

/* GET /v1/models (raw): emit the Codex `{models:[…]}` discovery schema from the
 * registered primary-agent models, alongside the OpenAI `{object,data}` list for
 * other clients. One `models` entry per agent (plus the default "aimee"); the
 * slug is the id Codex puts in the request `model` field to switch models. */
#define CODEX_FALLBACK_CONTEXT_WINDOW 32768

/* Codex uses this advertised value to budget and truncate the ordinary API
 * session.  It therefore has to describe the selected delegate, not Codex's own
 * historical default.  Prefer the operator's per-agent override, then the model
 * catalog, and finally a conservative compatibility fallback. */
static int codex_agent_context_window(const agent_t *agent)
{
   if (agent && agent->middleware.context_window > 0)
      return agent->middleware.context_window;
   if (agent)
   {
      model_capability_t cap;
      memset(&cap, 0, sizeof cap);
      if (model_capability_get(agent_catalog_provider(agent), agent->model, &cap) &&
          cap.context_window > 0)
         return cap.context_window;
   }
   return CODEX_FALLBACK_CONTEXT_WINDOW;
}

static int codex_models_raw(char *resp, int cap)
{
   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0)
      return -1;

   cJSON *root = cJSON_CreateObject();
   if (!root)
      return -1;
   cJSON_AddStringToObject(root, "object", "list");
   cJSON *data = cJSON_AddArrayToObject(root, "data");
   cJSON *models = cJSON_AddArrayToObject(root, "models");
   if (!data || !models)
   {
      cJSON_Delete(root);
      return -1;
   }

   /* Always advertise the default "aimee" model, then each named agent. */
   int seen_aimee = 0;
   agent_t *primary = agent_default_primary(&acfg);
   codex_add_model(models, "aimee", "Aimee primary agent.", codex_agent_context_window(primary));
   cJSON *de = cJSON_CreateObject();
   cJSON_AddStringToObject(de, "id", "aimee");
   cJSON_AddStringToObject(de, "object", "model");
   cJSON_AddStringToObject(de, "owned_by", "aimee");
   cJSON_AddItemToArray(data, de);

   for (int i = 0; i < acfg.agent_count; i++)
   {
      const char *nm = acfg.agents[i].name;
      if (!nm || !nm[0] || strcmp(nm, "aimee") == 0)
      {
         if (nm && strcmp(nm, "aimee") == 0)
            seen_aimee = 1;
         continue;
      }
      codex_add_model(models, nm, acfg.agents[i].model,
                      codex_agent_context_window(&acfg.agents[i]));
      cJSON *e = cJSON_CreateObject();
      cJSON_AddStringToObject(e, "id", nm);
      cJSON_AddStringToObject(e, "object", "model");
      cJSON_AddStringToObject(e, "owned_by", "aimee");
      cJSON_AddItemToArray(data, e);
   }
   (void)seen_aimee;

   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!s)
      return -1;
   int len = snprintf(resp, (size_t)cap, "%s", s);
   free(s);
   return (len < 0 || len >= cap) ? -1 : len;
}

/* POST /v1/embeddings: embed each input via the configured embedder
 * (the shared Go embedding command) and shape the OpenAI embeddings list. Returns 502 when
 * the embedder is unavailable (e.g. the sidecar isn't running). */
static int embeddings_handler(const char *body, char *resp, int cap)
{
   char model[64] = "";
   char **inputs = NULL;
   int n = 0;
   if (openai_parse_embeddings_request(body, model, sizeof(model), &inputs, &n) != 0)
   {
      openai_format_error(resp, cap, "invalid_request_error",
                          "invalid embeddings request: expected `input` string or array");
      return 400;
   }
   const char *cmd = config_embedder_command_current(NULL);

   float **vecs = calloc((size_t)n, sizeof(float *));
   int *dims = calloc((size_t)n, sizeof(int));
   if (!vecs || !dims)
   {
      free(vecs);
      free(dims);
      openai_free_inputs(inputs, n);
      openai_format_error(resp, cap, "server_error", "out of memory");
      return 500;
   }

   int ok = 1;
   int prompt_chars = 0;
   for (int i = 0; i < n; i++)
   {
      vecs[i] = malloc(sizeof(float) * EMBED_MAX_DIM);
      if (!vecs[i])
      {
         ok = 0;
         break;
      }
      cJSON *embed_0_args = cJSON_CreateObject(), *embed_0_reply = NULL;
      cJSON_AddStringToObject(embed_0_args, "base_url", cmd);
      cJSON_AddStringToObject(embed_0_args, "input_type", "document");
      cJSON_AddStringToObject(embed_0_args, "text", inputs[i]);
      cJSON_AddNumberToObject(embed_0_args, "max_dim", EMBED_MAX_DIM);
      (void)aimee_module_commands_dispatch_internal("memory.embed_text", embed_0_args,
                                                    &embed_0_reply);
      cJSON_Delete(embed_0_args);
      int d = jo_float_array(cJSON_GetObjectItemCaseSensitive(embed_0_reply, "vector"), vecs[i],
                             EMBED_MAX_DIM);
      cJSON_Delete(embed_0_reply);
      if (d <= 0)
      {
         ok = 0;
         break;
      }
      dims[i] = d;
      prompt_chars += (int)strlen(inputs[i]);
   }

   int status;
   if (!ok)
   {
      openai_format_error(resp, cap, "upstream_error",
                          "embedding generation failed (is the embedder available?)");
      status = 502;
   }
   else
   {
      /* ~4 chars/token is the usual OpenAI rule of thumb. */
      int len = openai_format_embeddings(model, (const float *const *)vecs, dims, n,
                                         prompt_chars / 4, resp, cap);
      if (len < 0)
      {
         openai_format_error(resp, cap, "server_error",
                             "embeddings response did not fit the buffer");
         status = 500;
      }
      else
         status = 200;
   }

   for (int i = 0; i < n; i++)
      free(vecs[i]);
   free(vecs);
   free(dims);
   openai_free_inputs(inputs, n);
   return status;
}

/* Unique /v1/responses id (the listener serves one connection at a time, so the
 * counter needs no lock); distinct ids let a client chain turns even within the
 * same wall-clock second. Shared by the unary and streaming handlers. */
static int responses_mint_id(long created, char *buffer, size_t capacity)
{
   (void)created;
   char random[33];
   if (capacity < 38 || platform_random_hex(random, 32))
      return -1;
   snprintf(buffer, capacity, "resp_%s", random);
   return 0;
}

/* Responses continuation is a canonical checkpoint in the existing durable
 * session log. The authenticated actor and, when supplied, parent session scope
 * must match on every load; an unknown id never becomes an empty conversation. */
static int responses_history_scope(char scope[80])
{
   const char *session = server_http_identity_session_hdr();
   if (!session || !session[0])
   {
      snprintf(scope, 80, "responses");
      return 0;
   }
   char hash[65];
   if (aimee_sha256_hex(session, strlen(session), hash))
      return -1;
   snprintf(scope, 80, "responses:%s", hash);
   return 0;
}
static int responses_restore_history(aimee_request_t *request, char *error, size_t capacity)
{
   const cJSON *previous = cJSON_GetObjectItemCaseSensitive(request->raw, "previous_response_id");
   if (!previous || cJSON_IsNull(previous))
      return 0;
   const char *id = cJSON_GetStringValue(previous);
   const char *actor = server_http_identity_principal();
   char scope[80], *json = NULL;
   if (!id || strncmp(id, "resp_", 5) || strlen(id) != 37 ||
       strspn(id + 5, "0123456789abcdef") != 32 || !actor || !actor[0] ||
       responses_history_scope(scope))
   {
      snprintf(error, capacity, "invalid or unauthorized previous_response_id");
      return -1;
   }
   int status = db1_session_ir_history_load(id, scope, actor, "", &json);
   if (status)
   {
      snprintf(error, capacity, "%s",
               status > 0 ? "previous_response_id was not found in this session scope"
                          : "durable Responses history is unavailable");
      return -1;
   }
   cJSON *history = cJSON_Parse(json);
   free(json);
   cJSON *current = openai_ir_history_json(request);
   if (!cJSON_IsArray(history) || !current)
   {
      cJSON_Delete(history);
      cJSON_Delete(current);
      return -1;
   }
   while (cJSON_GetArraySize(current))
   {
      cJSON *message = cJSON_DetachItemFromArray(current, 0);
      if (!cJSON_AddItemToArray(history, message))
      {
         cJSON_Delete(message);
         cJSON_Delete(history);
         cJSON_Delete(current);
         return -1;
      }
   }
   cJSON_Delete(current);
   int rc = openai_ir_history_replace(request, history);
   cJSON_Delete(history);
   return rc;
}
static int responses_store_history(const char *id, const aimee_request_t *request,
                                   const aimee_response_t *response)
{
   const char *actor = server_http_identity_principal();
   char scope[80];
   if (!actor || !actor[0] || responses_history_scope(scope))
      return -1;
   cJSON *history = openai_ir_history_json(request);
   aimee_message_t assistant = {
       .role = "assistant", .blocks = response->content, .n_blocks = response->n_content};
   cJSON *message = aimee_ir_message_json(&assistant);
   if (!history || !message || !cJSON_AddItemToArray(history, message))
   {
      cJSON_Delete(history);
      cJSON_Delete(message);
      return -1;
   }
   char *json = cJSON_PrintUnformatted(history);
   cJSON_Delete(history);
   int rc = json && !db1_server_session_create(id, "responses", actor)
                ? db1_session_ir_history_save(id, "response", scope, actor, json)
                : -1;
   free(json);
   return rc;
}
static int responses_admit_request(const char *body, aimee_request_t *request, char *error,
                                   size_t capacity)
{
   if (aimee_frontend_parse_wire(body, body ? strlen(body) : 0, AIMEE_WIRE_RESPONSES, request,
                                 error, capacity))
      return -1;
   const cJSON *input = cJSON_GetObjectItemCaseSensitive(request->raw, "input");
   if ((!cJSON_IsString(input) && !cJSON_IsArray(input)) || aimee_ir_request_admit(request))
   {
      snprintf(error, capacity, "invalid canonical Responses input");
      return -1;
   }
   if (responses_restore_history(request, error, capacity) || aimee_ir_request_admit(request))
      return -1;
   return 0;
}
static void responses_account(const agent_t *agent, const char *model,
                              const aimee_response_t *response)
{
   if (agent_ingress_accounting_enabled())
      agent_ingress_record_cost(
          agent->name, agent->model, model,
          response->raw_stop_reason ? response->raw_stop_reason
                                    : aimee_stop_reason_name(response->stop_reason),
          (int)response->usage_in, (int)response->usage_out, (int)response->usage_cache_write,
          (int)response->usage_cache_read, "openai-ingress", NULL);
}
static int responses_handler(const char *body, char *buffer, int capacity)
{
   aimee_request_t request = {0};
   aimee_response_t response = {0};
   char error[512] = "invalid Responses request";
   int status = 400;
   if (responses_admit_request(body, &request, error, sizeof error))
      goto failed;
   if (request.stream)
   {
      snprintf(error, sizeof error, "streaming request requires the streaming route");
      goto failed;
   }
   agent_t agent;
   if (agent_registry_resolve_ingress_model(request.model ? request.model : "", &agent))
   {
      status = 404;
      snprintf(error, sizeof error, "the requested model is not available");
      goto failed;
   }
   char *model = strdup(request.model ? request.model : agent.model);
   if (!model)
   {
      status = 500;
      goto failed;
   }
   if (agent_execute_ir_request(&agent, &request, 0, -1, &response, error, sizeof error, 0))
   {
      status = wire_fence_error_http_status(error);
      free(model);
      goto failed;
   }
   responses_account(&agent, model, &response);
   char id[64];
   if (responses_mint_id((long)time(NULL), id, sizeof id) ||
       responses_store_history(id, &request, &response))
   {
      status = 503;
      snprintf(error, sizeof error, "durable Responses history could not be committed");
      free(model);
      goto failed;
   }
   int length =
       aimee_openai_buffered_response(&response, id, model, (long)time(NULL), 1, buffer, capacity);
   free(model);
   aimee_request_free(&request);
   aimee_response_free(&response);
   if (length >= 0)
      return 200;
   openai_format_error(buffer, capacity, "server_error",
                       "canonical response did not fit the buffer");
   return 500;
failed:
   openai_format_error(buffer, capacity, status == 400 ? "invalid_request_error" : "server_error",
                       error);
   aimee_request_free(&request);
   aimee_response_free(&response);
   return status;
}

/* SSE streaming for POST /v1/chat/completions (stream:true). First cut:
 * compute the full completion (non-incremental) and emit it as OpenAI
 * chat.completion.chunk frames — a role frame, the content split into deltas,
 * and a terminal finish frame. server_http wraps each frame as `data: …` and
 * appends `data: [DONE]`. Provider-incremental token streaming is a follow-up;
 * this already lets OpenAI streaming clients work end-to-end. */
#define OPENAI_STREAM_CHUNK 80

static void emit_chunk(server_http_sse_emit emit, void *ctx, const char *id, const char *model,
                       long created, int role, const char *content, int finish)
{
   char frame[1024];
   if (openai_format_chat_chunk(id, model, created, role, content, finish, frame, sizeof(frame)) >
       0)
      emit(ctx, frame);
}

static void emit_text_chunk(server_http_sse_emit emit, void *ctx, const char *id, const char *model,
                            long created, const char *text, int finish)
{
   char frame[1024];
   if (openai_format_text_chunk(id, model, created, text, finish, frame, sizeof(frame)) > 0)
      emit(ctx, frame);
}

/* Fills `out` with the selected agent; returns 0 on success.
 *
 * Took an agent_config_t* and returned a pointer into it, which meant every
 * caller put 350,968 bytes on the stack of a request thread to obtain one
 * 16,720-byte agent. It now asks the registry for just that agent. */
static int stream_pick_agent(agent_t *out, const char *model)
{
   return agent_registry_resolve_ingress_model(model, out);
}

static int completion_stream_ir(int chat, const char *body, server_http_sse_emit emit, void *ctx)
{
   aimee_request_t request = {0};
   aimee_response_t response = {0};
   char error[512] = "invalid canonical completion request";
   char *model = NULL;
   long created = (long)time(NULL);
   char id[64];
   snprintf(id, sizeof id, "%s-%ld", chat ? "chatcmpl" : "cmpl", created);
   if (openai_admit_completion(body, chat, &request, error, sizeof error))
      goto failure;
   model = strdup(request.model && request.model[0] ? request.model : "aimee");
   if (!model)
      goto failure;
   agent_t agent;
   if (agent_registry_resolve_ingress_model(model, &agent))
   {
      snprintf(error, sizeof error, "the requested model is not available");
      goto failure;
   }
   int preserve = chat && openai_request_preserves_messages(body);
   if (agent_execute_ir_request(
           &agent, &request, request.has_max_tokens ? request.max_tokens : OPENAI_CHAT_MAX_TOKENS,
           OPENAI_CHAT_TEMPERATURE, &response, error, sizeof error, preserve))
      goto failure;
   if (!chat && aimee_ir_response_tool_count(&response))
   {
      snprintf(error, sizeof error, "text completion cannot represent a tool call");
      goto failure;
   }
   if (agent_ingress_accounting_enabled())
      agent_ingress_record_cost(
          agent.name, agent.model, model,
          response.raw_stop_reason ? response.raw_stop_reason
                                   : aimee_stop_reason_name(response.stop_reason),
          (int)response.usage_in, (int)response.usage_out, (int)response.usage_cache_write,
          (int)response.usage_cache_read, "openai-ingress", NULL);
   if (chat)
   {
      emit_chunk(emit, ctx, id, model, created, 1, NULL, 0);
      if (aimee_openai_emit_chat_response(&response, id, model, created, emit, ctx))
         goto failure;
   }
   else
   {
      char *text = aimee_ir_response_text_alloc(&response);
      if (!text)
         goto failure;
      text_sanitize_utf8(text);
      size_t length = strlen(text);
      for (size_t offset = 0; offset < length;)
      {
         char segment[OPENAI_STREAM_CHUNK + 1];
         size_t take = text_utf8_chunk_len(text + offset, length - offset, OPENAI_STREAM_CHUNK);
         memcpy(segment, text + offset, take);
         segment[take] = 0;
         emit_text_chunk(emit, ctx, id, model, created, segment, 0);
         offset += take;
      }
      free(text);
      emit_text_chunk(emit, ctx, id, model, created, "", 1);
   }
   goto cleanup;
failure:
{
   char failure[1024];
   openai_format_error(failure, sizeof failure, wire_fence_error_type(error), error);
   emit(ctx, failure);
}
cleanup:
   if (chat)
      emit(ctx, "[DONE]");
   free(model);
   aimee_request_free(&request);
   aimee_response_free(&response);
   return 0;
}
static int chat_stream_handler(const char *body, server_http_sse_emit emit, void *ctx)
{
   return completion_stream_ir(1, body, emit, ctx);
}
static int completion_stream_handler(const char *body, server_http_sse_emit emit, void *ctx)
{
   return completion_stream_ir(0, body, emit, ctx);
}

/* Shared canonical request preparation and provider execution. */

static cJSON *openai_ir_history_json(const aimee_request_t *request)
{
   cJSON *history = cJSON_CreateArray();
   if (!history)
      return NULL;
   for (int i = 0; i < request->n_messages; i++)
   {
      cJSON *message = aimee_ir_message_json(&request->messages[i]);
      if (!message || !cJSON_AddItemToArray(history, message))
      {
         cJSON_Delete(message);
         cJSON_Delete(history);
         return NULL;
      }
   }
   return history;
}
static int openai_ir_history_replace(aimee_request_t *request, const cJSON *history)
{
   int count = cJSON_GetArraySize(history);
   if (!cJSON_IsArray(history) || count > AIMEE_IR_MAX_BLOCKS)
      return -1;
   aimee_request_t replacement = {0};
   replacement.messages = count ? calloc((size_t)count, sizeof *replacement.messages) : NULL;
   if (count && !replacement.messages)
      return -1;
   replacement.n_messages = count;
   for (int i = 0; i < count; i++)
      if (aimee_ir_message_parse(cJSON_GetArrayItem(history, i), &replacement.messages[i]))
      {
         aimee_request_free(&replacement);
         return -1;
      }
   for (int i = 0; i < request->n_messages; i++)
      aimee_message_free(&request->messages[i]);
   free(request->messages);
   request->messages = replacement.messages;
   request->n_messages = replacement.n_messages;
   request->mutated = 1;
   return 0;
}
static int openai_ir_append_policy(aimee_request_t *request, char *policy)
{
   if (!policy)
      return 0;
   if (request->n_system >= AIMEE_IR_MAX_BLOCKS)
   {
      free(policy);
      return -1;
   }
   aimee_block_t *blocks =
       realloc(request->system, (size_t)(request->n_system + 1) * sizeof *blocks);
   if (!blocks)
   {
      free(policy);
      return -1;
   }
   request->system = blocks;
   aimee_block_t *block = &blocks[request->n_system++];
   *block = (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = policy};
   aimee_ir_block_set_context(block, AIMEE_CTX_ORIGIN_PLATFORM, AIMEE_CTX_AUTH_POLICY,
                              AIMEE_CTX_TRUST_VERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL, 0);
   request->mutated = 1;
   return 0;
}
static char *openai_build_ir_body(aimee_request_t *request, aimee_wire_t wire)
{
   if (aimee_frontend_guard_state(request, NULL, 0))
      return NULL;
   if (aimee_ir_request_admit(request))
      return NULL;
   char *body = aimee_backend_render_wire(request, wire);
   if (!body)
      return NULL;
   /* A Chat provider's explicitly counted template extension stays at its
    * native boundary. It is never turned into model content or another wire. */
   const cJSON *kwargs =
       request->frontend == AIMEE_WIRE_OPENAI_CHAT && wire == AIMEE_WIRE_OPENAI_CHAT
           ? cJSON_GetObjectItemCaseSensitive(request->raw, "chat_template_kwargs")
           : NULL;
   if (cJSON_IsObject(kwargs))
   {
      cJSON *native = cJSON_Parse(body);
      free(body);
      cJSON_DeleteItemFromObjectCaseSensitive(native, "chat_template_kwargs");
      if (!cJSON_AddItemToObject(native, "chat_template_kwargs", cJSON_Duplicate(kwargs, 1)))
      {
         cJSON_Delete(native);
         return NULL;
      }
      body = cJSON_PrintUnformatted(native);
      cJSON_Delete(native);
   }
   return body;
}
static int openai_prepare_ir_request(aimee_request_t *request, const agent_t *agent, int max_tokens,
                                     double temperature, int preserve_messages)
{
   if (aimee_ir_request_admit(request))
      return -1;
   char *model = strdup(agent->model);
   if (!model)
      return -1;
   free(request->model);
   request->model = model; /* Registered agent selection resolves to its actual provider model. */
   request->stream = 0;    /* This transport buffers; the provider profile may require SSE. */
   if (max_tokens > 0)
   {
      request->max_tokens = agent_request_max_tokens(agent, max_tokens);
      request->has_max_tokens = 1;
   }
   if (!request->has_max_tokens && !strcmp(agent->provider, "anthropic"))
   {
      request->max_tokens = agent_request_max_tokens(agent, 0);
      request->has_max_tokens = request->max_tokens > 0;
   }
   if (!request->has_temperature && temperature >= 0)
   {
      request->temperature = temperature;
      request->has_temperature = 1;
   }
   gateway_policy_apply_ir(request);
   router_advise_ir(request);
   if (!preserve_messages)
   {
      if (openai_ir_append_policy(request, gw_ir_request_tool_system_prompt(request, NULL)) ||
          openai_ir_append_policy(request, gw_ir_request_completion_system_prompt(request, NULL)))
         return -1;
      aimee_ir_apply_gateway_request_stages(request);
   }
   return aimee_ir_request_admit(request);
}
static int openai_post_ir(const agent_t *agent, aimee_request_t *request, aimee_wire_t wire,
                          const char *url, const char *auth, const char *headers, char **reply,
                          aimee_anthropic_replay_t *replay, char *error, size_t capacity)
{
   char *body = openai_build_ir_body(request, wire);
   if (!body)
   {
      snprintf(error, capacity, "the selected provider cannot represent the canonical request");
      return HTTP_RETRY_ADMISSION_REFUSED;
   }
   wire_fence_route_t route = wire == AIMEE_WIRE_ANTHROPIC   ? WIRE_FENCE_ANTHROPIC_MESSAGES
                              : wire == AIMEE_WIRE_RESPONSES ? WIRE_FENCE_OPENAI_RESPONSES
                                                             : WIRE_FENCE_OPENAI_CHAT;
   wire_fence_t *snapshot = NULL;
   wire_fence_bytes_t bytes;
   if (wire_fence_select(econ_mode_current() != ECON_MODE_OFF, route, body, strlen(body), &snapshot,
                         &bytes))
   {
      snprintf(error, capacity, "%s", wire_fence_last_error());
      free(body);
      return HTTP_RETRY_ADMISSION_REFUSED;
   }
   if (wire == AIMEE_WIRE_ANTHROPIC &&
       aimee_anthropic_replay_prepare(bytes.data, bytes.len, replay))
   {
      wire_fence_destroy(snapshot);
      free(body);
      snprintf(error, capacity, "cannot bind native replay prefix");
      return HTTP_RETRY_ADMISSION_REFUSED;
   }
   int attempts =
       config_retry_max_attempts() > 0 ? config_retry_max_attempts() : HTTP_RETRY_MAX_ATTEMPTS;
   int base = config_retry_base_ms() > 0 ? config_retry_base_ms() : HTTP_RETRY_BASE_MS;
   int maximum = config_retry_max_ms() > 0 ? config_retry_max_ms() : HTTP_RETRY_MAX_MS;
   int status =
       wire_fence_post(url, auth, bytes.data, bytes.len, reply, agent->timeout_ms, headers,
                       attempts, base, maximum, agent->provider, agent->model, NULL, route);
   wire_fence_destroy(snapshot);
   free(body);
   if (status == HTTP_RETRY_ADMISSION_REFUSED)
      snprintf(error, capacity, "%s", wire_fence_last_error());
   return status;
}
/* Cache payloads are canonical message blocks plus typed response metadata.
 * Reuse therefore needs no native decoder or text reconstruction. */
static char *openai_ir_cache_encode(const aimee_response_t *response,
                                    const openai_ir_cache_t *cache)
{
   aimee_message_t message = {.role = response->role ? response->role : "assistant",
                              .blocks = response->content,
                              .n_blocks = response->n_content};
   cJSON *root = cJSON_CreateObject(), *content = aimee_ir_message_json(&message);
   if (!root || !content || !cJSON_AddItemToObject(root, "message", content))
   {
      cJSON_Delete(content);
      cJSON_Delete(root);
      return NULL;
   }
   int ok = cJSON_AddStringToObject(root, "schema", "aimee.response-cache.ir.v1") &&
            cJSON_AddStringToObject(root, "client_id", cache->client_id) &&
            cJSON_AddNumberToObject(root, "created", cache->created) &&
            cJSON_AddStringToObject(root, "stop_reason",
                                    aimee_stop_reason_name(response->stop_reason)) &&
            cJSON_AddNumberToObject(root, "usage_in", response->usage_in) &&
            cJSON_AddNumberToObject(root, "usage_out", response->usage_out) &&
            cJSON_AddNumberToObject(root, "usage_cache_read", response->usage_cache_read) &&
            cJSON_AddNumberToObject(root, "usage_cache_write", response->usage_cache_write) &&
            cJSON_AddNumberToObject(root, "usage_reasoning", response->usage_reasoning);
   if (response->id)
      ok = ok && cJSON_AddStringToObject(root, "id", response->id);
   if (response->model)
      ok = ok && cJSON_AddStringToObject(root, "model", response->model);
   if (response->raw_stop_reason)
      ok = ok && cJSON_AddStringToObject(root, "raw_stop_reason", response->raw_stop_reason);
   char *encoded = ok ? cJSON_PrintUnformatted(root) : NULL;
   cJSON_Delete(root);
   return encoded;
}
static int openai_ir_cache_decode(const char *encoded, aimee_response_t *response,
                                  openai_ir_cache_t *cache)
{
   cJSON *root = encoded ? cJSON_ParseWithOpts(encoded, NULL, 1) : NULL;
   const char *schema = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "schema"));
   const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "client_id"));
   const char *stop = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "stop_reason"));
   aimee_message_t message = {0};
   long created;
   if (!schema || strcmp(schema, "aimee.response-cache.ir.v1") || !id || !id[0] ||
       strlen(id) >= sizeof cache->client_id || !stop ||
       aimee_ir_read_usage(root, "created", &created) ||
       aimee_ir_message_parse(cJSON_GetObjectItemCaseSensitive(root, "message"), &message))
      goto invalid;
   response->role = message.role;
   response->content = message.blocks;
   response->n_content = message.n_blocks;
   response->stop_reason = aimee_stop_reason_parse(stop);
   if (response->stop_reason == AIMEE_STOP_UNKNOWN && strcmp(stop, "unknown"))
      goto invalid;
   const char *fields[] = {"id", "model", "raw_stop_reason"};
   char **destinations[] = {&response->id, &response->model, &response->raw_stop_reason};
   for (int i = 0; i < 3; i++)
   {
      const cJSON *field = cJSON_GetObjectItemCaseSensitive(root, fields[i]);
      if (field && (!cJSON_IsString(field) || !(*destinations[i] = strdup(field->valuestring))))
         goto invalid;
   }
   if (aimee_ir_read_usage(root, "usage_in", &response->usage_in) ||
       aimee_ir_read_usage(root, "usage_out", &response->usage_out) ||
       aimee_ir_read_usage(root, "usage_cache_read", &response->usage_cache_read) ||
       aimee_ir_read_usage(root, "usage_cache_write", &response->usage_cache_write) ||
       aimee_ir_read_usage(root, "usage_reasoning", &response->usage_reasoning) ||
       aimee_ir_response_admit(response))
      goto invalid;
   snprintf(cache->client_id, sizeof cache->client_id, "%s", id);
   cache->created = created;
   cJSON_Delete(root);
   return 0;
invalid:
   cJSON_Delete(root);
   aimee_response_free(response);
   return -1;
}

static int agent_execute_prepared_ir_request(const agent_t *agent, aimee_request_t *request,
                                             aimee_response_t *out, char *error, size_t capacity,
                                             int preserve_messages, openai_ir_cache_t *cache)
{
   memset(out, 0, sizeof *out);
   snprintf(error, capacity, "canonical provider request failed");
   if (!agent || aimee_ir_request_admit(request))
      return -1;
   delegate_drivers_init();
   const delegate_driver_t *driver = delegate_driver_get(agent->provider);
   char url[MAX_ENDPOINT_LEN + 64], auth[MAX_API_KEY_LEN + 32], headers[512];
   if (!driver || delegate_build_url(driver, agent, url, sizeof url))
   {
      snprintf(error, capacity, "upstream model provider or endpoint is unavailable");
      return -1;
   }
   if (agent_resolve_auth_headers(agent, auth, sizeof auth, headers, sizeof headers))
   {
      snprintf(error, capacity, "upstream model credentials are unavailable");
      return -1;
   }
   aimee_wire_t wire = !strcmp(driver->name, "anthropic") ? AIMEE_WIRE_ANTHROPIC
                       : strstr(url, "/responses")        ? AIMEE_WIRE_RESPONSES
                                                          : AIMEE_WIRE_OPENAI_CHAT;
   gw_mutate_ctx_t mutation;
   gw_mutate_ctx_init(&mutation);
   cJSON *container = NULL;
   if (!preserve_messages && gw_mutate_upstream_ok(wire == AIMEE_WIRE_ANTHROPIC))
   {
      container = cJSON_CreateObject();
      cJSON *history = openai_ir_history_json(request);
      if (!container || !history || !cJSON_AddItemToObject(container, "input", history))
      {
         cJSON_Delete(history);
         cJSON_Delete(container);
         return -1;
      }
      gw_buffered_mutate(container, "input", agent->model, "", server_http_identity_session_hdr(),
                         server_http_identity_bearer(), server_http_identity_principal(),
                         &mutation);
      if (mutation.mutated &&
          openai_ir_history_replace(request, cJSON_GetObjectItemCaseSensitive(container, "input")))
         goto refused;
   }
   if (cache && !request->n_tools && config_present() && config_dedup_enabled() &&
       request_context_idempotency_key()[0])
   {
      cJSON *canonical = aimee_ir_request_json(request);
      char *prepared = canonical ? cJSON_PrintUnformatted(canonical) : NULL;
      cJSON_Delete(canonical);
      cache->enabled = prepared
                           ? dedup_eligible(cache->key, sizeof cache->key, agent, cache->endpoint,
                                            cache->body, prepared, &cache->ttl)
                           : -1;
      free(prepared);
      if (cache->enabled < 0)
      {
         snprintf(error, capacity, "response composition unavailable");
         cJSON_Delete(container);
         gw_mutate_ctx_free(&mutation);
         return -1;
      }
      char *encoded = NULL;
      double avoided = 0;
      if (cache->enabled && response_dedup_get(cache->key, (long)time(NULL), &encoded, &avoided))
      {
         int decoded = openai_ir_cache_decode(encoded, out, cache);
         free(encoded);
         if (!decoded)
         {
            cJSON_Delete(container);
            gw_mutate_ctx_free(&mutation);
            if (aimee_ir_session_emit_buffered_response(out, NULL))
            {
               snprintf(error, capacity, "canonical cached response persistence failed");
               return -1;
            }
            cache->avoided = 1;
            if (agent_ingress_accounting_enabled())
               record_avoided_turn(agent->model, avoided);
            return 0;
         }
      }
   }
   char *reply = NULL;
   aimee_anthropic_replay_t replay = {0};
   int status =
       openai_post_ir(agent, request, wire, url, auth, headers, &reply, &replay, error, capacity);
   if (status != HTTP_RETRY_ADMISSION_REFUSED && container &&
       gw_buffered_after_status(container, "input", status, &mutation) == GW_POST_RESEND)
   {
      if (cache)
         cache->enabled = 0; /* The restored request has a different cache identity. */
      free(reply);
      reply = NULL;
      if (openai_ir_history_replace(request, cJSON_GetObjectItemCaseSensitive(container, "input")))
         goto refused;
      status = openai_post_ir(agent, request, wire, url, auth, headers, &reply, &replay, error,
                              capacity);
   }
   cJSON_Delete(container);
   gw_mutate_ctx_free(&mutation);
   if (status != 200 || !reply)
   {
      if (status != HTTP_RETRY_ADMISSION_REFUSED)
         openai_upstream_error_message(status, reply, error, capacity);
      free(reply);
      return -1;
   }
   int rc = aimee_backend_parse_wire(reply, strlen(reply), wire, out, error, capacity);
   free(reply);
   if (!rc && wire == AIMEE_WIRE_ANTHROPIC && aimee_anthropic_bind_response(&replay, out))
   {
      aimee_ir_session_fail_capture();
      rc = -1;
      snprintf(error, capacity, "cannot retain native replay binding");
   }
   if (rc || out->error_message || aimee_ir_session_emit_buffered_response(out, NULL))
   {
      if (!rc)
         snprintf(error, capacity, "%s",
                  out->error_message ? out->error_message
                                     : "canonical response persistence failed");
      return -1;
   }
   rc = agent_response_govern(out, openai_governance_enabled(), gateway_prevent_subagents_enabled(),
                              NULL, 0);
   if (rc >= 0 && !preserve_messages)
      rc = gw_ir_response_run_completion(out, request);
   if (rc < 0)
   {
      snprintf(error, capacity, "canonical response policy or persistence failed");
      return -1;
   }
   if (cache && cache->enabled && !aimee_ir_response_tool_count(out))
   {
      char *encoded = openai_ir_cache_encode(out, cache);
      if (encoded)
      {
         token_usage_t usage = {.input_tokens = out->usage_in,
                                .output_tokens = out->usage_out,
                                .cache_write_tokens = out->usage_cache_write,
                                .cache_read_tokens = out->usage_cache_read};
         response_dedup_put(cache->key, encoded,
                            token_estimate_cost(out->model ? out->model : agent->model, &usage),
                            (long)time(NULL), cache->ttl);
         free(encoded);
      }
   }
   return 0;
refused:
   cJSON_Delete(container);
   gw_mutate_ctx_free(&mutation);
   snprintf(error, capacity, "canonical economizer history is invalid");
   return -1;
}
static int agent_execute_ir_request(const agent_t *agent, aimee_request_t *request, int max_tokens,
                                    double temperature, aimee_response_t *out, char *error,
                                    size_t capacity, int preserve_messages)
{
   memset(out, 0, sizeof *out);
   if (!agent || !request ||
       openai_prepare_ir_request(request, agent, max_tokens, temperature, preserve_messages))
   {
      snprintf(error, capacity, "canonical provider request failed");
      return -1;
   }
   return agent_execute_prepared_ir_request(agent, request, out, error, capacity, preserve_messages,
                                            NULL);
}
/* SSE streaming for POST /v1/responses — full Codex parity. Converts the Codex
 * Responses request into the OpenAI chat shape (instructions -> system, the
 * message / function_call / function_call_output history -> chat messages,
 * `function` tools passed through), runs ONE provider step on the selected
 * primary agent (NO internal tool loop), and relays the result as Responses-API
 * events: either a message item (text) or function_call items that Codex
 * executes locally and feeds back as function_call_output next turn. The Codex
 * parser requires output_item.added before any delta, so every turn is framed
 * created -> item.added -> delta(s) -> item.done -> completed. Compute-then-chunk. */
/* Resolve the governance response toggle: config-store `modules.governance` (canonical) ->
 * deprecated env default. Cached legacy_config_read; keeps gw_stage_governance config-free. */
static int openai_governance_enabled(void)
{
   int tri = config_present() ? config_module_governance() : -1;
   return config_module_enabled(tri, gw_response_governance_enabled());
}

static int responses_stream_handler(const char *body, server_http_sse_event_emit emit,
                                    void *context)
{
   aimee_request_t request = {0};
   aimee_response_t response = {0};
   char error[512] = "invalid Responses request", id[64], frame[2048];
   long created = (long)time(NULL);
   const char *model = "";
   char *model_copy = NULL;
   if (responses_mint_id(created, id, sizeof id))
   {
      openai_format_error(frame, sizeof frame, "server_error",
                          "response identity could not be created");
      emit(context, "error", frame);
      return 0;
   }
   int admitted = responses_admit_request(body, &request, error, sizeof error) == 0;
   model = request.model ? request.model : "";
   if (openai_format_responses_created(id, model, created, frame, sizeof frame) > 0)
      emit(context, "response.created", frame);
   if (!admitted)
      goto failed;
   agent_t agent;
   if (stream_pick_agent(&agent, model))
   {
      snprintf(error, sizeof error, "the requested model is not available");
      goto failed;
   }
   model_copy = strdup(model[0] ? model : agent.model);
   if (!model_copy)
      goto failed;
   model = model_copy;
   if (agent_execute_ir_request(&agent, &request, 0, -1, &response, error, sizeof error, 0))
      goto failed;
   responses_account(&agent, model, &response);
   if (responses_store_history(id, &request, &response))
   {
      snprintf(error, sizeof error, "durable Responses history could not be committed");
      goto failed;
   }
   if (aimee_openai_emit_responses_tail(&response, id, model, created, emit, context))
   {
      snprintf(error, sizeof error, "canonical response rendering failed");
      goto failed;
   }
   goto done;
failed:
   if (openai_format_responses_failed(id, model, created,
                                      admitted ? "server_error" : "invalid_request_error", error,
                                      frame, sizeof frame) > 0)
      emit(context, "response.failed", frame);
done:
   free(model_copy);
   aimee_response_free(&response);
   aimee_request_free(&request);
   return 0;
}

/* Async inference owns canonical input and a selected registration. The live
 * run store is an output projection; model history remains in the session log. */
#define RUN_JSON_CAP (256 * 1024)
typedef struct
{
   char run_id[128], canonical_session[128];
   agent_t selected;
   aimee_request_t request;
   request_context_snapshot_t owner;
   long created;
   atomic_int cancel, finished;
} run_job_t;

static void run_job_free(run_job_t *job)
{
   if (!job)
      return;
   aimee_request_free(&job->request);
   request_context_snapshot_clear(&job->owner);
   OPENSSL_cleanse(job, sizeof *job);
   free(job);
}

static cJSON *run_object(const run_job_t *job, const char *status, const aimee_response_t *response,
                         const char *error)
{
   aimee_response_t empty = {0};
   const char *model = job->request.model ? job->request.model : job->selected.name;
   cJSON *object = aimee_openai_render_response(response ? response : &empty, job->run_id, model,
                                                job->created, 1);
   if (!object)
      return NULL;
   cJSON_ReplaceItemInObjectCaseSensitive(object, "object", cJSON_CreateString("run"));
   if (status)
      cJSON_ReplaceItemInObjectCaseSensitive(object, "status", cJSON_CreateString(status));
   if (job->canonical_session[0])
      cJSON_AddStringToObject(object, "canonical_session_id", job->canonical_session);
   if (error && error[0])
   {
      cJSON *detail = cJSON_AddObjectToObject(object, "error");
      cJSON_AddStringToObject(detail, "code", "server_error");
      cJSON_AddStringToObject(detail, "message", error);
   }
   return object;
}
static char *run_status_json(const run_job_t *job, const char *status, const char *error)
{
   cJSON *object = run_object(job, status, NULL, error);
   char *json = object ? cJSON_PrintUnformatted(object) : NULL;
   cJSON_Delete(object);
   return json;
}
static void run_finish_status(run_job_t *job, openai_run_status_t status, const char *error)
{
   char *json = run_status_json(job, openai_run_status_str(status), error);
   openai_runs_store_append_event(job->run_id,
                                  status == OPENAI_RUN_FAILED ? "error" : "response.completed",
                                  json ? json : "{}");
   openai_runs_store_finalize(job->run_id, status, json ? json : "{}");
   free(json);
}
static void runs_tool_event_cb(const char *phase, const char *name, void *context)
{
   run_job_t *job = context;
   char event[48];
   snprintf(event, sizeof event, "tool_call.%s", phase ? phase : "");
   cJSON *object = cJSON_CreateObject();
   cJSON_AddStringToObject(object, "type", event);
   cJSON_AddStringToObject(object, "name", name ? name : "");
   char *json = cJSON_PrintUnformatted(object);
   if (json)
      openai_runs_store_append_event(job->run_id, event, json);
   free(json);
   cJSON_Delete(object);
}
static void run_emit(void *context, const char *event, const char *json)
{
   run_job_t *job = context;
   openai_runs_store_append_event(job->run_id, event, json);
   /* Preserve the existing activity event at this explicit native output
    * boundary, now carrying the complete native message instead of flat text. */
   if (!strcmp(event, "response.output_item.done"))
   {
      cJSON *frame = cJSON_Parse(json);
      cJSON *item = cJSON_GetObjectItemCaseSensitive(frame, "item");
      const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "type"));
      if (kind && !strcmp(kind, "message"))
      {
         cJSON *completed = cJSON_CreateObject();
         cJSON_AddStringToObject(completed, "type", "message.completed");
         cJSON_AddItemToObject(completed, "message", cJSON_Duplicate(item, 1));
         char *wire = cJSON_PrintUnformatted(completed);
         if (wire)
            openai_runs_store_append_event(job->run_id, "message.completed", wire);
         free(wire);
         cJSON_Delete(completed);
      }
      cJSON_Delete(frame);
   }
}
static void *run_cancel_watch(void *context)
{
   run_job_t *job = context;
   while (!atomic_load(&job->finished))
   {
      openai_run_status_t status;
      if (openai_runs_store_cancel_requested(job->run_id) ||
          !openai_runs_store_status(job->run_id, &status))
         atomic_store(&job->cancel, 1);
      struct timespec pause = {0, 100000000L};
      nanosleep(&pause, NULL);
   }
   return NULL;
}
static void *run_job_worker(void *context)
{
   run_job_t *job = context;
   agent_result_t result = {0};
   aimee_response_t response = {0};
   pthread_t watcher;
   int watching = 0;
   aimee_ir_session_begin_task();
   request_context_restore(&job->owner);
   server_http_identity_bind_request_owner(request_context_get());
   agent_set_request_vault_principal(job->owner.value.vault_principal);
   agent_set_request_codex_creds(NULL, NULL);
   agent_set_request_cancel(&job->cancel);
   if (openai_runs_store_cancel_requested(job->run_id))
   {
      run_finish_status(job, OPENAI_RUN_CANCELLED, NULL);
      goto done;
   }
   char *started = run_status_json(job, "in_progress", NULL);
   if (!started)
   {
      run_finish_status(job, OPENAI_RUN_FAILED, "could not allocate run status");
      goto done;
   }
   openai_runs_store_set_status(job->run_id, OPENAI_RUN_IN_PROGRESS);
   openai_runs_store_update_json(job->run_id, started);
   openai_runs_store_append_event(job->run_id, "response.in_progress", started);
   free(started);

   char session[33];
   if (platform_random_hex(session, 32) ||
       db1_server_session_create(session, "openai-run", job->owner.value.vault_principal))
   {
      run_finish_status(job, OPENAI_RUN_FAILED, "run session owner unavailable");
      goto done;
   }
   session_id_set_override(session);
   agent_set_request_session(session);
   agent_config_t *config = calloc(1, sizeof *config);
   if (!config)
   {
      run_finish_status(job, OPENAI_RUN_FAILED, "could not allocate run configuration");
      goto done;
   }
   if (agent_load_config(config) || pthread_create(&watcher, NULL, run_cancel_watch, job))
   {
      free(config);
      run_finish_status(job, OPENAI_RUN_FAILED, "could not prepare run execution");
      goto done;
   }
   watching = 1;
   agent_tools_set_tool_event_cb(runs_tool_event_cb, job);
   agent_set_ingress_source(job->owner.value.source[0] ? job->owner.value.source
                                                       : "openai-ingress");
   int rc = agent_run_request_with_tools(config, &job->selected, &job->request, &response, &result);
   OPENSSL_cleanse(config, sizeof *config);
   free(config);
   snprintf(job->canonical_session, sizeof job->canonical_session, "%s",
            result.canonical_session_id);
   if (atomic_load(&job->cancel))
   {
      run_finish_status(job, OPENAI_RUN_CANCELLED, NULL);
      goto done;
   }
   if (rc || !result.success)
   {
      run_finish_status(job, OPENAI_RUN_FAILED, result.error[0] ? result.error : "run failed");
      goto done;
   }
   cJSON *object = run_object(job, NULL, &response, NULL);
   char *final = object ? cJSON_PrintUnformatted(object) : NULL;
   cJSON_Delete(object);
   if (!final || strlen(final) >= RUN_JSON_CAP)
   {
      free(final);
      run_finish_status(
          job, OPENAI_RUN_FAILED,
          "run output exceeds the live response boundary; canonical history retained");
      goto done;
   }
   if (aimee_openai_emit_responses_tail(
           &response, job->run_id, job->request.model ? job->request.model : job->selected.name,
           job->created, run_emit, job))
   {
      free(final);
      run_finish_status(job, OPENAI_RUN_FAILED, "canonical run output could not be rendered");
      goto done;
   }
   openai_runs_store_finalize(job->run_id,
                              response.stop_reason == AIMEE_STOP_MAX_TOKENS ? OPENAI_RUN_INCOMPLETE
                                                                            : OPENAI_RUN_COMPLETED,
                              final);
   free(final);
done:
   if (watching)
   {
      atomic_store(&job->finished, 1);
      pthread_join(watcher, NULL);
   }
   agent_tools_set_tool_event_cb(NULL, NULL);
   agent_set_ingress_source("");
   ingress_preinject_finish_sources();
   agent_set_request_cancel(NULL);
   agent_set_request_session(NULL);
   agent_set_request_codex_creds(NULL, NULL);
   agent_set_request_vault_principal(NULL);
   session_id_clear_override();
   server_http_identity_clear();
   request_context_clear();
   aimee_response_free(&response);
   free(result.response);
   run_job_free(job);
   return NULL;
}

static int runs_handler(const char *body, char *resp, int cap)
{
   run_job_t *job = calloc(1, sizeof *job);
   if (!job)
      return 500;
   char error[512] = "invalid canonical run request";
   request_context_snapshot(&job->owner);
   if (!job->owner.active || job->owner.value.vault_transport == ATTEST_NONE ||
       !job->owner.value.vault_principal[0])
   {
      run_job_free(job);
      openai_format_error(resp, cap, "permission_error", "authenticated run owner required");
      return 403;
   }
   if (responses_admit_request(body, &job->request, error, sizeof error))
      goto invalid;
   /* The run entry retains its published max_tokens alias in canonical form.
    * Conflicting spellings refuse instead of silently selecting a different cap. */
   long alias = -1;
   if (aimee_ir_read_usage(job->request.raw, "max_tokens", &alias) || alias == 0 ||
       alias > INT_MAX ||
       (alias >= 0 && job->request.has_max_tokens && alias != job->request.max_tokens))
   {
      snprintf(error, sizeof error, "invalid or conflicting run max_tokens");
      goto invalid;
   }
   if (alias > 0)
   {
      job->request.has_max_tokens = 1;
      job->request.max_tokens = (int)alias;
   }
   if (!agent_request_with_tools_supported(&job->request))
   {
      snprintf(error, sizeof error, "run request contains unsupported execution controls");
      goto invalid;
   }
   if (stream_pick_agent(&job->selected, job->request.model))
   {
      run_job_free(job);
      openai_format_error_code(resp, cap, "server_error", "no agent configured",
                               AIMEE_ERR_NO_PRIMARY);
      return 503;
   }
   if (is_chatgpt_provider(&job->selected) &&
       (job->request.has_max_tokens || job->request.has_temperature))
   {
      snprintf(error, sizeof error,
               "selected Responses backend does not support run output limits or temperature");
      goto invalid;
   }
   job->created = (long)time(NULL);
   static atomic_ulong sequence;
   snprintf(job->run_id, sizeof job->run_id, "run_%s_%ld_%lu", openai_runs_store_generation(),
            job->created, atomic_fetch_add(&sequence, 1) + 1);
   atomic_init(&job->cancel, 0);
   atomic_init(&job->finished, 0);
   char *queued = run_status_json(job, "queued", NULL);
   if (!queued || strlen(queued) >= (size_t)cap ||
       !openai_runs_store_create_owned(job->run_id, queued, job->owner.value.vault_principal))
   {
      free(queued);
      run_job_free(job);
      openai_format_error(resp, cap, "server_error", "could not create run");
      return 500;
   }
   snprintf(resp, (size_t)cap, "%s", queued);
   free(queued);
   pthread_t worker;
   if (pthread_create(&worker, NULL, run_job_worker, job))
   {
      run_finish_status(job, OPENAI_RUN_FAILED, "could not start run worker");
      run_job_free(job);
      openai_format_error(resp, cap, "server_error", "could not start run worker");
      return 500;
   }
   pthread_detach(worker);
   return 200;
invalid:
   run_job_free(job);
   openai_format_error(resp, cap, "invalid_request_error", error);
   return 400;
}

void openai_chat_register(void)
{
   server_http_set_chat_handler(chat_completions_handler);
   server_http_set_runs_handler(runs_handler);
   server_http_set_chat_stream_handler(chat_stream_handler);
   server_http_set_completion_stream_handler(completion_stream_handler);
   server_http_set_responses_stream_handler(responses_stream_handler);
   server_http_set_completion_handler(completions_handler);
   server_http_set_embeddings_handler(embeddings_handler);
   server_http_set_responses_handler(responses_handler);
   server_http_set_models_provider(models_provider);
   server_http_set_models_raw_provider(codex_models_raw);
}
