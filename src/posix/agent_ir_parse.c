/* agent_ir_parse.c: parse a provider JSON response through the canonical IR and
 * bridge it into the legacy parsed_response_t the delegate turn loop consumes. The
 * parser also OWNS the XML tool-call rescue (aimee_ir_rescue_tool_calls): a model
 * without native function-calling may embed <tool_call> blocks in its text, and
 * that recovery now happens here, on the IR's typed TEXT blocks, rather than as a
 * post-parse pass over flat text in the turn loop. Split out of agent_runtime.c so
 * it can be unit-tested without the whole runtime. */
#include "aimee.h"

#include "agent_protocol.h"
#include "aimee_backend.h"
#include "aimee_ir.h"
#include "aimee_ir_rescue.h"
#include "tool_call_args.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

/* Build an OpenAI assistant message (role + null content + tool_calls) from the
 * parsed calls. Used to rebuild assistant_message after an XML rescue, where the
 * raw response had the calls as prose and cannot serve as the replay turn. Shared
 * with the legacy fallback rescue path in agent_runtime.c. */
cJSON *agent_build_openai_assistant_message_from_calls(parsed_response_t *parsed)
{
   cJSON *asst = cJSON_CreateObject();
   cJSON *tool_calls = cJSON_CreateArray();
   if (!asst || !tool_calls)
   {
      cJSON_Delete(asst);
      cJSON_Delete(tool_calls);
      return NULL;
   }

   cJSON_AddStringToObject(asst, "role", "assistant");
   cJSON_AddNullToObject(asst, "content");
   cJSON_AddItemToObject(asst, "tool_calls", tool_calls);

   for (int i = 0; i < parsed->call_count; i++)
   {
      parsed_tool_call_t *call = &parsed->calls[i];
      if (!call->id[0])
         snprintf(call->id, sizeof(call->id), "xml_call_%d", i + 1);

      cJSON *tc = cJSON_CreateObject();
      cJSON *fn = cJSON_CreateObject();
      if (!tc || !fn)
      {
         cJSON_Delete(tc);
         cJSON_Delete(fn);
         continue;
      }
      cJSON_AddStringToObject(tc, "id", call->id);
      cJSON_AddStringToObject(tc, "type", "function");
      cJSON_AddStringToObject(fn, "name", call->name);
      cJSON_AddStringToObject(fn, "arguments", call->arguments ? call->arguments : "{}");
      cJSON_AddItemToObject(tc, "function", fn);
      cJSON_AddItemToArray(tool_calls, tc);
   }

   return asst;
}

/* Parse a provider JSON response through the canonical IR and bridge it into the
 * legacy parsed_response_t the turn loop consumes.
 *
 * Content and tool calls come from the IR backend parser -- the proven path
 * (shadow-validated 191/191 on live .254 traffic across the anthropic + openai
 * wires, 0 mismatches). `rescue_mode` gates the XML tool-call rescue that the parser
 * now owns: <0 skips it; 0 rescues XML/dialect calls but not bare prose JSON; 1 also
 * rescues bare JSON. `*n_rescued` (when non-NULL) receives how many calls the rescue
 * recovered, so the caller can treat a rescued FINAL turn as a tool-in-final-turn
 * policy violation without re-scanning the text.
 *
 * assistant_message -- the multi-turn replay turn, which the response shadow does
 * NOT cover -- is taken from the raw response exactly as the legacy parser does
 * (content array for anthropic; the choice message, normalized, for openai) so
 * history stays byte-identical -- UNLESS a rescue fired, in which case the raw
 * response held the calls as prose and cannot replay them: anthropic drops it (as
 * legacy did) and openai rebuilds it from the recovered calls.
 *
 * Returns 0 on success; -1 if the IR could not parse (caller yields an empty
 * response -- the legacy translators are gone). */
static void ir_bridge_common(const aimee_response_t *ir, parsed_response_t *out);

