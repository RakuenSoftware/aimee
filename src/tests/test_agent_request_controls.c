/* Guarded execution owns its system/catalog. Unsupported typed controls must
 * refuse before the adapter rebuilds a profile from plain system text. */
#include "agent_exec.h"
#include <aimee/translation/aimee_frontend.h>
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void typed_controls(void)
{
   aimee_block_t task[] = {{.type = AIMEE_BLK_TEXT, .text = "original task"},
                           {.type = AIMEE_BLK_TEXT, .text = "retrieved evidence"}};
   aimee_block_t answer = {.type = AIMEE_BLK_TEXT, .text = "prior model response"};
   aimee_block_t repair = {.type = AIMEE_BLK_TEXT, .text = "repair it"};
   aimee_message_t messages[] = {{.role = "user", .blocks = task, .n_blocks = 2},
                                 {.role = "assistant", .blocks = &answer, .n_blocks = 1},
                                 {.role = "user", .blocks = &repair, .n_blocks = 1}};
   aimee_block_t policy[] = {{.type = AIMEE_BLK_TEXT, .text = "first policy"},
                             {.type = AIMEE_BLK_TEXT, .text = "second policy"}};
   aimee_request_t request = {.messages = messages,
                              .n_messages = 3,
                              .system = policy,
                              .n_system = 2,
                              .has_max_tokens = 1,
                              .max_tokens = 64,
                              .has_temperature = 1,
                              .temperature = 0.25,
                              .has_top_p = 1,
                              .top_p = 0.9,
                              .cache_policy = AIMEE_CACHE_STABLE_PREFIX,
                              .metadata = cJSON_Parse("{\"exact\":9007199254740993123456789}")};
   aimee_ir_block_set_context(&task[1], AIMEE_CTX_ORIGIN_RETRIEVAL, AIMEE_CTX_AUTH_EVIDENCE,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, "source",
                              "revision", 17);
   cJSON *before = aimee_ir_request_json(&request);
   assert(before && agent_request_with_tools_supported(&request));
   cJSON *after = aimee_ir_request_json(&request);
   char *a = cJSON_PrintUnformatted(before), *b = cJSON_PrintUnformatted(after);
   assert(a && b && !strcmp(a, b));
   free(a);
   free(b);
   cJSON_Delete(before);
   cJSON_Delete(after);
   request.stream = 1;
   assert(!agent_request_with_tools_supported(&request));
   request.stream = 0;
   aimee_tool_t custom = {.name = "unregistered", .schema = cJSON_CreateObject()};
   request.tools = &custom;
   request.n_tools = 1;
   assert(agent_request_with_tools_supported(&request));
   request.tools = NULL;
   request.n_tools = 0;
   request.max_tokens = 0;
   assert(!agent_request_with_tools_supported(&request));
   cJSON_Delete(custom.schema);
   cJSON_Delete(request.metadata);
}
static void wire_controls(void)
{
   const char *wires[] = {
       "{\"model\":\"m\",\"max_tokens\":64,\"system\":\"plain "
       "policy\",\"messages\":[{\"role\":\"user\",\"content\":\"task\"}]}",
       "{\"model\":\"m\",\"max_tokens\":64,\"system\":[{\"type\":\"text\",\"text\":\"cached "
       "policy\",\"cache_control\":{\"type\":\"ephemeral\",\"ttl\":\"1h\"}}],\"messages\":[{"
       "\"role\":\"user\",\"content\":\"task\"}]}"};
   for (int i = 0; i < 2; i++)
   {
      cJSON *json = cJSON_Parse(wires[i]);
      aimee_request_t request = {0};
      char error[256];
      assert(json && !anthropic_frontend_parse(json, &request, error, sizeof error));
      assert(!aimee_ir_request_admit(&request));
      if (i)
         assert(request.system[0].cache_control && !strcmp(request.system[0].cache_ttl, "1h"));
      assert(agent_request_with_tools_supported(&request));
      assert(!strcmp(request.system[0].text, i ? "cached policy" : "plain policy"));
      aimee_request_free(&request);
      cJSON_Delete(json);
   }
}
int main(void)
{
   typed_controls();
   wire_controls();
   puts("typed execution request controls: PASS");
   return 0;
}
