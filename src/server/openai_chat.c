/* openai_chat.c: inference-backed handlers for the OpenAI-compatible
 * /v1/chat/completions and /v1/completions routes.
 *
 * Registered into the server_http dispatch via server_http_set_*_handler at
 * server startup (openai_chat_register). Keeping the inference here — rather
 * than in server_http.c — keeps the agent/provider dependency closure out of
 * the server_http translation unit and its unit test (which injects a stub
 * handler instead).
 *
 * /v1/chat/completions supports SSE streaming via chat_stream_handler (the
 * server_http stream seam); /v1/completions and /v1/responses are still
 * non-streaming and reject "stream":true. Streaming computes the full
 * completion, then emits it as chat.completion.chunk deltas (provider-
 * incremental token streaming is a follow-up). The handler runs on the HTTP
 * listener thread, which serves one connection at a time, so a completion
 * blocks that listener for its duration — acceptable for the local
 * single-owner surface this first cut targets. */
#include "server_http.h"
#include "openai_shape.h"
#include "ingress_preinject.h"
#include <aimee/gateway/gateway_pipeline.h> /* gw_request_t + gw_pipeline_run_request — shared seam */
#include "modules/memory/gw_stage_memory.h"     /* gw_stage_memory + gw_memory_system_prompt (P3) */
#include "gw_stage_registry.h"                  /* Slice 7: config-driven stage catalog */
#include "gw_stage_governance.h"                /* response seam Slice 2: togglable governance */
#include "dogfood.h"                            /* dogfood_autolabel_next_turn_live */
#include "modules/learning/learning_implicit.h" /* learning_implicit_detect_turn */
#include "retrieval_outcome_bridge.h"           /* retrieval_outcome_bridge_on_autolabel */
#include "openai_responses_store.h"             /* previous_response_id continuation store */
#include "openai_runs_store.h"                  /* GET /v1/runs/{id} record store */
#include "aimee.h" /* EMBED_MAX_DIM, MAX_PATH_LEN (used by agent_types.h below) */
#include "log.h"   /* LOG_WARN: name the provider's error instead of discarding it */
#include "aimee_errors.h"
#include "config.h" /* config_t, config_load */
#include "agent_config.h"
#include "agent_exec.h"
#include "agent_protocol.h"                  /* parsed_response_t, message_history_repair */
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
#include "memory.h"                       /* memory_embed_text */
#include "request_context.h"
#include "response_dedup.h"
#include "token_tracker.h"
#include "token_audit.h" /* db1_token_audit_insert for the avoided-turn row */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OPENAI_CHAT_MAX_TOKENS  2048
#define OPENAI_CHAT_TEMPERATURE 0.7

/* Cap on the running /v1/responses transcript carried between turns via
 * previous_response_id. Bounds memory and the prompt we feed back to the agent. */
#define OPENAI_RESP_TRANSCRIPT_MAX (32 * 1024)

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
   snprintf(flags, sizeof(flags), "cs%d rc%d pi%d dw%d", config_cache_shaping_enabled(),
            config_reasoning_cap_enabled(), config_ingress_preinject_enabled(),
            config_dedup_window_seconds());
   response_dedup_key_inputs_t in = {
       .principal = request_context_principal(),
       .source = rc ? rc->source : "",
       .provider = ag ? ag->provider : "",
       .model = ag ? ag->model : "",
       .endpoint = endpoint,
       .stream = 0,
       .idempotency_key = idem,
       .body = body,
       .context = context,
       .behavior_flags = flags,
   };
   if (response_dedup_key(&in, key, key_cap) != 0)
      return -1;
   if (ttl_out)
      *ttl_out = config_dedup_window_seconds() > 0 ? config_dedup_window_seconds()
                                                   : RESPONSE_DEDUP_TTL_SECONDS;
   return 1;
}

/* Defined below, after the gateway pipeline they depend on; the chat tool-relay
 * branch in run_completion is their first caller. */
static int agent_execute_messages(const agent_t *agent, cJSON *messages, cJSON *tools,
                                  const char *system_prompt, int max_tokens, double temperature,
                                  parsed_response_t *out);
static int openai_governance_enabled(void);

/* Shared body for both endpoints. chat != 0 selects the chat.completion shape
 * (and chat-request parse); otherwise the legacy text_completion shape. */