int agent_ir_parse_json_response(cJSON *root, int anthropic, int rescue_mode, int *n_rescued,
                                 parsed_response_t *out)
{
   memset(out, 0, sizeof(*out));
   if (n_rescued)
      *n_rescued = 0;
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

   /* XML tool-call rescue, owned by the parser: scans the IR's TEXT blocks (never
    * THINKING) and turns embedded <tool_call>/dialect calls into TOOL_USE blocks. No-op
    * when the response already has a native tool_use. Runs BEFORE the bridge so the
    * recovered calls flow through the same path as native ones. */
   int rescued = 0;
   if (rescue_mode >= 0)
      rescued = aimee_ir_rescue_tool_calls(&ir, rescue_mode);
   if (n_rescued)
      *n_rescued = rescued;

   /* model, stop reason, usage, TEXT content, and TOOL_USE calls (shared bridge). */
   ir_bridge_common(&ir, out);

   /* assistant_message for multi-turn replay. When a rescue fired the raw response
    * carried the calls as prose, so it cannot be replayed: anthropic drops it (as
    * legacy did), openai rebuilds it from the recovered calls. Otherwise it is taken
    * from the raw response exactly as the legacy parser does. */
   if (rescued > 0)
   {
      if (!anthropic)
         out->assistant_message = agent_build_openai_assistant_message_from_calls(out);
      /* anthropic: leave assistant_message NULL, matching the legacy rescue path */
   }
   else if (anthropic)
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

/* Shared bridge from a parsed IR response into parsed_response_t: model, stop reason,
 * usage, concatenated TEXT content, and TOOL_USE calls. assistant_message is left to
 * the caller (it is wire-specific). */
static void ir_bridge_common(const aimee_response_t *ir, parsed_response_t *out)
{
   if (ir->model)
      snprintf(out->model, sizeof(out->model), "%s", ir->model);
   if (ir->raw_stop_reason)
      snprintf(out->stop_reason, sizeof(out->stop_reason), "%s", ir->raw_stop_reason);
   out->prompt_tokens = (int)ir->usage_in;
   out->completion_tokens = (int)ir->usage_out;
   out->cache_write_tokens = (int)ir->usage_cache_write;
   out->cache_read_tokens = (int)ir->usage_cache_read;

   size_t need = 1;
   for (int i = 0; i < ir->n_content; i++)
      if (ir->content[i].type == AIMEE_BLK_TEXT && ir->content[i].text)
         need += strlen(ir->content[i].text);
   if (need > 1)
   {
      out->content = malloc(need);
      if (out->content)
         aimee_ir_response_text(ir, out->content, need);
   }

   for (int i = 0; i < ir->n_content && out->call_count < AGENT_MAX_TOOL_CALLS; i++)
   {
      const aimee_block_t *b = &ir->content[i];
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
}

int agent_ir_parse_responses(const char *body, int rescue_mode, int *n_rescued,
                             parsed_response_t *out)
{
   memset(out, 0, sizeof(*out));
   if (n_rescued)
      *n_rescued = 0;

   /* The responses wire is SSE, not a single JSON object: extract the response object
    * (output items + usage) the same way the shadow does, then parse it via the IR. */
   cJSON *robj = agent_responses_sse_response_object(body);
   if (!robj)
      return -1;

   aimee_response_t ir;
   memset(&ir, 0, sizeof(ir));
   char err[128] = "";
   if (responses_backend_parse(robj, &ir, err, sizeof err) != 0)
   {
      aimee_response_free(&ir);
      cJSON_Delete(robj);
      return -1;
   }

   int rescued = 0;
   if (rescue_mode >= 0)
      rescued = aimee_ir_rescue_tool_calls(&ir, rescue_mode);
   if (n_rescued)
      *n_rescued = rescued;

   ir_bridge_common(&ir, out);

   /* assistant_message for multi-turn replay is the output-item array itself: the
    * turn loop appends the function_call items (matched by call_id) to the next
    * request's `input`. Matches the legacy parser's collected_output. When a rescue
    * fired, the calls came from prose (not native output items), so it cannot replay
    * -- leave it NULL, as the anthropic rescue path does. */
   if (out->is_tool_call && rescued == 0)
   {
      cJSON *output = cJSON_GetObjectItemCaseSensitive(robj, "output");
      if (output && cJSON_IsArray(output))
         out->assistant_message = cJSON_Duplicate(output, 1);
   }

   aimee_response_free(&ir);
   cJSON_Delete(robj);
   return 0;
}
