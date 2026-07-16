/* agent_ir_parse.c: parse a provider JSON response through the canonical IR and
 * bridge it into the legacy parsed_response_t the delegate turn loop consumes.
 * Split out of agent_runtime.c so it can be unit-tested without the whole runtime. */
#include "aimee.h"

#include "agent_protocol.h"
#include "aimee_backend.h"
#include "aimee_ir.h"
#include "tool_call_args.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

/* Parse a provider JSON response through the canonical IR and bridge it into the
 * legacy parsed_response_t the turn loop consumes. Content and tool calls come from
 * the IR backend parser -- the proven path (shadow-validated 191/191 on live .254
 * traffic across the anthropic + openai wires, 0 mismatches). assistant_message --
 * the conversation-replay turn, which the response shadow does NOT cover -- is taken
 * from `root` exactly as the legacy parser does, so multi-turn history stays
 * byte-identical. Returns 0 on success; -1 if the IR could not parse (the caller
 * then falls back to the legacy translator). */
int agent_ir_parse_json_response(cJSON *root, int anthropic, parsed_response_t *out)
{
   memset(out, 0, sizeof(*out));
   aimee_response_t ir;
   memset(&ir, 0, sizeof(ir));
   char err[128] = "";
   int rc = anthropic ? anthropic_backend_parse(root, &ir, err, sizeof err)
                      : openai_backend_parse(root, &ir, err, sizeof err);
   if (rc != 0)
   {
      aimee_response_free(&ir);
      return -1;
   }

   if (ir.model)
      snprintf(out->model, sizeof(out->model), "%s", ir.model);
   if (ir.raw_stop_reason)
      snprintf(out->stop_reason, sizeof(out->stop_reason), "%s", ir.raw_stop_reason);
   out->prompt_tokens = (int)ir.usage_in;
   out->completion_tokens = (int)ir.usage_out;
   out->cache_write_tokens = (int)ir.usage_cache_write;
   out->cache_read_tokens = (int)ir.usage_cache_read;

   /* content: concatenated TEXT blocks (THINKING is excluded by the IR accessor). */
   size_t need = 1;
   for (int i = 0; i < ir.n_content; i++)
      if (ir.content[i].type == AIMEE_BLK_TEXT && ir.content[i].text)
         need += strlen(ir.content[i].text);
   if (need > 1)
   {
      out->content = malloc(need);
      if (out->content)
         aimee_ir_response_text(&ir, out->content, need);
   }

   /* tool calls: id / name / arguments (arguments as a JSON string, like legacy). */
   for (int i = 0; i < ir.n_content && out->call_count < AGENT_MAX_TOOL_CALLS; i++)
   {
      const aimee_block_t *b = &ir.content[i];
      if (b->type != AIMEE_BLK_TOOL_USE)
         continue;
      out->is_tool_call = 1;
      parsed_tool_call_t *c = &out->calls[out->call_count++];
      if (b->tool_id)
         snprintf(c->id, sizeof(c->id), "%s", b->tool_id);
      if (b->tool_name)
         snprintf(c->name, sizeof(c->name), "%s", b->tool_name);
      char *args = b->tool_input ? cJSON_PrintUnformatted(b->tool_input) : NULL;
      c->arguments = args ? args : strdup("{}");
   }

   /* assistant_message for multi-turn replay -- taken from the raw response the same
    * way the legacy parser does (content array for anthropic; the choice message,
    * normalized, for openai), since the shadow validated content/tools but not this. */
   if (anthropic)
   {
      if (out->is_tool_call)
      {
         cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
         if (content)
            out->assistant_message = cJSON_Duplicate(content, 1);
      }
   }
   else
   {
      cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
      cJSON *choice0 = choices ? cJSON_GetArrayItem(choices, 0) : NULL;
      cJSON *msg = choice0 ? cJSON_GetObjectItemCaseSensitive(choice0, "message") : NULL;
      if (msg)
      {
         out->assistant_message = cJSON_Duplicate(msg, 1);
         for (int i = 0; i < out->call_count; i++)
            tool_call_normalize_assistant_arguments(out->assistant_message, i,
                                                    out->calls[i].arguments);
         tool_call_sanitize_assistant_arguments(out->assistant_message);
      }
   }

   aimee_response_free(&ir);
   return 0;
}
