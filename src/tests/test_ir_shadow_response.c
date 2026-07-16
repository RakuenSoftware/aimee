/* test_ir_shadow_response.c -- the response-side shadow comparator.
 *
 * aimee_ir_shadow_compare_response parses a provider response through the IR backend
 * parser and records whether it agrees with the legacy translator's
 * parsed_response_t. This is the evidence that must show parity before the response
 * translators can be retired, so the test's job is to prove the comparator actually
 * DISTINGUISHES agreement from each kind of divergence -- not just that it counts a
 * match when handed two identical things. Every mismatch axis is exercised, and each
 * assertion is one the comparator would fail if it stopped checking that axis. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h" /* MAX_PATH_LEN et al: agent_types.h depends on it */

#include "aimee_ir_metrics.h"
#include "aimee_ir_shadow.h"
#include "agent_protocol.h"
#include "cJSON.h"

static cJSON *text_resp(const char *text)
{
   cJSON *r = cJSON_CreateObject();
   cJSON_AddStringToObject(r, "stop_reason", "end_turn");
   cJSON *content = cJSON_AddArrayToObject(r, "content");
   cJSON *blk = cJSON_CreateObject();
   cJSON_AddStringToObject(blk, "type", "text");
   cJSON_AddStringToObject(blk, "text", text);
   cJSON_AddItemToArray(content, blk);
   return r;
}

static cJSON *tool_resp(const char *name, cJSON *input)
{
   cJSON *r = cJSON_CreateObject();
   cJSON_AddStringToObject(r, "stop_reason", "tool_use");
   cJSON *content = cJSON_AddArrayToObject(r, "content");
   cJSON *blk = cJSON_CreateObject();
   cJSON_AddStringToObject(blk, "type", "tool_use");
   cJSON_AddStringToObject(blk, "id", "t1");
   cJSON_AddStringToObject(blk, "name", name);
   cJSON_AddItemToObject(blk, "input", input);
   cJSON_AddItemToArray(content, blk);
   return r;
}

static long mism(void)
{
   return aimee_ir_metric_total(AIMEE_IR_M_RESP_MISMATCH);
}
static long match(void)
{
   return aimee_ir_metric_total(AIMEE_IR_M_RESP_MATCH);
}

