/* aimee_ir_serve.c -- see aimee_ir_serve.h. */
#include "aimee_ir_serve.h"

#include "aimee_backend.h"
#include "aimee_frontend.h"
#include "gw_stage_memory.h" /* ir_stage_memory + gw_stage_memory_enabled */
#include "config.h"          /* config_load + config_module_enabled (modules.memory) */
#include "aimee_ir_metrics.h"
#include "aimee.h"          /* size macros for agent_types.h */
#include "agent_protocol.h" /* parsed_response_t (Slice 3 transitional adapter) */
#include "aimee_ir.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int aimee_ir_path_enabled(void)
{
   /* DEFAULT-ON: the IR path is now the primary request-build path (proven live on
    * .254 for Claude Code->codex incl. tools/streaming). An explicit setting wins;
    * legacy translators remain as the automatic fallback on any IR-build failure.
    * Set AIMEE_IR_PATH=0 to force the legacy path. */
   const char *v = getenv("AIMEE_IR_PATH");
   if (v && v[0])
      return v[0] != '0';
   return 1;
}

int aimee_ir_stream_relay_enabled(void)
{
   /* DEFAULT-OFF, gated SEPARATELY from AIMEE_IR_PATH (roundtable Q6: gate
    * buffered-IR / streaming-IR / passthrough independently). When on, the
    * incremental OpenAI-chat -> Anthropic SSE relay is driven by the neutral
    * IR-delta model (openai_chunk_to_deltas -> anthropic_delta_emit) instead of
    * the legacy anthropic_stream_feed_openai translator -- eliminating the last
    * live direct-translation site. Ships dark; enablement is a rollout decision
    * gated on live cross-protocol parity, exactly like the legacy-deletion step.
    * (The user's codex config uses the buffered-replay path, not this relay.) */
   const char *v = getenv("AIMEE_IR_STREAM_RELAY");
   return v && (strcmp(v, "1") == 0 || strcmp(v, "on") == 0 || strcmp(v, "true") == 0);
}

/* Resolve the memory module toggle: config-store modules.memory (canonical) -> env
 * default (gw_stage_memory_enabled). Cached config_load, so an operator toggle applies
 * without a restart; keeps ir_stage_memory itself config-free. Resolved at the seam
 * call site, mirroring the legacy gw_stage_slot_t catalogs. */
static int ir_memory_enabled(void)
{
   config_t c;
   int tri = (config_load(&c) == 0) ? c.module_memory : -1;
   return config_module_enabled(tri, gw_stage_memory_enabled());
}

void aimee_ir_apply_request_stages(aimee_request_t *ir, int memory_enabled)
{
   /* The one place modules register on the IR (universal-gateway P4): fired once per
    * ingress, after frontend_parse and before backend_build, protocol-neutral. Memory
    * is the first ported module -- it replaces gw_stage_memory's two structured-ingress
    * arms (anthropic /v1/messages + /v1/responses). The four legacy plain-chat handlers
    * stay on gw_memory_system_prompt until agent_execute itself moves onto the IR. */
   const aimee_ir_transform_t stages[] = {
       {"memory", ir_stage_memory, NULL, memory_enabled},
   };
   aimee_ir_run_transforms(ir, stages, sizeof stages / sizeof stages[0]);
}

char *aimee_ir_build_provider_body(const cJSON *req, const char *driver_name,
                                   const char *agent_model, int max_tokens_override,
                                   int want_stream)
{
   aimee_request_t ir;
   char err[128];
   if (anthropic_frontend_parse(req, &ir, err, sizeof err) != 0)
   {
      aimee_ir_metric_inc(AIMEE_IR_M_PARSE_FAIL, AIMEE_WIRE_ANTHROPIC);
      return NULL;
   }
   /* the served model + cap come from the configured agent, not the client */
   if (agent_model && agent_model[0])
   {
      free(ir.model);
      ir.model = strdup(agent_model);
   }
   if (max_tokens_override > 0)
   {
      ir.max_tokens = max_tokens_override;
      ir.has_max_tokens = 1;
   }
   /* Match the legacy ingress, which defaults an absent temperature to 1.0 and
    * always emits it (build_provider_body is fed jo_num(req, "temperature", 1.0)).
    * Applied at BUILD time, like legacy -- not in the frontend parse, so the IR
    * request itself stays protocol-neutral (an Anthropic-parsed and OpenAI-parsed
    * copy of the same request remain equal). Without it the IR omits temperature
    * when the client sends none, diverging from the legacy provider body. */
   if (!ir.has_temperature)
   {
      ir.temperature = 1.0;
      ir.has_temperature = 1;
   }
   /* The upstream stream flag is the CALLER's decision, not the client's: a caller
    * that streams to the client may still want the upstream reply buffered so it can
    * police + replay it. Inheriting ir.stream from the request is what made the
    * buffered-replay path ask for SSE and then parse it as JSON. */
   ir.stream = want_stream ? 1 : 0;

   aimee_ir_apply_request_stages(
       &ir, ir_memory_enabled()); /* the single protocol-neutral module stage (memory ported) */

   int is_responses = driver_name && strcmp(driver_name, "chatgpt") == 0;
   cJSON *prov = is_responses ? responses_backend_build(&ir) : openai_backend_build(&ir);
   aimee_request_free(&ir);
   if (!prov)
   {
      aimee_ir_metric_inc(AIMEE_IR_M_BACKEND_BUILD_FAIL, AIMEE_WIRE_ANTHROPIC);
      return NULL;
   }
   char *s = cJSON_PrintUnformatted(prov);
   cJSON_Delete(prov);
   if (s)
      aimee_ir_metric_inc(AIMEE_IR_M_IR_PATH, AIMEE_WIRE_ANTHROPIC);
   return s;
}

