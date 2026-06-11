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
#include "dogfood.h"                /* dogfood_autolabel_next_turn_live */
#include "learning_implicit.h"      /* learning_implicit_detect_turn */
#include "openai_responses_store.h" /* previous_response_id continuation store */
#include "openai_runs_store.h"      /* GET /v1/runs/{id} record store */
#include "aimee.h"                  /* EMBED_MAX_DIM, MAX_PATH_LEN (used by agent_types.h below) */
#include "config.h"                 /* config_t, config_load */
#include "agent_config.h"
#include "agent_exec.h"
#include "agent_protocol.h"  /* parsed_response_t, message_history_repair */
#include "delegate_driver.h" /* single provider step for the Codex proxy */
#include "http_retry.h"
#include "cJSON.h"
#include "agent_tools.h" /* agent_tools_set_tool_event_cb — /v1/runs tool events */
#include "agent_types.h"
#include "memory.h" /* memory_embed_text */
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
 * rejected streaming, and this path runs no tools — both v1 requirements. */
static int dedup_eligible(char *key, size_t key_cap, const char *model, const char *endpoint,
                          const char *body, const char *context, int *ttl_out)
{
   config_t cfg;
   if (config_load(&cfg) != 0 || !cfg.dedup_enabled)
      return 0;
   const char *idem = request_context_idempotency_key();
   if (!idem || !idem[0])
      return 0;
   response_dedup_key(request_context_principal(), model, endpoint, idem, body, context, key,
                      key_cap);
   if (ttl_out)
      *ttl_out =
          cfg.dedup_window_seconds > 0 ? cfg.dedup_window_seconds : RESPONSE_DEDUP_TTL_SECONDS;
   return 1;
}

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
   char *pi_env = ingress_preinject_build(prompt, 0);

   /* §4 short-window dedup: serve a re-sent identical request (same principal,
    * model, endpoint, body, idempotency key, AND pre-injected context) from the
    * TTL cache without paying for the provider call again. Buffered + non-stream
    * + tool-free here. */
   char dedup_key[224] = "";
   int dedup_ttl = RESPONSE_DEDUP_TTL_SECONDS;
   const char *endpoint = chat ? "/v1/chat/completions" : "/v1/completions";
   int dedup_on =
       dedup_eligible(dedup_key, sizeof(dedup_key), model, endpoint, body, pi_env, &dedup_ttl);
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

   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0)
   {
      free(pi_env);
      free(prompt);
      openai_format_error(resp, cap, "server_error", "no agent configuration available");
      return 503;
   }
   /* Honour the requested model: "aimee" (or empty) means the default agent;
    * any other value selects a configured agent by name, falling back to the
    * default when it doesn't match one. */
   agent_t *ag = NULL;
   if (model[0] && strcmp(model, "aimee") != 0)
      ag = agent_find(&acfg, model);
   if (!ag)
      ag = agent_find(&acfg, acfg.default_agent);
   if (!ag && acfg.agent_count > 0)
      ag = &acfg.agents[0];
   if (!ag)
   {
      free(pi_env);
      free(prompt);
      openai_format_error(resp, cap, "server_error", "no agent configured");
      return 503;
   }

   /* Honour the OpenAI sampling params when present, else fall back to the
    * endpoint defaults. temperature clamps to [0, 2]; max_tokens to [1, hi]. */
   double temperature = openai_request_double(body, "temperature", OPENAI_CHAT_TEMPERATURE, 2.0);
   int max_tokens = openai_request_int(body, "max_tokens", OPENAI_CHAT_MAX_TOKENS, 32768);

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   /* pi_env (the <aimee-context> envelope) was built up front for the dedup key;
    * reuse it as the system prompt here. */
   int erc = agent_execute(ag, pi_env, prompt, max_tokens, temperature, &result);
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

   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   config_load(&cfg);
   const char *cmd = cfg.embedding_command[0] ? cfg.embedding_command : "builtin";

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
      int d = memory_embed_text(inputs[i], cmd, vecs[i], EMBED_MAX_DIM);
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

   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0)
   {
      free(prompt);
      openai_format_error(resp, cap, "server_error", "no agent configuration available");
      return 503;
   }
   agent_t *ag = NULL;
   if (model[0] && strcmp(model, "aimee") != 0)
      ag = agent_find(&acfg, model);
   if (!ag)
      ag = agent_find(&acfg, acfg.default_agent);
   if (!ag && acfg.agent_count > 0)
      ag = &acfg.agents[0];
   if (!ag)
   {
      free(prompt);
      openai_format_error(resp, cap, "server_error", "no agent configured");
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
   char *pi_env = ingress_preinject_build(full, 0);
   int erc = agent_execute(ag, pi_env, full, max_tokens, temperature, &result);
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
                                    result.completion_tokens, resp, cap);
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
static agent_t *stream_pick_agent(agent_config_t *acfg, const char *model)
{
   if (agent_load_config(acfg) != 0)
      return NULL;
   agent_t *ag = NULL;
   if (model[0] && strcmp(model, "aimee") != 0)
      ag = agent_find(acfg, model);
   if (!ag)
      ag = agent_find(acfg, acfg->default_agent);
   if (!ag && acfg->agent_count > 0)
      ag = &acfg->agents[0];
   return ag;
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

   agent_config_t acfg;
   agent_t *ag = stream_pick_agent(&acfg, model);
   if (!ag)
   {
      emit_chunk(emit, ctx, id, model, created, 1, "no agent configured", 0);
      emit_chunk(emit, ctx, id, model, created, 0, NULL, 1);
      free(prompt);
      return 0;
   }

   double temperature = openai_request_double(body, "temperature", OPENAI_CHAT_TEMPERATURE, 2.0);
   int max_tokens = openai_request_int(body, "max_tokens", OPENAI_CHAT_MAX_TOKENS, 32768);

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   /* P1 pre-injection: prepend the <aimee-context> envelope as the system
    * prompt (config ingress_preinject_enabled; no-op when off/empty). */
   char *pi_env = ingress_preinject_build(prompt, 0);
   int erc = agent_execute(ag, pi_env, prompt, max_tokens, temperature, &result);
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

   agent_config_t acfg;
   agent_t *ag = stream_pick_agent(&acfg, model);
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
   char *pi_env = ingress_preinject_build(prompt, 0);
   int erc = agent_execute(ag, pi_env, prompt, max_tokens, temperature, &result);
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

/* Single provider step for the Codex proxy: build the request for `agent`'s
 * provider from a caller-supplied messages array and passthrough `tools`, POST
 * it, and parse the reply into *out. Unlike agent_run_with_tools this does NOT
 * execute tool calls — out->calls[] surfaces them so the handler can relay them
 * to Codex. Returns 0 on success, -1 on transport/parse failure. *out is zeroed
 * here; the caller frees it with agent_free_parsed_response(). */
static int agent_execute_messages(const agent_t *agent, cJSON *messages, cJSON *tools,
                                  const char *system_prompt, int max_tokens, double temperature,
                                  parsed_response_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   if (!agent || !messages)
      return -1;

   delegate_drivers_init();
   const delegate_driver_t *driver = delegate_driver_get(agent->provider);
   if (!driver || !driver->build_request || !driver->parse_response)
      return -1;

   char url[MAX_ENDPOINT_LEN + 64];
   if (delegate_build_url(driver, agent, url, sizeof(url)) != 0)
      return -1;

   char auth_header[MAX_API_KEY_LEN + 32];
   if (agent_resolve_auth(agent, auth_header, sizeof(auth_header)) != 0)
      return -1;
   char extra_headers[512];
   agent_build_extra_headers(agent, extra_headers, sizeof(extra_headers));

   int tok = (max_tokens > 0) ? max_tokens : agent->max_tokens;
   cJSON *req = driver->build_request(agent, messages, tools, system_prompt, tok, temperature);
   if (!req)
      return -1;
   char *body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!body)
      return -1;

   char *response_body = NULL;
   config_t retry_cfg;
   config_load(&retry_cfg);
   int ra =
       retry_cfg.retry_max_attempts > 0 ? retry_cfg.retry_max_attempts : HTTP_RETRY_MAX_ATTEMPTS;
   int rb = retry_cfg.retry_base_ms > 0 ? retry_cfg.retry_base_ms : HTTP_RETRY_BASE_MS;
   int rm = retry_cfg.retry_max_ms > 0 ? retry_cfg.retry_max_ms : HTTP_RETRY_MAX_MS;
   int http_status =
       http_retry_post_context(url, auth_header, body, &response_body, agent->timeout_ms,
                               extra_headers, ra, rb, rm, agent->provider, agent->model, NULL);
   free(body);

   if (http_status != 200 || !response_body)
   {
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

   if (openai_parse_responses_to_chat(body, model, sizeof(model), &instructions, &messages, &tools,
                                      &stream) != 0)
   {
      if (openai_format_responses_created(id, model, created, frame, sizeof(frame)) > 0)
         emit(ctx, "response.created", frame);
      if (openai_format_responses_completed(id, model, "invalid responses request", created, 0, 0,
                                            frame, sizeof(frame)) > 0)
         emit(ctx, "response.completed", frame);
      free(instructions);
      cJSON_Delete(messages);
      cJSON_Delete(tools);
      return 0;
   }
   (void)stream; /* this handler is the streaming path; flag is informational */

   agent_config_t acfg;
   agent_t *ag = stream_pick_agent(&acfg, model);
   if (!ag)
   {
      if (openai_format_responses_created(id, model, created, frame, sizeof(frame)) > 0)
         emit(ctx, "response.created", frame);
      if (openai_format_responses_completed(id, model, "no agent configured", created, 0, 0, frame,
                                            sizeof(frame)) > 0)
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
         free(turn_text);
      }
   }

   /* P1 context pre-injection (config: ingress_preinject_enabled, default off):
    * prepend a fusion-recall <aimee-context> envelope to the system prompt so the
    * external agent reasons over already-loaded context instead of re-exploring
    * the repo. No-op when disabled or when recall yields nothing. */
   {
      char *pi_query = ingress_preinject_query_from_messages(messages);
      char *pi_env = ingress_preinject_build(pi_query, 0);
      if (pi_env)
      {
         char *merged = ingress_preinject_apply(instructions, pi_env);
         if (merged)
         {
            free(instructions);
            instructions = merged;
         }
      }
      free(pi_query);
      free(pi_env);
   }

   parsed_response_t parsed;
   int erc =
       agent_execute_messages(ag, messages, tools, instructions, max_tokens, temperature, &parsed);

   if (openai_format_responses_created(id, model, created, frame, sizeof(frame)) > 0)
      emit(ctx, "response.created", frame);

   if (erc == 0)
   {
      /* Cost accounting: streaming /v1/responses runs the provider call directly.
       * agent_execute_messages returns a parsed_response_t (no model/agent name),
       * so synthesize the fields the audit needs from the served agent. */
      agent_result_t ar;
      memset(&ar, 0, sizeof(ar));
      snprintf(ar.agent_name, sizeof(ar.agent_name), "%s", ag->name);
      snprintf(ar.model, sizeof(ar.model), "%s", ag->model);
      snprintf(ar.requested_model, sizeof(ar.requested_model), "%s", model);
      ar.prompt_tokens = parsed.prompt_tokens;
      ar.completion_tokens = parsed.completion_tokens;
      ar.cache_write_tokens = parsed.cache_write_tokens;
      ar.cache_read_tokens = parsed.cache_read_tokens;
      snprintf(ar.stop_reason, sizeof(ar.stop_reason), "%s", parsed.stop_reason);
      if (agent_ingress_accounting_enabled())
         agent_record_token_audit(&ar, "", "openai-ingress");
   }

   if (erc == 0 && parsed.is_tool_call && parsed.call_count > 0)
   {
      /* Tool calls: relay each as a function_call item for Codex to execute. */
      cJSON *output = cJSON_CreateArray();
      for (int i = 0; i < parsed.call_count; i++)
      {
         const char *name = parsed.calls[i].name;
         const char *cid = parsed.calls[i].id;
         const char *args = parsed.calls[i].arguments ? parsed.calls[i].arguments : "{}";
         char fc_id[80];
         snprintf(fc_id, sizeof(fc_id), "%s-fc-%d", id, i);

         char added[256];
         if (openai_format_responses_fc_item_added(fc_id, cid, name, i, added, sizeof(added)) > 0)
            emit(ctx, "response.output_item.added", added);

         size_t acap = strlen(args) * 6 + 256;
         char *af = malloc(acap);
         if (af)
         {
            if (openai_format_responses_fc_args_delta(fc_id, i, args, af, (int)acap) > 0)
               emit(ctx, "response.function_call_arguments.delta", af);
            if (openai_format_responses_fc_args_done(fc_id, i, args, af, (int)acap) > 0)
               emit(ctx, "response.function_call_arguments.done", af);
            free(af);
         }

         size_t dcap = strlen(args) * 6 + strlen(name) + strlen(cid) + 512;
         char *df = malloc(dcap);
         if (df)
         {
            if (openai_format_responses_fc_item_done(fc_id, cid, name, args, i, df, (int)dcap) > 0)
               emit(ctx, "response.output_item.done", df);
            free(df);
         }
         if (output)
            cJSON_AddItemToArray(
                output, openai_responses_function_call_item(fc_id, cid, name, args, "completed"));
      }

      size_t ccap = 4096;
      for (int i = 0; i < parsed.call_count; i++)
         ccap += (parsed.calls[i].arguments ? strlen(parsed.calls[i].arguments) : 0) * 6 + 256;
      char *cf = malloc(ccap);
      if (cf)
      {
         if (openai_format_responses_completed_items(id, model, created, output,
                                                     parsed.prompt_tokens, parsed.completion_tokens,
                                                     cf, (int)ccap) > 0)
            emit(ctx, "response.completed", cf);
         free(cf);
      }
      else
         cJSON_Delete(output);
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

      size_t ccap = n * 6 + 1024;
      char *cf = malloc(ccap);
      if (cf)
      {
         if (openai_format_responses_completed(id, model, txt, created, parsed.prompt_tokens,
                                               parsed.completion_tokens, cf, (int)ccap) > 0)
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
   return openai_format_run(j->run_id, j->model, "", j->created, 0, 0, status, buf, cap) > 0 ? buf
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
   agent_t *ag = stream_pick_agent(&acfg, j->model);
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
                         result.completion_tokens, "completed", buf, RUN_JSON_CAP) > 0)
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
   agent_config_t acfg;
   agent_t *ag = stream_pick_agent(&acfg, model);
   if (!ag)
   {
      free(prompt);
      openai_format_error(resp, cap, "server_error", "no agent configured");
      return 503;
   }

   double temperature = openai_request_double(body, "temperature", OPENAI_CHAT_TEMPERATURE, 2.0);
   int max_tokens = openai_request_int(body, "max_tokens", OPENAI_CHAT_MAX_TOKENS, 32768);

   long created = (long)time(NULL);
   static atomic_ulong g_run_seq = 0; /* connections run concurrently; atomic */
   char id[64];
   snprintf(id, sizeof(id), "run_%ld_%lu", created, atomic_fetch_add(&g_run_seq, 1) + 1);

   /* Queued snapshot returned to the caller and stored for GET /v1/runs/{id}. */
   int len = openai_format_run(id, model, "", created, 0, 0, "queued", resp, cap);
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