static int run_completion(int chat, const char *body, char *resp, int cap)
{
   char model[64] = "";
   char *prompt = NULL;
   int stream = 0;
   int prc = chat ? openai_parse_chat_request(body, model, sizeof(model), &prompt, &stream)
                  : openai_parse_completion_request(body, model, sizeof(model), &prompt, &stream);
   if (prc != 0)
   {
      openai_format_error(resp, cap, "invalid_request_error",
                          chat ? "invalid chat completion request: expected messages[] with content"
                               : "invalid completion request: expected a non-empty prompt");
      return 400;
   }
   if (stream)
   {
      free(prompt);
      openai_format_error(resp, cap, "invalid_request_error",
                          "streaming responses are not yet supported on this endpoint");
      return 400;
   }

   /* Build the pre-injection envelope up front: it is part of the model input,
    * so the §4 dedup key must hash it (a stale reply must not be replayed after
    * the injected memory/context changes). On a cache miss it is reused for the
    * provider call below; on a hit it is freed unused. */
   char *pi_env = gw_memory_system_prompt(prompt);

   /* Resolve the backend BEFORE dedup so the key carries the resolved
    * provider/model — two requests resolving to a different backend must not
    * collide even with an identical body. Honour the requested model: "aimee"
    * (or empty) means the default agent; any other value selects a configured
    * agent by name, and is rejected with model_not_found when it matches none
    * (never silently falls back to a different model). */
   agent_t agbuf;
   agent_t *ag = NULL;
   if (model[0] && strcmp(model, "aimee") != 0)
   {
      /* An explicitly requested model must match a configured agent — never
       * silently fall back to the default (that would run a different model than
       * the client asked for and echo the wrong name back). */
      if (agent_registry_find(model, &agbuf) != 0)
      {
         free(pi_env);
         free(prompt);
         openai_format_error(resp, cap, "model_not_found", "the requested model is not available");
         return 404;
      }
      ag = &agbuf;
   }
   if (!ag && agent_registry_default_primary(&agbuf) == 0)
      ag = &agbuf;
   if (!ag)
   {
      free(pi_env);
      free(prompt);
      openai_format_error_code(resp, cap, "server_error", "no agent configured",
                               AIMEE_ERR_NO_PRIMARY);
      return 503;
   }

   /* Tool relay: when the caller supplies `tools`, this endpoint must return the
    * model's tool_calls for the CALLER to execute — the stateless half of the
    * OpenAI contract that every agentic client depends on. Without it a client
    * sees a provider that cannot call tools and refuses to run at all.
    *
    * This does NOT make the endpoint agentic: aimee still executes nothing and
    * holds no loop state (that remains /v1/runs). agent_execute_messages is the
    * same passthrough the /v1/responses ingress uses, so inbound tools go
    * through the shared tool-policing stage rather than around it.
    *
    * Dedup is skipped here: its key does not cover `tools`, so a cached
    * tool-free reply must never be replayed for a tool-bearing request. */
   if (chat)
   {
      char *instructions = NULL;
      cJSON *messages = NULL, *tools = NULL;
      if (openai_split_chat_request(body, &instructions, &messages, &tools) == 0 && tools)
      {
         parsed_response_t parsed;
         int trc = agent_execute_messages(
             ag, messages, tools, instructions ? instructions : pi_env,
             openai_request_int(body, "max_tokens", OPENAI_CHAT_MAX_TOKENS, 32768),
             openai_request_double(body, "temperature", OPENAI_CHAT_TEMPERATURE, 2.0), &parsed);
         free(instructions);
         cJSON_Delete(messages);
         cJSON_Delete(tools);
         free(pi_env);
         free(prompt);

         if (trc != 0)
         {
            agent_free_parsed_response(&parsed);
            openai_format_error(resp, cap, "upstream_error", "completion failed");
            return 502;
         }

         if (agent_ingress_accounting_enabled())
            agent_ingress_record_cost(ag->name, ag->model, model, parsed.stop_reason,
                                      parsed.prompt_tokens, parsed.completion_tokens,
                                      parsed.cache_write_tokens, parsed.cache_read_tokens,
                                      "openai-ingress", NULL);

         /* Same response-side policing the streaming path runs, so a denied
          * subagent-spawning call cannot reach an external caller. */
         if (parsed.is_tool_call && parsed.call_count > 0)
            (void)gw_response_run_governance(&parsed, openai_governance_enabled(),
                                             gateway_prevent_subagents_enabled());

         long tcreated = (long)time(NULL);
         char tid[48];
         snprintf(tid, sizeof(tid), "chatcmpl-%ld", tcreated);

         int tlen;
         if (parsed.is_tool_call && parsed.call_count > 0)
            tlen =
                openai_format_chat_completion_tool_calls(tid, model, &parsed, tcreated, resp, cap);
         else
            tlen = openai_format_chat_completion(tid, model, parsed.content ? parsed.content : "",
                                                 tcreated, parsed.prompt_tokens,
                                                 parsed.completion_tokens, resp, cap);
         agent_free_parsed_response(&parsed);
         if (tlen < 0)
         {
            openai_format_error(resp, cap, "server_error", "response did not fit the buffer");
            return 500;
         }
         return 200;
      }
      free(instructions);
      cJSON_Delete(messages);
      cJSON_Delete(tools);
   }

   /* §4 short-window dedup: serve a re-sent identical request (same account/source
    * boundary, RESOLVED provider+model, endpoint, stream mode, idempotency key,
    * body, pre-injected context, AND behaviour-config flags) from the TTL cache
    * without paying for the provider call again. Buffered + non-stream + tool-free
    * here. The key is built from the resolved agent above. */
   char dedup_key[288] = "";
   int dedup_ttl = RESPONSE_DEDUP_TTL_SECONDS;
   const char *endpoint = chat ? "/v1/chat/completions" : "/v1/completions";
   int dedup_on =
       dedup_eligible(dedup_key, sizeof(dedup_key), ag, endpoint, body, pi_env, &dedup_ttl);
   if (dedup_on < 0)
   {
      free(pi_env);
      free(prompt);
      openai_format_error(resp, cap, "server_error", "response composition unavailable");
      return 503;
   }
   if (dedup_on)
   {
      char *cached = NULL;
      double avoided = 0.0;
      if (response_dedup_get(dedup_key, (long)time(NULL), &cached, &avoided))
      {
         int n = snprintf(resp, cap, "%s", cached);
         free(cached);
         free(pi_env);
         free(prompt);
         if (n < 0 || n >= cap)
         {
            openai_format_error(resp, cap, "server_error",
                                "cached response did not fit the buffer");
            return 500;
         }
         if (agent_ingress_accounting_enabled())
            record_avoided_turn(model, avoided);
         return 200;
      }
   }

   /* Honour the OpenAI sampling params when present, else fall back to the
    * endpoint defaults. temperature clamps to [0, 2]; max_tokens to [1, hi]. */
   double temperature = openai_request_double(body, "temperature", OPENAI_CHAT_TEMPERATURE, 2.0);
   int max_tokens = openai_request_int(body, "max_tokens", OPENAI_CHAT_MAX_TOKENS, 32768);

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   /* pi_env (the <aimee-context> envelope) was built up front for the dedup key;
    * reuse it as the system prompt here. */
   int erc = agent_dispatch_one(ag, NULL, NULL, pi_env, prompt, max_tokens, temperature,
                                0 /* use_tools: plain chat completion */, &result);
   free(pi_env);
   free(prompt);

   if (erc != 0 || !result.response)
   {
      openai_format_error(resp, cap, "upstream_error",
                          result.error[0] ? result.error : "completion failed");
      free(result.response);
      return 502;
   }

   /* Cost accounting for the OpenAI-compatible ingress: this handler runs the
    * provider call directly (no agent_log_call), so record the turn's spend. */
   snprintf(result.requested_model, sizeof(result.requested_model), "%s", model);
   if (agent_ingress_accounting_enabled())
      agent_record_token_audit(&result, "", "openai-ingress");

   long created = (long)time(NULL);
   char id[48];
   snprintf(id, sizeof(id), "%s-%ld", chat ? "chatcmpl" : "cmpl", created);

   int len = chat ? openai_format_chat_completion(id, model, result.response, created,
                                                  result.prompt_tokens, result.completion_tokens,
                                                  resp, cap)
                  : openai_format_text_completion(id, model, result.response, created,
                                                  result.prompt_tokens, result.completion_tokens,
                                                  resp, cap);
   free(result.response);
   if (len < 0)
   {
      openai_format_error(resp, cap, "server_error", "response did not fit the buffer");
      return 500;
   }

   /* Cache this successful, tool-free 200 so an immediate identical retry under
    * the same idempotency key is served without a second provider call. The
    * stored cost is the realized spend, replayed as the avoided cost on a hit. */
   if (dedup_on)
   {
      token_usage_t usage = {.input_tokens = result.prompt_tokens,
                             .output_tokens = result.completion_tokens,
                             .cache_write_tokens = result.cache_write_tokens,
                             .cache_read_tokens = result.cache_read_tokens};
      const char *bill_model = result.model[0] ? result.model : model;
      double cost = token_estimate_cost(bill_model, &usage);
      response_dedup_put(dedup_key, resp, cost, (long)time(NULL), dedup_ttl);
   }
   return 200;
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
#define CODEX_DEFAULT_CONTEXT_WINDOW 272000
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
   codex_add_model(models, "aimee", "Aimee primary agent.", CODEX_DEFAULT_CONTEXT_WINDOW);
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
      codex_add_model(models, nm, acfg.agents[i].model, CODEX_DEFAULT_CONTEXT_WINDOW);
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
 * (memory_embed_text) and shape the OpenAI embeddings list. Returns 502 when
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
      int d = memory_embed_text(inputs[i], cmd, EMBED_INPUT_DOCUMENT, vecs[i], EMBED_MAX_DIM);
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
static atomic_ulong g_resp_seq = 0;
static void responses_mint_id(long created, char *buf, size_t n)
{
   snprintf(buf, n, "resp_%ld_%lu", created, atomic_fetch_add(&g_resp_seq, 1) + 1);
}

/* Persist the running transcript (full prompt + assistant reply) under id so a
 * follow-up carrying it as previous_response_id continues the conversation. */
static void responses_store_turn(const char *id, const char *full, const char *response)
{
   size_t need = strlen(full) + sizeof("\nassistant: ") + strlen(response) + 1;
   char *transcript = malloc(need);
   if (transcript)
   {
      snprintf(transcript, need, "%s\nassistant: %s", full, response);
      openai_responses_store_put(id, transcript);
      free(transcript);
   }
}

/* POST /v1/responses: the OpenAI Responses API (non-streaming). Runs inference
 * on the flattened `input` like chat/completions and shapes a `response`
 * object. When `previous_response_id` names a prior turn (held in the
 * in-process responses store), its accumulated transcript is prepended so the
 * model continues the conversation; the new turn (prior transcript + this input
 * + the assistant reply) is stored under the freshly minted response id so a
 * follow-up can chain off it in turn. The store is in-process only (not durable
 * across restarts), matching this surface's local single-owner posture. */
static int responses_handler(const char *body, char *resp, int cap)
{
   char model[64] = "";
   char prev_id[128] = "";
   char *prompt = NULL;
   int stream = 0;
   if (openai_parse_responses_request(body, model, sizeof(model), &prompt, prev_id, sizeof(prev_id),
                                      &stream) != 0)
   {
      openai_format_error(resp, cap, "invalid_request_error",
                          "invalid responses request: expected non-empty `input`");
      return 400;
   }
   if (stream)
   {
      free(prompt);
      openai_format_error(resp, cap, "invalid_request_error",
                          "streaming responses are not yet supported on this endpoint");
      return 400;
   }

   agent_t agbuf;
   agent_t *ag = NULL;
   if (model[0] && strcmp(model, "aimee") != 0)
   {
      /* Explicit model must match a configured agent — no silent fallback. */
      if (agent_registry_find(model, &agbuf) != 0)
      {
         free(prompt);
         openai_format_error(resp, cap, "model_not_found", "the requested model is not available");
         return 404;
      }
      ag = &agbuf;
   }
   if (!ag && agent_registry_default_primary(&agbuf) == 0)
      ag = &agbuf;
   if (!ag)
   {
      free(prompt);
      openai_format_error_code(resp, cap, "server_error", "no agent configured",
                               AIMEE_ERR_NO_PRIMARY);
      return 503;
   }

   double temperature = openai_request_double(body, "temperature", OPENAI_CHAT_TEMPERATURE, 2.0);
   int max_tokens = openai_request_int(body, "max_tokens", OPENAI_CHAT_MAX_TOKENS, 32768);

   /* Continuation: if previous_response_id names a stored conversation, prepend
    * its transcript so this turn continues it. `full` points at either the bare
    * input or the heap-allocated combined transcript (freed below). */
   char *full = prompt;
   char *combined = NULL;
   char prev[OPENAI_RESP_TRANSCRIPT_MAX];
   if (prev_id[0] && openai_responses_store_get(prev_id, prev, sizeof(prev)) && prev[0])
   {
      size_t need = strlen(prev) + 1 + strlen(prompt) + 1;
      combined = malloc(need);
      if (combined)
      {
         snprintf(combined, need, "%s\n%s", prev, prompt);
         full = combined;
      }
   }

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   /* P1 pre-injection: prepend the <aimee-context> envelope as the system
    * prompt (config ingress_preinject_enabled; no-op when off/empty). */
   char *pi_env = gw_memory_system_prompt(full);
   int erc = agent_dispatch_one(ag, NULL, NULL, pi_env, full, max_tokens, temperature,
                                0 /* use_tools: plain chat completion */, &result);
   free(pi_env);

   if (erc != 0 || !result.response)
   {
      openai_format_error(resp, cap, "upstream_error",
                          result.error[0] ? result.error : "response failed");
      free(result.response);
      free(prompt);
      free(combined);
      return 502;
   }

   /* Cost accounting: buffered /v1/responses runs the provider call directly. */
   snprintf(result.requested_model, sizeof(result.requested_model), "%s", model);
   if (agent_ingress_accounting_enabled())
      agent_record_token_audit(&result, "", "openai-ingress");

   long created = (long)time(NULL);
   char id[64];
   responses_mint_id(created, id, sizeof(id));
   responses_store_turn(id, full, result.response);
   free(prompt);
   free(combined);

   int len = openai_format_response(id, model, result.response, created, result.prompt_tokens,
                                    result.completion_tokens, result.cache_read_tokens, resp, cap);
   free(result.response);
   if (len < 0)
   {
      openai_format_error(resp, cap, "server_error", "response did not fit the buffer");
      return 500;
   }
   return 200;
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

/* Resolve the agent for `model` into *acfg (caller-owned, must outlive the
 * returned pointer — it indexes into acfg). Returns NULL when none configured. */
/* Fills `out` with the selected agent; returns 0 on success.
 *
 * Took an agent_config_t* and returned a pointer into it, which meant every
 * caller put 350,968 bytes on the stack of a request thread to obtain one
 * 16,720-byte agent. It now asks the registry for just that agent. */
static int stream_pick_agent(agent_t *out, const char *model)
{
   /* An explicitly requested model must match a configured agent. Failing
    * (rather than falling back to the default) makes callers refuse instead of
    * silently streaming a different model than the client asked for. */
   if (model[0] && strcmp(model, "aimee") != 0)
      return agent_registry_find(model, out);
   return agent_registry_default_primary(out);
}

static int chat_stream_handler(const char *body, server_http_sse_emit emit, void *ctx)
{
   char model[64] = "";
   char *prompt = NULL;
   int stream = 0;
   long created = (long)time(NULL);
   char id[48];
   snprintf(id, sizeof(id), "chatcmpl-%ld", created);

   if (openai_parse_chat_request(body, model, sizeof(model), &prompt, &stream) != 0)
   {
      emit_chunk(emit, ctx, id, model, created, 1, "invalid chat completion request", 0);
      emit_chunk(emit, ctx, id, model, created, 0, NULL, 1);
      free(prompt);
      return 0;
   }

   agent_t agbuf;
   agent_t *ag = stream_pick_agent(&agbuf, model) == 0 ? &agbuf : NULL;
   if (!ag)
   {
      emit_chunk(emit, ctx, id, model, created, 1, "no agent configured", 0);
      emit_chunk(emit, ctx, id, model, created, 0, NULL, 1);
      free(prompt);
      return 0;
   }

   double temperature = openai_request_double(body, "temperature", OPENAI_CHAT_TEMPERATURE, 2.0);
   int max_tokens = openai_request_int(body, "max_tokens", OPENAI_CHAT_MAX_TOKENS, 32768);

   /* Tool relay, streaming half. Agentic clients stream by default, so without
    * this the buffered relay in run_completion would never be reached by them.
    * Same statelessness contract as that branch. This handler already computes
    * the whole reply before chunking it, so the calls ship in a single delta
    * followed by a tool_calls finish frame. */
   {
      char *instructions = NULL;
      cJSON *messages = NULL, *tools = NULL;
      if (openai_split_chat_request(body, &instructions, &messages, &tools) == 0 && tools)
      {
         parsed_response_t parsed;
         int trc = agent_execute_messages(ag, messages, tools, instructions, max_tokens,
                                          temperature, &parsed);
         free(instructions);
         cJSON_Delete(messages);
         cJSON_Delete(tools);
         free(prompt);

         emit_chunk(emit, ctx, id, model, created, 1, NULL, 0); /* role frame */
         if (trc != 0)
         {
            emit_chunk(emit, ctx, id, model, created, 0, "completion failed", 0);
            emit_chunk(emit, ctx, id, model, created, 0, NULL, 1);
            agent_free_parsed_response(&parsed);
            return 0;
         }

         if (agent_ingress_accounting_enabled())
            agent_ingress_record_cost(ag->name, ag->model, model, parsed.stop_reason,
                                      parsed.prompt_tokens, parsed.completion_tokens,
                                      parsed.cache_write_tokens, parsed.cache_read_tokens,
                                      "openai-ingress", NULL);

         if (parsed.is_tool_call && parsed.call_count > 0)
            (void)gw_response_run_governance(&parsed, openai_governance_enabled(),
                                             gateway_prevent_subagents_enabled());

         if (parsed.is_tool_call && parsed.call_count > 0)
         {
            char frame[4096];
            if (parsed.content && parsed.content[0])
               emit_chunk(emit, ctx, id, model, created, 0, parsed.content, 0);
            if (openai_format_chat_chunk_tool_calls(id, model, created, &parsed, frame,
                                                    sizeof(frame)) > 0)
               emit(ctx, frame);
            if (openai_format_chat_chunk_finish(id, model, created, "tool_calls", frame,
                                                sizeof(frame)) > 0)
               emit(ctx, frame);
         }
         else
         {
            /* Policing may have dropped every call; fall back to a text turn. */
            emit_chunk(emit, ctx, id, model, created, 0, parsed.content ? parsed.content : "", 0);
            emit_chunk(emit, ctx, id, model, created, 0, NULL, 1);
         }
         agent_free_parsed_response(&parsed);
         return 0;
      }
      free(instructions);
      cJSON_Delete(messages);
      cJSON_Delete(tools);
   }

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   /* P1 pre-injection: prepend the <aimee-context> envelope as the system
    * prompt (config ingress_preinject_enabled; no-op when off/empty). */
   char *pi_env = gw_memory_system_prompt(prompt);
   int erc = agent_dispatch_one(ag, NULL, NULL, pi_env, prompt, max_tokens, temperature,
                                0 /* use_tools: plain chat completion */, &result);
   free(pi_env);
   free(prompt);

   emit_chunk(emit, ctx, id, model, created, 1, NULL, 0); /* role frame */
   if (erc != 0 || !result.response)
   {
      emit_chunk(emit, ctx, id, model, created, 0,
                 result.error[0] ? result.error : "completion failed", 0);
   }
   else
   {
      /* Cost accounting: streaming chat/completions runs the provider call
       * directly (no agent_log_call). */
      snprintf(result.requested_model, sizeof(result.requested_model), "%s", model);
      if (agent_ingress_accounting_enabled())
         agent_record_token_audit(&result, "", "openai-ingress");
      const char *txt = result.response;
      size_t n = strlen(txt);
      for (size_t off = 0; off < n; off += OPENAI_STREAM_CHUNK)
      {
         char seg[OPENAI_STREAM_CHUNK + 1];
         size_t take = (n - off < OPENAI_STREAM_CHUNK) ? (n - off) : OPENAI_STREAM_CHUNK;
         memcpy(seg, txt + off, take);
         seg[take] = '\0';
         emit_chunk(emit, ctx, id, model, created, 0, seg, 0);
      }
   }
   emit_chunk(emit, ctx, id, model, created, 0, NULL, 1); /* finish frame */
   free(result.response);
   return 0;
}

/* SSE streaming for POST /v1/completions (legacy text_completion). Same
 * compute-then-chunk shape as chat, emitting text_completion chunk frames. */
static int completion_stream_handler(const char *body, server_http_sse_emit emit, void *ctx)
{
   char model[64] = "";
   char *prompt = NULL;
   int stream = 0;
   long created = (long)time(NULL);
   char id[48];
   snprintf(id, sizeof(id), "cmpl-%ld", created);

   if (openai_parse_completion_request(body, model, sizeof(model), &prompt, &stream) != 0)
   {
      emit_text_chunk(emit, ctx, id, model, created, "invalid completion request", 0);
      emit_text_chunk(emit, ctx, id, model, created, "", 1);
      free(prompt);
      return 0;
   }

   agent_t agbuf;
   agent_t *ag = stream_pick_agent(&agbuf, model) == 0 ? &agbuf : NULL;
   if (!ag)
   {
      emit_text_chunk(emit, ctx, id, model, created, "no agent configured", 0);
      emit_text_chunk(emit, ctx, id, model, created, "", 1);
      free(prompt);
      return 0;
   }

   double temperature = openai_request_double(body, "temperature", OPENAI_CHAT_TEMPERATURE, 2.0);
   int max_tokens = openai_request_int(body, "max_tokens", OPENAI_CHAT_MAX_TOKENS, 32768);

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   /* P1 pre-injection: prepend the <aimee-context> envelope as the system
    * prompt (config ingress_preinject_enabled; no-op when off/empty). */
   char *pi_env = gw_memory_system_prompt(prompt);
   int erc = agent_dispatch_one(ag, NULL, NULL, pi_env, prompt, max_tokens, temperature,
                                0 /* use_tools: plain chat completion */, &result);
   free(pi_env);
   free(prompt);

   if (erc != 0 || !result.response)
   {
      emit_text_chunk(emit, ctx, id, model, created,
                      result.error[0] ? result.error : "completion failed", 0);
   }
   else
   {
      /* Cost accounting: streaming completions runs the provider call directly. */
      snprintf(result.requested_model, sizeof(result.requested_model), "%s", model);
      if (agent_ingress_accounting_enabled())
         agent_record_token_audit(&result, "", "openai-ingress");
      const char *txt = result.response;
      size_t n = strlen(txt);
      for (size_t off = 0; off < n; off += OPENAI_STREAM_CHUNK)
      {
         char seg[OPENAI_STREAM_CHUNK + 1];
         size_t take = (n - off < OPENAI_STREAM_CHUNK) ? (n - off) : OPENAI_STREAM_CHUNK;
         memcpy(seg, txt + off, take);
         seg[take] = '\0';
         emit_text_chunk(emit, ctx, id, model, created, seg, 0);
      }
   }
   emit_text_chunk(emit, ctx, id, model, created, "", 1); /* finish frame */
   free(result.response);
   return 0;
}

/* ---- OpenAI-ingress gateway request pipeline (universal-gateway) -------------
 * The /v1/responses ingress runs the same ordered [memory, tool_policing] stages
 * over a gw_request_t as the Anthropic ingress (anthropic_http.c), so both APIs
 * aimee presents share the one audited pipeline seam. The OpenAI request reaches
 * the provider step DECOMPOSED (an instructions string + a tools array) rather
 * than as a single inbound cJSON, so `raw` is a small IR object assembled from
 * those two mutable fields; the stages mutate it and the caller reads the results
 * back. Behavior is identical to the prior inline memory block + the P2b tool
 * strip it replaces. */

/* Memory/context pre-injection is now the shared gw_stage_memory (gw_stage_memory.c)
 * with mem_target = GW_MEM_OPENAI_INSTRUCTIONS: the /v1/responses proxy always
 * translates, so memory always applies (gated only by config, inside the stage),
 * merging the envelope into raw.instructions. `ud` stays the chat-shape `messages`
 * the recall query is derived from, so recall is byte-unchanged. */

/* Tool-policing stage (OpenAI tool shape): strip subagent-spawning tools from
 * raw.tools. Mirrors the Anthropic ingress's policing stage; same contract as
 * gateway_policy_apply_request (returns the number of tools stripped). */
static int gw_stage_openai_tool_policing(gw_request_t *r, void *ud)
{
   (void)ud;
   return gateway_policy_apply_request(r->raw, 1 /* OpenAI tool shape */);
}

/* Single provider step for the Codex proxy: build the request for `agent`'s
 * provider from a caller-supplied messages array and passthrough `tools`, POST
 * it, and parse the reply into *out. Unlike agent_run_with_tools this does NOT
 * execute tool calls — out->calls[] surfaces them so the handler can relay them
 * to Codex. Returns 0 on success, -1 on transport/parse failure. *out is zeroed
 * here; the caller frees it with agent_free_parsed_response(). */
/* Build the provider request body from the effective messages/tools/system (IR path
 * when enabled, else the legacy driver builder). Re-callable so the gateway-mutation
 * 4xx restore path can rebuild the body from the restored (pristine) messages.
 * Returns a malloc'd JSON string (caller frees) or NULL. */
static char *openai_build_body(const agent_t *agent, const delegate_driver_t *driver,
                               cJSON *eff_messages, cJSON *eff_tools, const char *eff_system,
                               int tok, double temperature)
{
   cJSON *req = NULL;
   if (aimee_ir_path_enabled())
      req =
          aimee_ir_build_from_chat(agent->model, eff_messages, eff_tools, eff_system, driver->name);
   if (!req)
      req = driver->build_request(agent, eff_messages, eff_tools, eff_system, tok, temperature);
   if (!req)
      return NULL;
   char *body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   return body;
}

/* Drop tools the provider will reject, and say which.
 *
 * A single tool with an empty name invalidates the WHOLE request: the ChatGPT
 * Responses backend answers 400 `Invalid 'tools[16].name': empty string`, so the
 * turn dies and every other tool in the catalog dies with it. Measured with a real
 * Codex client, which sends 17 tools; the legacy responses->chat translator filters
 * non-function and nameless tools, and the IR path that runs ahead of it does not,
 * so which translator handled the request decided whether the turn worked.
 *
 * Dropping one unusable entry is strictly better than failing the turn: a tool the
 * provider will not accept cannot be called either way. Logged per tool so a
 * silently-missing capability is diagnosable rather than mysterious. */
static int strip_unusable_tools(cJSON *tools)
{
   if (!cJSON_IsArray(tools))
      return 0;
   int dropped = 0;
   for (int i = cJSON_GetArraySize(tools) - 1; i >= 0; i--)
   {
      cJSON *t = cJSON_GetArrayItem(tools, i);
      const cJSON *name = cJSON_GetObjectItemCaseSensitive(t, "name");
      if (!cJSON_IsString(name) || !name->valuestring[0])
      {
         const cJSON *fn = cJSON_GetObjectItemCaseSensitive(t, "function");
         name = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
      }
      if (cJSON_IsString(name) && name->valuestring[0])
         continue;
      const cJSON *type = cJSON_GetObjectItemCaseSensitive(t, "type");
      LOG_WARN("openai.responses", "dropping tool[%d] with no usable name (type=%s)", i,
               cJSON_IsString(type) ? type->valuestring : "?");
      cJSON_DeleteItemFromArray(tools, i);
      dropped++;
   }
   return dropped;
}

static int agent_execute_messages(const agent_t *agent, cJSON *messages, cJSON *tools,
                                  const char *system_prompt, int max_tokens, double temperature,
                                  parsed_response_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   if (!agent || !messages)
      return -1;
   strip_unusable_tools(tools);

   /* Every bare `return -1` below reported the same generic failure to the client.
    * Each now says which precondition failed: the difference between "no driver for
    * this provider" and "this agent has no usable token" is the whole diagnosis, and
    * the client message ("upstream model request failed") actively misdirects,
    * because on three of these paths nothing upstream was ever contacted. */
   delegate_drivers_init();
   const delegate_driver_t *driver = delegate_driver_get(agent->provider);
   if (!driver || !driver->build_request || !driver->parse_response)
   {
      LOG_WARN("openai.responses", "no usable driver for provider=%s (agent=%s)",
               agent->provider ? agent->provider : "?", agent->name ? agent->name : "?");
      return -1;
   }

   char url[MAX_ENDPOINT_LEN + 64];
   if (delegate_build_url(driver, agent, url, sizeof(url)) != 0)
   {
      LOG_WARN("openai.responses", "build_url failed: driver=%s endpoint=%s", driver->name,
               agent->endpoint ? agent->endpoint : "?");
      return -1;
   }

   char auth_header[MAX_API_KEY_LEN + 32];
   if (agent_resolve_auth(agent, auth_header, sizeof(auth_header)) != 0)
   {
      LOG_WARN("openai.responses", "auth unresolved: agent=%s auth_type=%s",
               agent->name ? agent->name : "?", agent->auth_type ? agent->auth_type : "?");
      return -1;
   }
   char extra_headers[512];
   agent_build_extra_headers(agent, extra_headers, sizeof(extra_headers));

   /* Gateway request pipeline: run [memory, tool_policing] over an IR assembled
    * from this /v1/responses request's two mutable fields (the decomposed
    * instructions string + tools array). `tools` is added by REFERENCE so the
    * policy stage strips it in place while the caller keeps ownership. The memory
    * stage derives its query from `messages` (per-call stage ud). After the run:
    * eff_system is the (maybe envelope-merged) instructions; eff_tools is the
    * original array unless the policy emptied + dropped it (then omit — never
    * forward `tools: []`). */
   cJSON *gw_raw = cJSON_CreateObject();
   cJSON_AddStringToObject(gw_raw, "instructions", system_prompt ? system_prompt : "");
   if (cJSON_IsArray(tools))
      cJSON_AddItemReferenceToObject(gw_raw, "tools", tools);
   gw_request_t gr = {
       .raw = gw_raw,
       .driver = driver,
       .ag = agent,
       .serving_api = GW_API_OPENAI,
       .mem_target = GW_MEM_OPENAI_INSTRUCTIONS,
       .parity = 1, /* client OpenAI == serving OpenAI; informational for these stages */
       .stream = 0,
   };
   /* Memory PORTED to the IR transform seam (aimee_ir_apply_request_stages): it now
    * fires once on the typed IR inside openai_build_body's IR path, so it is no longer
    * a pre-IR wire-anchored stage here. This catalog keeps the stages that still act on
    * the raw /v1/responses request (tool policing, routing). */
   const gw_stage_slot_t slots[] = {
       {"tool_policing", gw_stage_openai_tool_policing, NULL, 1},
       {"router", gw_stage_router, NULL, 1}, /* S1/S2: unified request->workflow seam */
   };
   gw_stage_t stages[6];
   int nstages = gw_stage_registry_build(slots, sizeof(slots) / sizeof(slots[0]), stages, 6);
   if (nstages < 0)
   {
      /* Misconfigured stage catalog: fail closed (like the anthropic ingress) rather
       * than run a partial/empty pipeline that skips tool policing + routing. */
      LOG_WARN("openai.responses", "stage catalog build failed; refusing a partial pipeline");
      cJSON_Delete(gw_raw);
      return -1;
   }
   gw_pipeline_run_request(&gr, stages, (size_t)nstages);

   cJSON *rawi = cJSON_GetObjectItemCaseSensitive(gw_raw, "instructions");
   const char *eff_system = rawi && rawi->valuestring ? rawi->valuestring : system_prompt;
   cJSON *eff_tools = cJSON_GetObjectItemCaseSensitive(gw_raw, "tools") ? tools : NULL;

   int tok = agent_request_max_tokens(agent, max_tokens);

   /* AGGRESSIVE may apply the existing lossy reducer to request history. SAFE
    * never enters this path. Anthropic-backed routes are no longer excluded: the
    * gateway's per-session freeze keeps a folded prefix byte-identical, which is
    * what their cache prefix actually requires. */
   gw_mutate_ctx_t gwmc;
   gw_mutate_ctx_init(&gwmc);
   cJSON *mbox = NULL;
   cJSON *econ_messages = messages;
   int upstream_is_anthropic = driver && driver->name && strcmp(driver->name, "anthropic") == 0;
   if (gw_mutate_upstream_ok(upstream_is_anthropic))
   {
      mbox = cJSON_CreateObject();
      if (mbox)
      {
         cJSON_AddItemReferenceToObject(mbox, "input", messages);
         gw_buffered_mutate(mbox, "input", agent->model, eff_system,
                            server_http_identity_session_hdr(), server_http_identity_bearer(),
                            server_http_identity_principal(), &gwmc);
         cJSON *reduced = cJSON_GetObjectItemCaseSensitive(mbox, "input");
         if (reduced)
            econ_messages = reduced;
      }
   }

   char *body =
       openai_build_body(agent, driver, econ_messages, eff_tools, eff_system, tok, temperature);
   if (!body)
   {
      LOG_WARN("openai.responses", "openai_build_body returned nothing: driver=%s model=%s",
               driver->name, agent->model ? agent->model : "?");
      cJSON_Delete(mbox);
      gw_mutate_ctx_free(&gwmc);
      cJSON_Delete(gw_raw);
      return -1;
   }

   int economizer_active = econ_mode_current() != ECON_MODE_OFF;
   int wire_anthropic = driver && driver->name && strcmp(driver->name, "anthropic") == 0;
   wire_fence_route_t wire_route = wire_anthropic              ? WIRE_FENCE_ANTHROPIC_MESSAGES
                                   : strstr(url, "/responses") ? WIRE_FENCE_OPENAI_RESPONSES
                                                               : WIRE_FENCE_OPENAI_CHAT;
   wire_fence_t *wire_snapshot = NULL;
   wire_fence_bytes_t wire_body;
   if (wire_fence_select(economizer_active, wire_route, body, strlen(body), &wire_snapshot,
                         &wire_body) != 0)
   {
      free(body);
      cJSON_Delete(mbox);
      gw_mutate_ctx_free(&gwmc);
      cJSON_Delete(gw_raw);
      return -1;
   }

   char *response_body = NULL;
   int ra = config_retry_max_attempts() > 0 ? config_retry_max_attempts() : HTTP_RETRY_MAX_ATTEMPTS;
   int rb = config_retry_base_ms() > 0 ? config_retry_base_ms() : HTTP_RETRY_BASE_MS;
   int rm = config_retry_max_ms() > 0 ? config_retry_max_ms() : HTTP_RETRY_MAX_MS;
   int http_status = http_retry_post_context_bytes(url, auth_header, wire_body.data, wire_body.len,
                                                   &response_body, agent->timeout_ms, extra_headers,
                                                   ra, rb, rm, agent->provider, agent->model, NULL);
   free(body);
   wire_fence_destroy(wire_snapshot);

   cJSON_Delete(mbox);
   gw_mutate_ctx_free(&gwmc);
   cJSON_Delete(gw_raw);

   if (http_status != 200 || !response_body)
   {
      /* Say WHAT the provider said. This path reports a single generic
       * "upstream model request failed" to the client, which is all a Codex user
       * ever sees -- and the buffered /v1/responses handler on the SAME agent and
       * the SAME url succeeds, so "the provider is down" is not the explanation
       * and the difference is in this request. Discarding the body here meant the
       * only way to find out was to read the source and guess.
       *
       * Truncated because a provider error can carry an HTML error page. */
      LOG_WARN("openai.responses",
               "streaming upstream failed: status=%d provider=%s model=%s url=%s body=%.400s",
               http_status, agent->provider ? agent->provider : "?",
               agent->model ? agent->model : "?", url, response_body ? response_body : "(none)");
      free(response_body);
      return -1;
   }

   cJSON *root = cJSON_Parse(response_body);
   driver->parse_response(root, response_body, out);
   if (root)
      cJSON_Delete(root);
   free(response_body);
   return 0;
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
 * deprecated env default. Cached config_load; keeps gw_stage_governance config-free. */
static int openai_governance_enabled(void)
{
   int tri = config_present() ? config_module_governance() : -1;
   return config_module_enabled(tri, gw_response_governance_enabled());
}

static int responses_stream_handler(const char *body, server_http_sse_event_emit emit, void *ctx)
{
   char model[64] = "";
   char *instructions = NULL;
   cJSON *messages = NULL;
   cJSON *tools = NULL;
   int stream = 0;
   long created = (long)time(NULL);
   char id[64];
   responses_mint_id(created, id, sizeof(id));
   char frame[2048];

   /* Route the Responses CLIENT parse through the IR (no direct Responses->chat
    * translation) when the flag is on; fall back to the legacy translator on any
    * failure or when off. Both feed the same downstream agent path. */
   int rp_ok = 0;
   if (aimee_ir_path_enabled())
      rp_ok = (aimee_ir_responses_to_chat(body, model, sizeof(model), &instructions, &messages,
                                          &tools, &stream) == 0);
   if (!rp_ok)
      rp_ok = (openai_parse_responses_to_chat(body, model, sizeof(model), &instructions, &messages,
                                              &tools, &stream) == 0);
   if (!rp_ok)
   {
      if (openai_format_responses_created(id, model, created, frame, sizeof(frame)) > 0)
         emit(ctx, "response.created", frame);
      if (openai_format_responses_completed(id, model, "invalid responses request", created, 0, 0,
                                            0, frame, sizeof(frame)) > 0)
         emit(ctx, "response.completed", frame);
      free(instructions);
      cJSON_Delete(messages);
      cJSON_Delete(tools);
      return 0;
   }
   (void)stream; /* this handler is the streaming path; flag is informational */

   agent_t agbuf;
   agent_t *ag = stream_pick_agent(&agbuf, model) == 0 ? &agbuf : NULL;
   if (!ag)
   {
      if (openai_format_responses_created(id, model, created, frame, sizeof(frame)) > 0)
         emit(ctx, "response.created", frame);
      if (openai_format_responses_completed(id, model, "no agent configured", created, 0, 0, 0,
                                            frame, sizeof(frame)) > 0)
         emit(ctx, "response.completed", frame);
      free(instructions);
      cJSON_Delete(messages);
      cJSON_Delete(tools);
      return 0;
   }

   double temperature = openai_request_double(body, "temperature", OPENAI_CHAT_TEMPERATURE, 2.0);
   int max_tokens = openai_request_int(body, "max_tokens", OPENAI_CHAT_MAX_TOKENS, 32768);

   /* Heal orphaned tool calls/results — each Codex turn is stateless full-history. */
   message_history_repair(messages);

   /* Per-turn learning signals (primary external-agent turn — this is the
    * converged chat-completions path that Anthropic /v1/messages and Codex
    * /v1/responses both translate into; delegates use a different path). On this
    * turn's user text we (1) autolabel the PRIOR turn's memory-citation moment
    * and (2) emit the implicit citation_then_{repair,continuation} learning
    * signal. Both are self-gating: they no-op unless a prior moment was logged
    * (g_last_record_id) and the relevant flags are on. Runs BEFORE pre-injection
    * recall below, which would overwrite g_last_record_id with this turn's moment. */
   {
      char *turn_text = ingress_preinject_query_from_messages(messages);
      if (turn_text)
      {
         dogfood_autolabel_next_turn_live(turn_text);
         learning_implicit_detect_turn(turn_text);
         /* Bridge the same continuation/repair classification into a retrieval
          * OUTCOME for the prior turn's surfaced rows (default-off). Closes the
          * demotion + learning-to-rank loops from the signal already computed.
          * The prior assistant answer (from the message history) drives per-doc
          * overlap attribution — which surfaced rows the answer actually used. */
         {
            dogfood_autolabel_kind_t rob_kind = dogfood_classify_next_turn(turn_text);
            char *prior_answer = ingress_preinject_last_assistant_from_messages(messages);
            retrieval_outcome_bridge_on_autolabel(prior_answer,
                                                  rob_kind == DOGFOOD_AUTOLABEL_CONTINUATION,
                                                  rob_kind == DOGFOOD_AUTOLABEL_REPAIR);
            free(prior_answer);
         }
         free(turn_text);
      }
   }

   /* P1 context pre-injection now runs as the `memory` stage of the gateway
    * request pipeline inside agent_execute_messages (below), alongside tool
    * policing — so `instructions` is passed through un-merged and the pipeline
    * folds in the <aimee-context> envelope. */

   /* Economizer gateway-mutation NOTE (regression guard): this streaming handler
    * BUFFERS the upstream reply via agent_execute_messages (the same buffered path the
    * non-streaming /v1/responses handler uses) and replays it as SSE. So the gateway
    * mutation + its 4xx-restore-resend / 5xx-disable already apply here via S6 — there
    * is NO true token-by-token upstream stream for the OpenAI primary path (hence no
    * S5-style streaming disable-only code). If a future refactor introduces a real
    * agent_http_post_stream path here, it must add the streaming mutation contract. */
   parsed_response_t parsed;
   int erc =
       agent_execute_messages(ag, messages, tools, instructions, max_tokens, temperature, &parsed);

   /* `response.created` is the per-response envelope event: emitted exactly
    * once here for EVERY downstream path (error -> response.failed, text turn,
    * and the tool-call branch). openai_responses_emit_policed() deliberately
    * does NOT re-emit it, so the tool-call path never doubles it on the wire.
    * `frame` is 2048 bytes and this event holds only the minted id (~30 chars)
    * + model alias + a fixed JSON envelope, so it cannot truncate in practice;
    * the `> 0` check is belt-and-suspenders, not a real partial-stream path. */
   if (openai_format_responses_created(id, model, created, frame, sizeof(frame)) > 0)
      emit(ctx, "response.created", frame);

   if (erc != 0)
   {
      /* Provider/transport failure: surface a real terminal error event the way
       * the OpenAI backend would, so Codex applies its typed-error handling
       * (retry/backoff) instead of treating a failed turn as an empty success.
       * Unknown `code` maps to ApiError::Retryable in Codex's parser. */
      if (openai_format_responses_failed(id, model, created, "server_error",
                                         "upstream model request failed", frame, sizeof(frame)) > 0)
         emit(ctx, "response.failed", frame);
      agent_free_parsed_response(&parsed);
      free(instructions);
      cJSON_Delete(messages);
      cJSON_Delete(tools);
      return 0;
   }

   if (erc == 0)
   {
      /* Cost accounting: streaming /v1/responses runs the provider call directly.
       * agent_execute_messages returns a parsed_response_t (no model/agent name),
       * so synthesize the fields the audit needs from the served agent. */
      if (agent_ingress_accounting_enabled())
         agent_ingress_record_cost(ag->name, ag->model, model, parsed.stop_reason,
                                   parsed.prompt_tokens, parsed.completion_tokens,
                                   parsed.cache_write_tokens, parsed.cache_read_tokens,
                                   "openai-ingress", NULL);
   }

   if (erc == 0 && parsed.is_tool_call && parsed.call_count > 0)
   {
      /* P2c streaming: when gateway_prevent_subagents is ON, run the police
       * event-bus governance module first (drops denied subagent tool_call
       * entries in place; preserves upstream stop_reason in partial-drop,
       * rewrites to "end_turn" on all-dropped). The module receives the
       * already-resolved policy gate; it remains the sole decision path.
       * Police contract is finalized in PR #677 (buffered) and PR #679
       * (streaming Anthropic). */
      int police_drops = 0;
      police_drops = gw_response_run_governance(&parsed, openai_governance_enabled(),
                                                gateway_prevent_subagents_enabled());
      (void)police_drops; /* drop count plumbed through the pipeline total
                             for the future P2b audit pass; today the only
                             visible effect is the parsed.calls[] mutation. */

      /* Name every call handed back to the client. A Codex client sends its MCP
       * servers as `namespace` groups and can only route a call whose name is
       * namespace-qualified; a bare nested name comes back as
       * "unsupported call: <name>" and the turn is wasted. Logging the names is the
       * only way to tell "the provider emitted it bare" from "we stripped it". */
      for (int ci = 0; ci < parsed.call_count; ci++)
         LOG_INFO("openai.responses", "returning tool call name=%s",
                  parsed.calls[ci].name[0] ? parsed.calls[ci].name : "(empty)");
      openai_responses_emit_policed(&parsed, id, model, created, emit, ctx);
   }
   else

   {
      /* Text turn: a single assistant message item carrying the content. */
      const char *txt =
          (erc == 0 && parsed.content) ? parsed.content : "the model returned no content";
      char item_id[72];
      snprintf(item_id, sizeof(item_id), "%s-msg", id);

      if (openai_format_responses_msg_item_added(item_id, 0, frame, sizeof(frame)) > 0)
         emit(ctx, "response.output_item.added", frame);

      size_t n = strlen(txt);
      for (size_t off = 0; off < n; off += OPENAI_STREAM_CHUNK)
      {
         char seg[OPENAI_STREAM_CHUNK + 1];
         size_t take = (n - off < OPENAI_STREAM_CHUNK) ? (n - off) : OPENAI_STREAM_CHUNK;
         memcpy(seg, txt + off, take);
         seg[take] = '\0';
         char dframe[OPENAI_STREAM_CHUNK * 6 + 256];
         if (openai_format_responses_delta(item_id, seg, dframe, sizeof(dframe)) > 0)
            emit(ctx, "response.output_text.delta", dframe);
      }

      size_t icap = n * 6 + 512;
      char *itf = malloc(icap);
      if (itf)
      {
         if (openai_format_responses_msg_item_done(item_id, txt, 0, itf, (int)icap) > 0)
            emit(ctx, "response.output_item.done", itf);
         free(itf);
      }

      /* OpenAI finish_reason "length" (carried into stop_reason) means the model
       * was truncated at the token cap. The backend reports that as a terminal
       * response.incomplete{reason:max_output_tokens}, not response.completed;
       * mirror it so Codex applies its truncation handling. */
      int truncated = (parsed.stop_reason[0] && strcmp(parsed.stop_reason, "length") == 0);
      size_t ccap = n * 6 + 1024;
      char *cf = malloc(ccap);
      if (cf)
      {
         if (truncated)
         {
            if (openai_format_responses_incomplete(id, model, created, "max_output_tokens", cf,
                                                   (int)ccap) > 0)
               emit(ctx, "response.incomplete", cf);
         }
         else if (openai_format_responses_completed(id, model, txt, created, parsed.prompt_tokens,
                                                    parsed.completion_tokens,
                                                    parsed.cache_read_tokens, cf, (int)ccap) > 0)
            emit(ctx, "response.completed", cf);
         free(cf);
      }
   }

   agent_free_parsed_response(&parsed);
   free(instructions);
   cJSON_Delete(messages);
   cJSON_Delete(tools);
   return 0;
}

/* Async /v1/runs execution. POST enqueues a background worker and returns the
 * `queued` run immediately; the worker runs inference off the listener thread,
 * publishes live events + status into the runs store, and honors cancellation
 * at the step boundary. There is no provider-incremental token streaming yet, so
 * output deltas are emitted in one batch once the (blocking) step returns —
 * still off-thread, and preceded by a live `response.in_progress` event so
 * subscribers see progress before completion. */
#define RUN_JSON_CAP (256 * 1024)
#define RUN_DELTA    80

typedef struct
{
   char run_id[64];
   char model[64];
   char *prompt; /* heap; owned by the worker, freed there */
   double temperature;
   int max_tokens;
   long created;
   /* Request context captured at enqueue time (#3): the worker runs on a
    * separate thread, so the request-thread's context is snapshotted here and
    * re-established on the worker before the run, giving the run's audit rows the
    * originating request id / principal / source. */
   request_context_t reqctx;
   int has_reqctx;
} run_job_t;

/* Build a run object with no output text at the given status into buf. Returns
 * the pointer (buf) on success or "{}" if it does not fit. */
static const char *run_status_json(const run_job_t *j, const char *status, char *buf, int cap)
{
   return openai_format_run(j->run_id, j->model, "", j->created, 0, 0, 0, status, buf, cap) > 0
              ? buf
              : "{}";
}

/* Tool-event hook for /v1/runs: append a `tool_call.started` /
 * `tool_call.completed` event (with the tool name) to the run's event stream as
 * each tool fires during the (blocking) turn. ud is the run_id. */
static void runs_tool_event_cb(const char *phase, const char *name, void *ud)
{
   const char *run_id = (const char *)ud;
   char ev[48];
   snprintf(ev, sizeof(ev), "tool_call.%s", phase ? phase : "");
   cJSON *f = cJSON_CreateObject();
   cJSON_AddStringToObject(f, "type", ev);
   cJSON_AddStringToObject(f, "name", name ? name : "");
   char *frame = cJSON_PrintUnformatted(f);
   if (frame)
   {
      openai_runs_store_append_event(run_id, ev, frame);
      free(frame);
   }
   cJSON_Delete(f);
}

static void *run_job_worker(void *arg)
{
   run_job_t *j = (run_job_t *)arg;
   char *buf = (char *)malloc(RUN_JSON_CAP);
   if (!buf)
   {
      free(j->prompt);
      free(j);
      return NULL;
   }

   /* Cancelled before we even start. */
   if (openai_runs_store_cancel_requested(j->run_id))
   {
      openai_runs_store_append_event(j->run_id, "response.completed",
                                     run_status_json(j, "cancelled", buf, RUN_JSON_CAP));
      openai_runs_store_finalize(j->run_id, OPENAI_RUN_CANCELLED,
                                 run_status_json(j, "cancelled", buf, RUN_JSON_CAP));
      goto done;
   }

   openai_runs_store_set_status(j->run_id, OPENAI_RUN_IN_PROGRESS);
   openai_runs_store_update_json(j->run_id, run_status_json(j, "in_progress", buf, RUN_JSON_CAP));
   openai_runs_store_append_event(j->run_id, "response.in_progress",
                                  run_status_json(j, "in_progress", buf, RUN_JSON_CAP));

   agent_config_t acfg;
   agent_t agbuf;
   agent_t *ag = stream_pick_agent(&agbuf, j->model) == 0 ? &agbuf : NULL;
   if (!ag)
   {
      openai_runs_store_append_event(j->run_id, "error", "{\"error\":\"no agent configured\"}");
      openai_runs_store_finalize(j->run_id, OPENAI_RUN_FAILED,
                                 run_status_json(j, "failed", buf, RUN_JSON_CAP));
      goto done;
   }

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   /* /v1/runs is the agentic endpoint (vs. the stateless /v1/chat/completions):
    * run tool-enabled so the run can use aimee's tools and emit tool_call events
    * (AC #7). agent_run_with_tools self-preps the provider catalog and drives the
    * tool loop; the tool-event hook fires from dispatch_tool_call_ctx within it.
    * (ag was validated above as a configured agent; agent_run_with_tools routes
    * by role within the same config — the model field is informational here.) */
   (void)ag;
   agent_tools_set_tool_event_cb(runs_tool_event_cb, (void *)j->run_id);
   /* Re-establish the originating request context on this worker thread (#3) so
    * the run's audit rows carry the request id / principal captured at enqueue.
    * Prefer the request-supplied source over the constant when the proxy stamped
    * one. */
   if (j->has_reqctx)
      request_context_set(&j->reqctx);
   /* /v1/runs drives the agent loop (which logs via agent_log_call); tag that
    * row as ingress so the run's spend is distinguishable from internal agent
    * execution, without writing a second row. */
   agent_set_ingress_source((j->has_reqctx && j->reqctx.source[0]) ? j->reqctx.source
                                                                   : "openai-ingress");
   int erc = agent_run_with_tools(&acfg, "execute", NULL, j->prompt, j->max_tokens, &result);
   agent_set_ingress_source("");
   if (j->has_reqctx)
      request_context_clear();
   agent_tools_set_tool_event_cb(NULL, NULL);

   /* Honor a cancel requested while the (blocking) step ran. */
   if (openai_runs_store_cancel_requested(j->run_id))
   {
      free(result.response);
      openai_runs_store_append_event(j->run_id, "response.completed",
                                     run_status_json(j, "cancelled", buf, RUN_JSON_CAP));
      openai_runs_store_finalize(j->run_id, OPENAI_RUN_CANCELLED,
                                 run_status_json(j, "cancelled", buf, RUN_JSON_CAP));
      goto done;
   }

   if (erc != 0 || !result.response)
   {
      char errbuf[256];
      snprintf(errbuf, sizeof(errbuf), "{\"error\":\"%s\"}",
               result.error[0] ? result.error : "run failed");
      openai_runs_store_append_event(j->run_id, "error", errbuf);
      openai_runs_store_finalize(j->run_id, OPENAI_RUN_FAILED,
                                 run_status_json(j, "failed", buf, RUN_JSON_CAP));
      free(result.response);
      goto done;
   }

   /* Success: emit the output as delta events, then a terminal completed frame. */
   {
      char item_id[160];
      snprintf(item_id, sizeof(item_id), "%s-msg", j->run_id);
      const char *txt = result.response;
      size_t n = strlen(txt);
      for (size_t off = 0; off < n; off += RUN_DELTA)
      {
         char seg[RUN_DELTA + 1];
         size_t take = (n - off < RUN_DELTA) ? (n - off) : RUN_DELTA;
         memcpy(seg, txt + off, take);
         seg[take] = '\0';
         char frame[RUN_DELTA * 6 + 256];
         if (openai_format_responses_delta(item_id, seg, frame, sizeof(frame)) > 0)
            openai_runs_store_append_event(j->run_id, "response.output_text.delta", frame);
      }
      /* message.completed: the full assistant message is now available (the AC's
       * terminal message event, distinct from the run-level response.completed). */
      cJSON *mc = cJSON_CreateObject();
      cJSON_AddStringToObject(mc, "type", "message.completed");
      cJSON_AddStringToObject(mc, "item_id", item_id);
      cJSON *msg = cJSON_AddObjectToObject(mc, "message");
      cJSON_AddStringToObject(msg, "role", "assistant");
      cJSON_AddStringToObject(msg, "content", txt);
      char *mframe = cJSON_PrintUnformatted(mc);
      if (mframe)
      {
         openai_runs_store_append_event(j->run_id, "message.completed", mframe);
         free(mframe);
      }
      cJSON_Delete(mc);
   }
   if (openai_format_run(j->run_id, j->model, result.response, j->created, result.prompt_tokens,
                         result.completion_tokens, result.cache_read_tokens, "completed", buf,
                         RUN_JSON_CAP) > 0)
   {
      openai_runs_store_append_event(j->run_id, "response.completed", buf);
      openai_runs_store_finalize(j->run_id, OPENAI_RUN_COMPLETED, buf);
   }
   else
   {
      openai_runs_store_finalize(j->run_id, OPENAI_RUN_FAILED,
                                 "{\"object\":\"run\",\"status\":\"failed\"}");
   }
   free(result.response);

done:
   free(buf);
   free(j->prompt);
   free(j);
   return NULL;
}

/* POST /v1/runs: parse the request, create a `queued` run, spawn the background
 * worker, and return the queued run object immediately (the listener thread is
 * never blocked on inference). GET /v1/runs/{id}/events streams the worker's
 * live events; POST /v1/runs/{id}/stop requests cancellation. */
static int runs_handler(const char *body, char *resp, int cap)
{
   char model[64] = "";
   char prev_id[128] = "";
   char *prompt = NULL;
   int stream = 0;
   if (openai_parse_responses_request(body, model, sizeof(model), &prompt, prev_id, sizeof(prev_id),
                                      &stream) != 0)
   {
      openai_format_error(resp, cap, "invalid_request_error",
                          "invalid run request: expected non-empty `input`");
      return 400;
   }

   /* Validate an agent is configured synchronously so callers still get 503 fast. */
   agent_t agbuf;
   agent_t *ag = stream_pick_agent(&agbuf, model) == 0 ? &agbuf : NULL;
   if (!ag)
   {
      free(prompt);
      openai_format_error_code(resp, cap, "server_error", "no agent configured",
                               AIMEE_ERR_NO_PRIMARY);
      return 503;
   }

   double temperature = openai_request_double(body, "temperature", OPENAI_CHAT_TEMPERATURE, 2.0);
   int max_tokens = openai_request_int(body, "max_tokens", OPENAI_CHAT_MAX_TOKENS, 32768);

   long created = (long)time(NULL);
   static atomic_ulong g_run_seq = 0; /* connections run concurrently; atomic */
   char id[64];
   snprintf(id, sizeof(id), "run_%ld_%lu", created, atomic_fetch_add(&g_run_seq, 1) + 1);

   /* Queued snapshot returned to the caller and stored for GET /v1/runs/{id}. */
   int len = openai_format_run(id, model, "", created, 0, 0, 0, "queued", resp, cap);
   if (len < 0)
   {
      free(prompt);
      openai_format_error(resp, cap, "server_error", "run did not fit the buffer");
      return 500;
   }
   openai_runs_store_create(id, resp);

   run_job_t *j = (run_job_t *)calloc(1, sizeof(*j));
   if (!j)
   {
      free(prompt);
      openai_runs_store_finalize(id, OPENAI_RUN_FAILED, resp);
      return 200; /* resp still holds the queued object */
   }
   snprintf(j->run_id, sizeof(j->run_id), "%s", id);
   snprintf(j->model, sizeof(j->model), "%s", model);
   j->prompt = prompt; /* ownership moves to the worker */
   j->temperature = temperature;
   j->max_tokens = max_tokens;
   j->created = created;
   /* Snapshot the request context now, on the request thread, so the worker can
    * re-establish it (capture-at-enqueue, #3). */
   {
      const request_context_t *rc = request_context_get();
      if (rc)
      {
         j->reqctx = *rc;
         j->has_reqctx = 1;
      }
   }

   pthread_t th;
   if (pthread_create(&th, NULL, run_job_worker, j) != 0)
   {
      free(j->prompt);
      free(j);
      openai_runs_store_finalize(id, OPENAI_RUN_FAILED, resp);
      return 200;
   }
   pthread_detach(th);
   return 200; /* resp already holds the queued run object */
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