static const char *jo_str(const cJSON *o, const char *k)
{
   const cJSON *it = cJSON_GetObjectItemCaseSensitive((cJSON *)o, k);
   return (it && cJSON_IsString(it)) ? it->valuestring : NULL;
}

int aimee_ir_responses_to_chat(const char *body, char *model, size_t model_n,
                               char **instructions_out, cJSON **messages_out, cJSON **tools_out,
                               int *stream_out)
{
   if (model && model_n)
      model[0] = '\0';
   if (instructions_out)
      *instructions_out = NULL;
   if (messages_out)
      *messages_out = NULL;
   if (tools_out)
      *tools_out = NULL;
   if (stream_out)
      *stream_out = 0;

   cJSON *req = cJSON_Parse((body && body[0]) ? body : "{}");
   if (!req)
      return -1;
   aimee_request_t ir;
   char err[128];
   if (responses_frontend_parse(req, &ir, err, sizeof err) != 0)
   {
      cJSON_Delete(req);
      aimee_ir_metric_inc(AIMEE_IR_M_PARSE_FAIL, AIMEE_WIRE_RESPONSES);
      return -1;
   }
   cJSON_Delete(req);
   if (model && model_n && ir.model)
      snprintf(model, model_n, "%s", ir.model);
   if (stream_out)
      *stream_out = ir.stream;

   aimee_ir_apply_request_stages(
       &ir, ir_memory_enabled()); /* the single protocol-neutral module stage (memory ported) */

   /* build the chat shape, then split leading system messages -> instructions */
   cJSON *chat = openai_backend_build(&ir);
   aimee_request_free(&ir);
   if (!chat)
   {
      aimee_ir_metric_inc(AIMEE_IR_M_BACKEND_BUILD_FAIL, AIMEE_WIRE_RESPONSES);
      return -1;
   }
   cJSON *msgs = cJSON_DetachItemFromObjectCaseSensitive(chat, "messages");
   cJSON *tools = cJSON_DetachItemFromObjectCaseSensitive(chat, "tools");
   cJSON_Delete(chat);

   char *instr = NULL;
   size_t ilen = 0;
   cJSON *first;
   while (msgs && (first = cJSON_GetArrayItem(msgs, 0)) != NULL)
   {
      const char *role = jo_str(first, "role");
      if (!role || strcmp(role, "system") != 0)
         break;
      const char *c = jo_str(first, "content");
      if (c && c[0])
      {
         size_t cl = strlen(c);
         char *p = realloc(instr, ilen + cl + 3);
         if (p)
         {
            instr = p;
            if (ilen)
            {
               memcpy(instr + ilen, "\n\n", 2);
               ilen += 2;
            }
            memcpy(instr + ilen, c, cl);
            ilen += cl;
            instr[ilen] = '\0';
         }
      }
      cJSON_DeleteItemFromArray(msgs, 0);
   }

   if (instructions_out)
      *instructions_out = instr;
   else
      free(instr);
   if (messages_out)
      *messages_out = msgs;
   else
      cJSON_Delete(msgs);
   if (tools_out)
      *tools_out = tools;
   else
      cJSON_Delete(tools);
   aimee_ir_metric_inc(AIMEE_IR_M_IR_PATH, AIMEE_WIRE_RESPONSES);
   return 0;
}

