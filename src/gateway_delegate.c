/* gateway_delegate.c: P4 — wire aimee's own outbound model-call loop into the gateway
 * pipeline. See gateway_delegate.h for the scope/threat model. This module composes the
 * shared gw_pipeline_run_request runner (gateway_pipeline.c) with the shared
 * gateway_policy_* primitives (gateway_policy.c) — one implementation of tool-policing,
 * called from both the ingresses and here. */
#include "aimee.h" /* size macros for agent_types.h (MAX_PATH_LEN), via gateway_policy.h */
#include "gateway_delegate.h"

#include "cJSON.h"
#include "gateway_pipeline.h"
#include "gateway_policy.h"
#include "log.h"

gw_tool_shape_t gateway_delegate_tool_shape(int anthropic, int chatgpt)
{
   if (anthropic || chatgpt)
      return GW_TOOL_SHAPE_NAMED;        /* anthropic + /responses both carry a flat tool.name */
   return GW_TOOL_SHAPE_FUNCTION_NESTED; /* openai /chat/completions */
}

/* Per-call userdata for the tool-policing stage: the runtime shape + delegate gate
 * (a static stage list cannot carry these, so they ride in the stage's ud). */
typedef struct
{
   gw_tool_shape_t shape;
   int is_delegate;
} tp_ud_t;

/* The ONLY stage the delegate/primary loop runs. Memory + model-pin are deliberately
 * omitted (ingress concerns — see header); do not add them here without revisiting the
 * double-inject / fallback-clobber rationale. No-op for the primary and for an
 * unsupported tool shape (the latter logs when there was something to police). */
static int gw_stage_tool_policing(gw_request_t *r, void *ud)
{
   const tp_ud_t *u = (const tp_ud_t *)ud;

   if (!u->is_delegate)
      return 0; /* primary shares the pipeline but is not policed here */

   if (u->shape == GW_TOOL_SHAPE_UNSUPPORTED)
   {
      cJSON *tools = cJSON_GetObjectItemCaseSensitive(r->raw, "tools");
      if (gateway_prevent_subagents_enabled() && cJSON_IsArray(tools) &&
          cJSON_GetArraySize(tools) > 0)
         LOG_WARN("gateway", "tool-policing unsupported for this provider's tool shape; "
                             "subagent-strip skipped on a delegate request");
      return 0;
   }

   return gateway_policy_apply_request(r->raw, u->shape == GW_TOOL_SHAPE_FUNCTION_NESTED ? 1 : 0);
}

int gateway_delegate_run_request_pipeline(cJSON *req, gw_tool_shape_t shape, int is_delegate)
{
   tp_ud_t ud = {shape, is_delegate};
   gw_request_t r = {
       .raw = req,
       .driver = NULL,
       .ag = NULL,
       /* The tool-policing stage keys off `ud->shape`, NOT these IR fields, so
        * serving_api/mem_target/parity/stream are inert here — placeholders, not an
        * accurate label of the provider (gw_api_t has no gemini value, and a NAMED
        * shape covers both anthropic and openai /responses). If a future stage that
        * DOES read serving_api/mem_target is added to this loop, derive them properly
        * from the driver first. */
       .serving_api = shape == GW_TOOL_SHAPE_FUNCTION_NESTED ? GW_API_OPENAI : GW_API_ANTHROPIC,
       .mem_target = GW_MEM_ANTHROPIC_MESSAGES,
       .parity = 0,
       .stream = 0,
   };
   /* One documented stage. (Not a file-scope const because the ud is per-call.) */
   const gw_stage_t stages[] = {
       {gw_stage_tool_policing, &ud, "tool_policing"},
   };
   return gw_pipeline_run_request(&r, stages, sizeof(stages) / sizeof(stages[0]));
}
