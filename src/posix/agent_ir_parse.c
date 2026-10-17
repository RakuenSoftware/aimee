/* Provider entry adapters return owned canonical IR. Legacy wrappers remain
 * for callers still migrating; the delegate runtime never uses their projection.
 * Tool-call rescue belongs to this entry boundary and runs once on typed TEXT. */
#include "aimee.h"

#include "agent_protocol.h"
#include <aimee/translation/aimee_backend.h>
#include <aimee/translation/aimee_frontend.h>
#include <aimee/ir/aimee_ir.h>
#include <aimee/ir/session_event.h>
#include <aimee/ir/aimee_ir_metrics.h>
#include <aimee/delegates/aimee_ir_rescue.h>
#include "tool_call_args.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

/* Build an OpenAI assistant message (role + null content + tool_calls) from the
 * parsed calls. Used to rebuild assistant_message after an XML rescue, where the
 * raw response had the calls as prose and cannot serve as the replay turn. Used only by the legacy
 * projection wrapper while its callers migrate. */
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

/* Compatibility wrappers parse through owned IR and project for legacy consumers.
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
 * Returns 0 on success; -1 if the response cannot be admitted as IR. */
static void ir_bridge_common(const aimee_response_t *ir, parsed_response_t *out);

static void ir_observe_reasoning(const aimee_response_t *ir);

static int response_admit(aimee_response_t *response)
{
   if (response->n_content < 0 || response->n_content > AIMEE_IR_MAX_BLOCKS)
      return -1;
   if (response->role && strcmp(response->role, "assistant"))
      return -1;
   if (response->usage_in < 0 || response->usage_out < 0 || response->usage_cache_read < 0 ||
       response->usage_cache_write < 0 || response->usage_reasoning < 0)
      return -1;
   for (int i = 0; i < response->n_content; i++)
   {
      const aimee_block_t *block = &response->content[i];
      if (!aimee_ir_block_valid(block) || block->type == AIMEE_BLK_TOOL_RESULT)
         return -1;
      if (block->type == AIMEE_BLK_TOOL_USE && !cJSON_IsObject(block->tool_input))
         return -1;
      if (block->type == AIMEE_BLK_TOOL_USE)
         for (int j = 0; j < i; j++)
            if (response->content[j].type == AIMEE_BLK_TOOL_USE &&
                !strcmp(block->tool_id, response->content[j].tool_id))
               return -1;
   }
   aimee_message_t message = {.role = response->role ? response->role : "assistant",
                              .blocks = response->content,
                              .n_blocks = response->n_content};
   aimee_request_t request = {.messages = &message, .n_messages = 1};
   aimee_ir_classify_context(&request);
   return 0;
}

int agent_ir_decode_json_response(const cJSON *root, int anthropic, int rescue_mode, int *n_rescued,
                                  aimee_response_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   if (n_rescued)
      *n_rescued = 0;
   char err[128] = "";
   int rc = anthropic ? anthropic_backend_parse(root, out, err, sizeof(err))
                      : openai_backend_parse(root, out, err, sizeof(err));
   if (rc != 0)
      goto fail;
   int rescued = rescue_mode >= 0 ? aimee_ir_rescue_tool_calls(out, rescue_mode) : 0;
   if (rescued < 0 || response_admit(out) != 0)
      goto fail;
   if (n_rescued)
      *n_rescued = rescued;
   ir_observe_reasoning(out);
   if (aimee_ir_session_emit_buffered_response(out, NULL) != 0)
   {
      aimee_response_free(out);
      return AGENT_IR_CAPTURE_FAILED;
   }
   return 0;
fail:
   aimee_response_free(out);
   aimee_ir_session_fail_capture();
   return -1;
}