cJSON *aimee_ir_build_from_chat(const char *agent_model, const cJSON *messages, const cJSON *tools,
                                const char *system, const char *driver_name)
{
   /* assemble a chat request {model, messages: [system?] + messages, tools} */
   cJSON *chat = cJSON_CreateObject();
   if (agent_model)
      cJSON_AddStringToObject(chat, "model", agent_model);
   cJSON *msgs = cJSON_AddArrayToObject(chat, "messages");
   if (system && system[0])
   {
      cJSON *sm = cJSON_CreateObject();
      cJSON_AddStringToObject(sm, "role", "system");
      cJSON_AddStringToObject(sm, "content", system);
      cJSON_AddItemToArray(msgs, sm);
   }
   if (cJSON_IsArray(messages))
   {
      const cJSON *m = NULL;
      cJSON_ArrayForEach(m, messages) cJSON_AddItemToArray(msgs, cJSON_Duplicate(m, 1));
   }
   if (cJSON_IsArray(tools))
      cJSON_AddItemToObject(chat, "tools", cJSON_Duplicate((cJSON *)tools, 1));

   aimee_request_t ir;
   char err[128];
   int rc = openai_frontend_parse(chat, &ir, err, sizeof err);
   cJSON_Delete(chat);
   if (rc != 0)
   {
      aimee_ir_metric_inc(AIMEE_IR_M_PARSE_FAIL, AIMEE_WIRE_OPENAI_CHAT);
      return NULL;
   }
   if (agent_model && agent_model[0])
   {
      free(ir.model);
      ir.model = strdup(agent_model);
   }
   aimee_ir_apply_request_stages(
       &ir, ir_memory_enabled()); /* the single protocol-neutral module stage (memory ported) */

   int is_responses = driver_name && strcmp(driver_name, "chatgpt") == 0;
   cJSON *prov = is_responses ? responses_backend_build(&ir) : openai_backend_build(&ir);
   aimee_request_free(&ir);
   if (!prov)
      aimee_ir_metric_inc(AIMEE_IR_M_BACKEND_BUILD_FAIL, AIMEE_WIRE_OPENAI_CHAT);
   else
      aimee_ir_metric_inc(AIMEE_IR_M_IR_PATH, AIMEE_WIRE_OPENAI_CHAT);
   return prov;
}

/* aimee_ir_resp_path_enabled -- Slice 3 gate (config-only: AIMEE_IR_RESP_PATH env,
 * DEFAULT-OFF). When on, the OPENAI-WIRE buffered response parse on the /v1/messages
 * ingress runs provider-JSON -> openai_backend_parse -> IR -> aimee_ir_response_to_parsed,
 * replacing driver->parse_response. Per-wire by ruling: anthropic + responses stay on
 * legacy until their response-shadow parity is proven live. Ships dark; flip after the
 * on-box emit diff confirms byte-identity. */
int aimee_ir_resp_path_enabled(void)
{
   const char *v = getenv("AIMEE_IR_RESP_PATH");
   return v && (strcmp(v, "1") == 0 || strcmp(v, "on") == 0 || strcmp(v, "true") == 0);
}

/* TRANSITIONAL (Slice 3, canonical-IR) -- bridge the IR response back to the legacy
 * parsed_response_t so the existing Anthropic emit + gateway policing keep working
 * while the response PARSE moves onto the IR. REMOVE once the emit + police paths are
 * IR-native (proposal: full IR render + IR-coupled police). DO NOT add new callers.
 * Fills every parsed_response_t field the ingress emit/police consume (audited): content,
 * calls[]/call_count, is_tool_call, prompt/completion/cache tokens, stop_reason, model.
 * `out` is zeroed here; caller frees via the usual agent_free_parsed_response. */
void aimee_ir_response_to_parsed(const aimee_response_t *r, parsed_response_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   if (!r)
      return;

   out->prompt_tokens = (int)r->usage_in;
   out->completion_tokens = (int)r->usage_out;
   out->cache_read_tokens = (int)r->usage_cache_read;
   out->cache_write_tokens = (int)r->usage_cache_write;
   if (r->model)
      snprintf(out->model, sizeof(out->model), "%s", r->model);
   if (r->raw_stop_reason)
      snprintf(out->stop_reason, sizeof(out->stop_reason), "%s", r->raw_stop_reason);

   /* content = concatenation of all TEXT blocks (parsed_response_t convention: a
    * single flat string; NULL when there is no text, e.g. a pure tool call). */
   size_t cap = 1;
   for (int i = 0; i < r->n_content; i++)
      if (r->content[i].type == AIMEE_BLK_TEXT && r->content[i].text)
         cap += strlen(r->content[i].text);
   if (cap > 1)
   {
      out->content = malloc(cap);
      if (out->content)
      {
         out->content[0] = '\0';
         for (int i = 0; i < r->n_content; i++)
            if (r->content[i].type == AIMEE_BLK_TEXT && r->content[i].text)
               strncat(out->content, r->content[i].text, cap - strlen(out->content) - 1);
      }
   }

   /* tool calls: id (opaque) / name / arguments (serialized tool_input JSON). */
   int nc = 0;
   for (int i = 0; i < r->n_content && nc < AGENT_MAX_TOOL_CALLS; i++)
   {
      const aimee_block_t *b = &r->content[i];
      if (b->type != AIMEE_BLK_TOOL_USE)
         continue;
      parsed_tool_call_t *c = &out->calls[nc++];
      if (b->tool_id)
         snprintf(c->id, sizeof(c->id), "%s", b->tool_id);
      if (b->tool_name)
         snprintf(c->name, sizeof(c->name), "%s", b->tool_name);
      c->arguments = b->tool_input ? cJSON_PrintUnformatted(b->tool_input) : NULL;
      if (!c->arguments)
         c->arguments = strdup("{}");
   }
   out->call_count = nc;
   out->is_tool_call = (nc > 0);
}