int main(void)
{
   /* The comparator is a no-op unless shadow mode is on -- turn it on. */
   setenv("AIMEE_IR_SHADOW", "1", 1);
   printf("ir-shadow-response:\n");

   /* 1. Identical text -> match. */
   {
      aimee_ir_metrics_reset();
      parsed_response_t lg;
      memset(&lg, 0, sizeof(lg));
      lg.content = strdup("hello world");
      cJSON *r = text_resp("hello world");
      aimee_ir_shadow_compare_response(&lg, r, AIMEE_WIRE_ANTHROPIC);
      assert(match() == 1 && mism() == 0);
      cJSON_Delete(r);
      free(lg.content);
      printf("  PASS: identical text counts a match\n");
   }

   /* 2. Different text -> mismatch. If the comparator ignored text, this would be a
    * false match and parity would look proven when it is not. */
   {
      aimee_ir_metrics_reset();
      parsed_response_t lg;
      memset(&lg, 0, sizeof(lg));
      lg.content = strdup("hello world");
      cJSON *r = text_resp("something else entirely");
      aimee_ir_shadow_compare_response(&lg, r, AIMEE_WIRE_ANTHROPIC);
      assert(mism() == 1 && match() == 0);
      cJSON_Delete(r);
      free(lg.content);
      printf("  PASS: divergent text counts a mismatch\n");
   }

   /* 3. Whitespace-only difference is NOT a divergence. */
   {
      aimee_ir_metrics_reset();
      parsed_response_t lg;
      memset(&lg, 0, sizeof(lg));
      lg.content = strdup("  hello world\n");
      cJSON *r = text_resp("hello world");
      aimee_ir_shadow_compare_response(&lg, r, AIMEE_WIRE_ANTHROPIC);
      assert(match() == 1 && mism() == 0);
      cJSON_Delete(r);
      free(lg.content);
      printf("  PASS: trim-only text difference is a match\n");
   }

   /* 4. Same tool call (name + args) -> match. */
   {
      aimee_ir_metrics_reset();
      parsed_response_t lg;
      memset(&lg, 0, sizeof(lg));
      lg.is_tool_call = 1;
      lg.call_count = 1;
      strncpy(lg.calls[0].name, "bash", sizeof(lg.calls[0].name) - 1);
      lg.calls[0].arguments = strdup("{\"cmd\":\"ls\"}");
      cJSON *input = cJSON_CreateObject();
      cJSON_AddStringToObject(input, "cmd", "ls");
      cJSON *r = tool_resp("bash", input);
      aimee_ir_shadow_compare_response(&lg, r, AIMEE_WIRE_ANTHROPIC);
      assert(match() == 1 && mism() == 0);
      cJSON_Delete(r);
      free(lg.calls[0].arguments);
      printf("  PASS: identical tool call (name+args) counts a match\n");
   }

   /* 5. Same shape but different tool NAME -> mismatch. */
   {
      aimee_ir_metrics_reset();
      parsed_response_t lg;
      memset(&lg, 0, sizeof(lg));
      lg.is_tool_call = 1;
      lg.call_count = 1;
      strncpy(lg.calls[0].name, "grep", sizeof(lg.calls[0].name) - 1);
      lg.calls[0].arguments = strdup("{\"cmd\":\"ls\"}");
      cJSON *input = cJSON_CreateObject();
      cJSON_AddStringToObject(input, "cmd", "ls");
      cJSON *r = tool_resp("bash", input);
      aimee_ir_shadow_compare_response(&lg, r, AIMEE_WIRE_ANTHROPIC);
      assert(mism() == 1 && match() == 0);
      cJSON_Delete(r);
      free(lg.calls[0].arguments);
      printf("  PASS: differing tool name counts a mismatch\n");
   }

   /* 6. Same name, different ARGS -> mismatch (semantic, not string). */
   {
      aimee_ir_metrics_reset();
      parsed_response_t lg;
      memset(&lg, 0, sizeof(lg));
      lg.is_tool_call = 1;
      lg.call_count = 1;
      strncpy(lg.calls[0].name, "bash", sizeof(lg.calls[0].name) - 1);
      lg.calls[0].arguments = strdup("{\"cmd\":\"rm -rf /\"}");
      cJSON *input = cJSON_CreateObject();
      cJSON_AddStringToObject(input, "cmd", "ls");
      cJSON *r = tool_resp("bash", input);
      aimee_ir_shadow_compare_response(&lg, r, AIMEE_WIRE_ANTHROPIC);
      assert(mism() == 1 && match() == 0);
      cJSON_Delete(r);
      free(lg.calls[0].arguments);
      printf("  PASS: differing tool arguments counts a mismatch\n");
   }

   /* 7. Key ORDER in args must NOT read as a divergence (semantic compare). */
   {
      aimee_ir_metrics_reset();
      parsed_response_t lg;
      memset(&lg, 0, sizeof(lg));
      lg.is_tool_call = 1;
      lg.call_count = 1;
      strncpy(lg.calls[0].name, "bash", sizeof(lg.calls[0].name) - 1);
      lg.calls[0].arguments = strdup("{\"a\":1,\"b\":2}");
      cJSON *input = cJSON_CreateObject();
      cJSON_AddNumberToObject(input, "b", 2);
      cJSON_AddNumberToObject(input, "a", 1);
      cJSON *r = tool_resp("bash", input);
      aimee_ir_shadow_compare_response(&lg, r, AIMEE_WIRE_ANTHROPIC);
      assert(match() == 1 && mism() == 0);
      cJSON_Delete(r);
      free(lg.calls[0].arguments);
      printf("  PASS: argument key order is not a divergence\n");
   }

   /* 8. text-vs-tool disagreement -> mismatch: legacy says tool, IR sees text. */
   {
      aimee_ir_metrics_reset();
      parsed_response_t lg;
      memset(&lg, 0, sizeof(lg));
      lg.is_tool_call = 1;
      lg.call_count = 1;
      strncpy(lg.calls[0].name, "bash", sizeof(lg.calls[0].name) - 1);
      lg.calls[0].arguments = strdup("{}");
      cJSON *r = text_resp("no tool here");
      aimee_ir_shadow_compare_response(&lg, r, AIMEE_WIRE_ANTHROPIC);
      assert(mism() == 1 && match() == 0);
      cJSON_Delete(r);
      free(lg.calls[0].arguments);
      printf("  PASS: tool-vs-text disagreement counts a mismatch\n");
   }

   /* 9. Disabled by default: with the flag off, the comparator records nothing. */
   {
      unsetenv("AIMEE_IR_SHADOW");
      aimee_ir_metrics_reset();
      parsed_response_t lg;
      memset(&lg, 0, sizeof(lg));
      lg.content = strdup("hello");
      cJSON *r = text_resp("hello");
      aimee_ir_shadow_compare_response(&lg, r, AIMEE_WIRE_ANTHROPIC);
      assert(match() == 0 && mism() == 0);
      cJSON_Delete(r);
      free(lg.content);
      setenv("AIMEE_IR_SHADOW", "1", 1);
      printf("  PASS: off unless AIMEE_IR_SHADOW is set\n");
   }

   printf("ir-shadow-response: ok\n");
   return 0;
}
