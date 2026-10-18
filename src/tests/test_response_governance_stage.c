/* test_response_governance_stage.c -- production governance response seam.
 * Proves that enabled decisions use the registered event-bus bridge, disabled
 * decisions do not call it, and bridge failures deny pending tool calls rather
 * than falling back to local policy evaluation. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h"
#include <aimee/ir/aimee_ir.h>
#include "gw_stage_governance.h"
#include "cJSON.h"
#include "agent_response_projection.h"
#include <aimee/ir/session_event.h>

static int g_provider_calls;
static int g_policy_active;
static int g_provider_mode;
static const char *expected_policy_stop;

static int denied_tool(const char *name)
{
   return strcmp(name, "Agent") == 0 || strcmp(name, "spawn_agent") == 0 ||
          strcmp(name, "RemoteTrigger") == 0 || strcmp(name, "Task") == 0;
}

static int fake_event_bus_provider(int policy_active, const char *const *tool_names,
                                   uint32_t tool_count, const char *stop_reason,
                                   aimee_governance_decision_t *decision)
{
   g_provider_calls++;
   if (expected_policy_stop)
      assert(stop_reason && !strcmp(stop_reason, expected_policy_stop));
   g_policy_active = policy_active;
   if (g_provider_mode == 1)
      return -1;
   memset(decision, 0, sizeof(*decision));
   if (g_provider_mode == 2)
   {
      decision->keep_mask = UINT32_MAX;
      return 0;
   }

   decision->keep_mask = aimee_governance_mask_for_count(tool_count);
   snprintf(decision->stop_reason, sizeof(decision->stop_reason), "%s",
            stop_reason ? stop_reason : "");
   if (!policy_active)
      return 0;

   decision->keep_mask = 0;
   for (uint32_t i = 0; i < tool_count; ++i)
   {
      if (denied_tool(tool_names[i]))
         decision->drop_count++;
      else
         decision->keep_mask |= 1u << i;
   }
   uint32_t kept = tool_count - decision->drop_count;
   if (!decision->stop_reason[0] || kept == 0)
      snprintf(decision->stop_reason, sizeof(decision->stop_reason), "%s",
               kept > 0 ? "tool_use" : "end_turn");
   return 0;
}

static char *copy_string(const char *value)
{
   size_t len = strlen(value) + 1;
   char *copy = malloc(len);
   assert(copy != NULL);
   memcpy(copy, value, len);
   return copy;
}

static aimee_response_t tool_response(int count, const char *first, const char *other)
{
   aimee_response_t response = {0};
   response.n_content = count;
   response.content = calloc((size_t)count, sizeof *response.content);
   assert(response.content);
   for (int i = 0; i < count; i++)
   {
      char id[32];
      snprintf(id, sizeof id, "opaque-%d", i);
      response.content[i] = (aimee_block_t){.type = AIMEE_BLK_TOOL_USE,
                                            .tool_name = copy_string(i ? other : first),
                                            .tool_id = copy_string(id),
                                            .tool_input = cJSON_CreateObject()};
   }
   response.stop_reason = AIMEE_STOP_TOOL_USE;
   return response;
}

static void test_native_stop_metadata_survives_policy(void)
{
   const aimee_stop_reason_t stops[] = {AIMEE_STOP_TOOL_USE, AIMEE_STOP_MAX_TOKENS,
                                        AIMEE_STOP_STOP_SEQUENCE, AIMEE_STOP_UNKNOWN};
   char reason[301];
   memset(reason, 'r', sizeof reason - 1);
   memcpy(reason, "native-reason:", 14);
   reason[sizeof reason - 1] = 0;
   gw_response_governance_register_provider(fake_event_bus_provider);
   g_provider_mode = 0;
   for (size_t i = 0; i < sizeof stops / sizeof stops[0]; i++)
      for (int partial = 0; partial <= 1; partial++)
      {
         aimee_response_t response = tool_response(partial ? 2 : 1, "Agent", "read_file");
         response.stop_reason = stops[i];
         expected_policy_stop = aimee_stop_reason_name(stops[i]);
         response.raw_stop_reason = copy_string(reason);
         assert(gw_ir_response_run_governance(&response, 1, 1) == 1);
         if (!partial && stops[i] == AIMEE_STOP_TOOL_USE)
            assert(response.stop_reason == AIMEE_STOP_END_TURN && !response.raw_stop_reason);
         else
            assert(response.stop_reason == stops[i] && response.raw_stop_reason &&
                   !strcmp(response.raw_stop_reason, reason));
         aimee_response_free(&response);
      }
   expected_policy_stop = NULL;
}

static int denied_events, reject_event;
static unsigned denied_ids;
static int capture_denial(const aimee_session_event_t *event, void *context)
{
   (void)context;
   assert(event->kind == AIMEE_SESSION_MESSAGE && event->message->n_blocks == 1);
   const aimee_block_t *block = &event->message->blocks[0];
   assert(block->type == AIMEE_BLK_TOOL_RESULT && block->tool_is_error);
   int index = -1;
   assert(sscanf(block->tool_id, "opaque-%d", &index) == 1 && index >= 0 && index <= 16);
   denied_ids |= 1u << (unsigned)index;
   denied_events++;
   return reject_event ? -1 : 0;
}

int main(void)
{
   test_native_stop_metadata_survives_policy();
   /* The stage and separately supervised process must share the descriptor's
    * default-OFF posture. Otherwise an unspecified config enables this stage
    * without starting the module, and the fail-closed provider path drops every
    * ordinary tool call. Legacy env activation remains an explicit opt-in. */
   unsetenv("AIMEE_STAGE_GOVERNANCE");
   assert(gw_response_governance_enabled() == 0);
   setenv("AIMEE_STAGE_GOVERNANCE", "0", 1);
   assert(gw_response_governance_enabled() == 0);
   setenv("AIMEE_STAGE_GOVERNANCE", "off", 1);
   assert(gw_response_governance_enabled() == 0);
   setenv("AIMEE_STAGE_GOVERNANCE", "false", 1);
   assert(gw_response_governance_enabled() == 0);
   setenv("AIMEE_STAGE_GOVERNANCE", "1", 1);
   assert(gw_response_governance_enabled() == 1);
   setenv("AIMEE_STAGE_GOVERNANCE", "yes", 1);
   assert(gw_response_governance_enabled() == 1);
   setenv("AIMEE_STAGE_GOVERNANCE", "nope", 1);
   assert(gw_response_governance_enabled() == 0); /* invalid values do not opt in */

   /* Default/disabled governance does not call an absent owner or mutate IR. */
   unsetenv("AIMEE_STAGE_GOVERNANCE");
   gw_response_governance_register_provider(NULL);
   aimee_response_t plain = tool_response(1, "read_file", "read_file");
   assert(!gw_ir_response_run_governance(&plain, gw_response_governance_enabled(), 1));
   assert(aimee_ir_response_tool_count(&plain) == 1);
   assert(!strcmp(plain.content[0].tool_name, "read_file"));
   aimee_response_free(&plain);
   gw_response_governance_register_provider(fake_event_bus_provider);
   for (int enabled = 0; enabled <= 1; enabled++)
      for (int policy = 0; policy <= 1; policy++)
      {
         aimee_response_t response = tool_response(2, "Agent", "read_file");
         g_provider_calls = g_provider_mode = 0;
         assert(gw_ir_response_run_governance(&response, enabled, policy) == (enabled && policy));
         assert(g_provider_calls == enabled);
         if (enabled)
            assert(g_policy_active == policy);
         assert(aimee_ir_response_tool_count(&response) == 2 - (enabled && policy));
         aimee_response_free(&response);
      }
   /* Missing owner fails closed across multiple batches, beyond the old mask width. */
   gw_response_governance_register_provider(NULL);
   plain = tool_response(40, "read_file", "read_file");
   assert(gw_ir_response_run_governance(&plain, 1, 1) == 40);
   assert(!aimee_ir_response_tool_count(&plain) && plain.stop_reason == AIMEE_STOP_END_TURN);
   aimee_response_free(&plain);

   /* Typed execution uses the same decision owner without flattening reasoning,
    * media or long IDs. Failed decisions remove tools, preserving other blocks. */
   gw_response_governance_register_provider(fake_event_bus_provider);
   for (int failed = 0; failed < 3; failed++)
   {
      aimee_response_t response = {0};
      response.n_content = 4;
      response.content = calloc(4, sizeof *response.content);
      assert(response.content);
      response.content[0] = (aimee_block_t){.type = AIMEE_BLK_THINKING,
                                            .text = copy_string("reason"),
                                            .thinking_signature = copy_string("signature")};
      response.content[1] = (aimee_block_t){.type = AIMEE_BLK_TOOL_USE,
                                            .tool_name = copy_string("Agent"),
                                            .tool_id = copy_string("deny")};
      response.content[2] = (aimee_block_t){.type = AIMEE_BLK_IMAGE,
                                            .media_type = copy_string("image/png"),
                                            .media_ref = copy_string("AA==")};
      response.content[3] = (aimee_block_t){.type = AIMEE_BLK_TOOL_USE,
                                            .tool_name = copy_string("read_file"),
                                            .tool_id = copy_string("keep:opaque")};
      response.stop_reason = AIMEE_STOP_TOOL_USE;
      response.raw = cJSON_CreateObject();
      g_provider_mode = failed;
      g_provider_calls = 0;
      assert(gw_ir_response_run_governance(&response, 0, 1) == 0);
      assert(g_provider_calls == 0 && response.n_content == 4);
      assert(gw_ir_response_run_governance(&response, 1, 1) == (failed ? 2 : 1));
      assert(g_provider_calls == 1);
      assert(response.n_content == (failed ? 2 : 3));
      assert(!strcmp(response.content[0].thinking_signature, "signature"));
      assert(response.content[1].type == AIMEE_BLK_IMAGE);
      assert(response.raw == NULL);
      assert(response.stop_reason == (failed ? AIMEE_STOP_END_TURN : AIMEE_STOP_TOOL_USE));
      if (!failed)
         assert(!strcmp(aimee_ir_response_tool(&response, 0)->tool_id, "keep:opaque"));
      aimee_response_free(&response);
   }

   /* The old mask width must not limit the canonical response. Each policy
    * batch decides real tool names; truncation remains visible after policing. */
   {
      aimee_response_t response = {0};
      response.n_content = 18;
      response.content = calloc(18, sizeof *response.content);
      assert(response.content);
      response.content[0] = (aimee_block_t){.type = AIMEE_BLK_THINKING,
                                            .text = copy_string("reason"),
                                            .thinking_signature = copy_string("sig")};
      for (int i = 0; i < 17; i++)
      {
         char id[32];
         snprintf(id, sizeof id, "opaque-%d", i);
         response.content[i + 1] =
             (aimee_block_t){.type = AIMEE_BLK_TOOL_USE,
                             .tool_name = copy_string(i % 2 ? "read_file" : "Agent"),
                             .tool_id = copy_string(id)};
      }
      response.stop_reason = AIMEE_STOP_MAX_TOKENS;
      g_provider_calls = g_provider_mode = 0;
      aimee_ir_session_set_sink(capture_denial, NULL);
      char first_denied[64] = "";
      assert(agent_response_govern(&response, 1, 1, first_denied, sizeof first_denied) == 9);
      assert(denied_events == 9 && denied_ids == 0x15555 && !strcmp(first_denied, "Agent"));
      aimee_ir_session_set_sink(NULL, NULL);
      assert(g_provider_calls == 2 && aimee_ir_response_tool_count(&response) == 8);
      assert(response.stop_reason == AIMEE_STOP_MAX_TOKENS);
      assert(!strcmp(response.content[0].thinking_signature, "sig"));
      aimee_response_free(&response);
   }

   {
      aimee_response_t response = {0};
      response.content = calloc(1, sizeof *response.content);
      assert(response.content);
      response.n_content = 1;
      response.content[0] = (aimee_block_t){.type = AIMEE_BLK_TOOL_USE,
                                            .tool_id = copy_string("opaque-0"),
                                            .tool_name = copy_string("Agent")};
      reject_event = 1;
      aimee_ir_session_set_sink(capture_denial, NULL);
      assert(agent_response_govern(&response, 1, 1, NULL, 0) < 0);
      assert(aimee_ir_session_capture_failed());
      aimee_ir_session_set_sink(NULL, NULL);
      aimee_response_free(&response);
   }
   /* NULL-safe. */
   assert(gw_ir_response_run_governance(NULL, 1, 1) == 0);

   printf("ok\n");
   return 0;
}
