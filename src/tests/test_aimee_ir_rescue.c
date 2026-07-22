/* test_aimee_ir_rescue.c -- the IR-side tool-call rescue.
 *
 * The point of the IR rescue is not "it also parses XML" -- delegate_xml_fallback
 * already did that. It is that the IR knows which blocks are REASONING, so it cannot
 * rescue a tool call the model was only thinking about. The legacy path had to strip
 * reasoning out of a flat string to approximate this. That test is here. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee_ir_metrics.h"
#include "aimee_ir_rescue.h"
#include "cJSON.h"

/* The dialect parser consults the tool registry only on its bare-JSON/bracket rescue
 * paths, to gate unknown tool names. These cases use the explicit <tool_call> form,
 * which accepts any name, so a NULL stub keeps the test self-contained -- same shape
 * as test_delegate_xml_fallback.c. */
struct cJSON *agent_tool_get_schema_cached(const char *tool_name)
{
   (void)tool_name;
   return NULL;
}

static aimee_response_t *resp_of(aimee_block_type_t t, const char *text)
{
   aimee_response_t *r = calloc(1, sizeof(*r));
   r->content = calloc(1, sizeof(*r->content));
   r->n_content = 1;
   r->content[0].type = t;
   r->content[0].text = strdup(text);
   r->stop_reason = AIMEE_STOP_END_TURN;
   return r;
}

#define XMLCALL                                                                                    \
   "<tool_call><name>bash</name><arguments>{\"command\":\"ls\"}</arguments></tool_call>"

int main(void)
{
   printf("aimee-ir-rescue:\n");

   /* 1. A prose XML tool call in a TEXT block becomes a real TOOL_USE block. */
   {
      aimee_ir_metrics_reset();
      aimee_response_t *r = resp_of(AIMEE_BLK_TEXT, "sure, running it\n" XMLCALL);
      int n = aimee_ir_rescue_tool_calls(r, 0);
      assert(n == 1);
      assert(aimee_ir_response_has_tool_use(r));
      /* the leading prose survives as text, the call becomes its own block */
      assert(r->n_content == 2);
      assert(r->content[0].type == AIMEE_BLK_TEXT);
      assert(strstr(r->content[0].text, "sure"));
      assert(r->content[1].type == AIMEE_BLK_TOOL_USE);
      assert(strcmp(r->content[1].tool_name, "bash") == 0);
      cJSON *cmd = cJSON_GetObjectItem(r->content[1].tool_input, "command");
      assert(cmd && strcmp(cmd->valuestring, "ls") == 0);
      /* a rescued call IS a tool turn -- if the stop reason still says end_turn the
       * caller answers the user instead of dispatching the tool */
      assert(r->stop_reason == AIMEE_STOP_TOOL_USE);
      assert(aimee_ir_metric_total(AIMEE_IR_M_RESCUE_RECOVERIES) == 1);
      aimee_response_free(r);
      printf("  PASS: prose XML call in TEXT -> TOOL_USE, stop_reason corrected\n");
   }

   /* 2. THE STRUCTURAL GUARANTEE: the same bytes inside a THINKING block are NOT a
    * call. The model reasoning "I could run <tool_call>...</tool_call>" must never be
    * dispatched. The IR gets this from the block type, not from a text heuristic. */
   {
      aimee_ir_metrics_reset();
      aimee_response_t *r = resp_of(AIMEE_BLK_THINKING, "maybe I should run " XMLCALL);
      int n = aimee_ir_rescue_tool_calls(r, 0);
      assert(n == 0);
      assert(!aimee_ir_response_has_tool_use(r));
      assert(r->n_content == 1 && r->content[0].type == AIMEE_BLK_THINKING);
      assert(r->stop_reason == AIMEE_STOP_END_TURN);
      assert(aimee_ir_metric_total(AIMEE_IR_M_RESCUE_RECOVERIES) == 0);
      aimee_response_free(r);
      printf("  PASS: identical call text in THINKING is never rescued\n");
   }

   /* 3. Native tool calling already won -- rescuing on top would double-dispatch. */
   {
      aimee_response_t *r = calloc(1, sizeof(*r));
      r->content = calloc(2, sizeof(*r->content));
      r->n_content = 2;
      r->content[0].type = AIMEE_BLK_TOOL_USE;
      r->content[0].tool_id = strdup("t1");
      r->content[0].tool_name = strdup("grep");
      r->content[0].tool_input = cJSON_CreateObject();
      r->content[1].type = AIMEE_BLK_TEXT;
      r->content[1].text = strdup(XMLCALL);
      int n = aimee_ir_rescue_tool_calls(r, 0);
      assert(n == 0);
      assert(r->n_content == 2);
      aimee_response_free(r);
      printf("  PASS: no rescue when a native TOOL_USE is already present\n");
   }

   /* 4. Plain prose is left exactly alone. */
   {
      aimee_response_t *r = resp_of(AIMEE_BLK_TEXT, "just an ordinary answer");
      int n = aimee_ir_rescue_tool_calls(r, 0);
      assert(n == 0);
      assert(r->n_content == 1 && strcmp(r->content[0].text, "just an ordinary answer") == 0);
      assert(r->stop_reason == AIMEE_STOP_END_TURN);
      aimee_response_free(r);
      printf("  PASS: prose without a call is untouched\n");
   }

   /* 5. Degenerate input must not crash. */
   assert(aimee_ir_rescue_tool_calls(NULL, 0) == 0);
   printf("  PASS: NULL response is a no-op\n");

   printf("aimee-ir-rescue: ok\n");
   return 0;
}