int agent_ir_parse_json_response(cJSON *root, int anthropic, int rescue_mode, int *n_rescued,
                                 parsed_response_t *out)
{
   memset(out, 0, sizeof(*out));
   aimee_response_t ir = {0};
   int rescued = 0;
   if (agent_ir_decode_json_response(root, anthropic, rescue_mode, &rescued, &ir) != 0)
      return -1;
   if (n_rescued)
      *n_rescued = rescued;
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
/* Cap matching the relay tap's: reasoning is only ever MATCHED against, never
 * replayed, so a bound costs recall of late thought where an unbounded copy would
 * scale with the model's chain of thought. */
#define IR_REASONING_OBSERVE_MAX 16384

/* Observation-only reasoning tap for the BUFFERED wires, the counterpart of the
 * streaming relay's. It sits at the shared response entry boundary every
 * JSON-wire response parse passes through -- anthropic, openai-chat and responses
 * alike -- so one call covers all three without naming a provider. The rule is the
 * same neutral one the streaming tap uses: read AIMEE_BLK_THINKING off the IR.
 *
 * Execution and persistence retain the reasoning blocks. This tap only observes
 * them; it never changes or substitutes for the canonical content.
 *
 * Truncation is FLAGGED, and a reasoning block whose text did not survive parsing
 * (an empty thought) counts as incomplete rather than observed -- a block that is
 * present but empty must never be mistaken for a thought something can act on. */
static void ir_observe_reasoning(const aimee_response_t *ir)
{
   int saw_block = 0;
   for (int i = 0; i < ir->n_content; i++)
      if (ir->content[i].type == AIMEE_BLK_THINKING)
         saw_block = 1;
   if (!saw_block)
      return;

   char buf[IR_REASONING_OBSERVE_MAX];
   size_t n = aimee_ir_response_reasoning(ir, buf, sizeof buf);
   if (n > 0)
      aimee_ir_metric_inc(AIMEE_IR_M_REASONING_OBSERVED, AIMEE_WIRE_UNKNOWN);
   /* n == 0 with a THINKING block present means the block carried no text: the
    * reasoning was dropped somewhere upstream of here, which is exactly the failure
    * that must not read as "no reasoning in this turn". */
   if (n == 0 || n >= sizeof buf - 1)
      aimee_ir_metric_inc(AIMEE_IR_M_REASONING_INCOMPLETE, AIMEE_WIRE_UNKNOWN);
}

static void ir_bridge_common(const aimee_response_t *ir, parsed_response_t *out)
{
   if (ir->model)
      snprintf(out->model, sizeof(out->model), "%s", ir->model);
   if (ir->raw_stop_reason)
      snprintf(out->stop_reason, sizeof(out->stop_reason), "%s", ir->raw_stop_reason);
   out->stop = ir->stop_reason;
   out->prompt_tokens = (int)ir->usage_in;
   out->completion_tokens = (int)ir->usage_out;
   out->cache_write_tokens = (int)ir->usage_cache_write;
   out->cache_read_tokens = (int)ir->usage_cache_read;

   out->content = aimee_ir_response_text_alloc(ir);
   if (out->content && !out->content[0])
   {
      free(out->content);
      out->content = NULL;
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
      /* Carried with the name: a namespaced call is only routable as the pair. */
      if (b->tool_namespace)
         snprintf(c->tool_namespace, sizeof(c->tool_namespace), "%s", b->tool_namespace);
      char *args = b->tool_input ? cJSON_PrintUnformatted(b->tool_input) : NULL;
      c->arguments = args ? args : strdup("{}");
   }
}

int agent_ir_decode_responses(const char *body, int rescue_mode, int *n_rescued,
                              aimee_response_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   if (n_rescued)
      *n_rescued = 0;
   cJSON *object = responses_backend_sse_object(body);
   if (!object)
      goto fail;
   char error[128];
   int rc = responses_backend_parse(object, out, error, sizeof(error));
   cJSON_Delete(object);
   if (rc != 0)
      goto fail;
   int rescued = rescue_mode >= 0 ? aimee_ir_rescue_tool_calls(out, rescue_mode) : 0;
   if (rescued < 0 || response_admit(out) != 0)
      goto fail;
   if (n_rescued)
      *n_rescued = rescued;
   ir_observe_reasoning(out);
   if (aimee_ir_session_emit_buffered_response(out, NULL) != 0)
   {
      aimee_response_free(out);
      return AGENT_IR_CAPTURE_FAILED;
   }
   return 0;
fail:
   aimee_response_free(out);
   aimee_ir_session_fail_capture();
   return -1;
}

int agent_ir_parse_responses(const char *body, int rescue_mode, int *n_rescued,
                             parsed_response_t *out)
{
   memset(out, 0, sizeof(*out));
   aimee_response_t ir = {0};
   int rescued = 0;
   if (agent_ir_decode_responses(body, rescue_mode, &rescued, &ir) != 0)
      return -1;
   if (n_rescued)
      *n_rescued = rescued;
   ir_bridge_common(&ir, out);
   cJSON *robj = responses_backend_sse_object(body);
   if (!robj)
   {
      aimee_response_free(&ir);
      return -1;
   }

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
