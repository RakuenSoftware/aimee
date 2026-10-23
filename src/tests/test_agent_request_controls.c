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
   aimee_block_t user = {.type = AIMEE_BLK_TEXT, .text = "task"};
   aimee_message_t message = {.role = "user", .blocks = &user, .n_blocks = 1};
   aimee_block_t system = {.type = AIMEE_BLK_TEXT, .text = "owned policy"};
   aimee_request_t request = {.messages = &message,
                              .n_messages = 1,
                              .system = &system,
                              .n_system = 1,
                              .has_max_tokens = 1,
                              .max_tokens = 64,
                              .has_temperature = 1,
                              .temperature = 0.25};
   assert(agent_request_with_tools_supported(&request));
   request.cache_policy = AIMEE_CACHE_STABLE_PREFIX;
   assert(!agent_request_with_tools_supported(&request));
   request.cache_policy = AIMEE_CACHE_DEFAULT;
   request.output_format.kind = AIMEE_OUTPUT_TEXT;
   assert(agent_request_with_tools_supported(&request));
   request.output_format.name = "format name";
   assert(!agent_request_with_tools_supported(&request));
   request.output_format.name = NULL;
   request.output_format.description = "format description";
   assert(!agent_request_with_tools_supported(&request));
   request.output_format.description = NULL;
   request.output_format.schema = cJSON_CreateObject();
   assert(!agent_request_with_tools_supported(&request));
   cJSON_Delete(request.output_format.schema);
   request.output_format.schema = NULL;
   request.output_format.strict = AIMEE_STRICT_NULL;
   assert(!agent_request_with_tools_supported(&request));
   request.output_format.strict = AIMEE_STRICT_UNSET;
   request.output_format.kind = AIMEE_OUTPUT_UNSET;
   request.output_format.name = "still unsupported when kind unset";
   assert(!agent_request_with_tools_supported(&request));
   request.output_format.name = NULL;
   char **unrepresented[] = {
       &system.cache_control,     &system.cache_ttl,   &system.thinking_signature,
       &system.thinking_binding,  &system.thinking_id, &system.thinking_encrypted_content,
       &system.thinking_redacted, &system.tool_id,     &system.tool_name,
       &system.tool_namespace,    &system.media_type,  &system.media_ref};
   for (size_t i = 0; i < sizeof unrepresented / sizeof *unrepresented; i++)
   {
      *unrepresented[i] = "unsupported";
      assert(!agent_request_with_tools_supported(&request));
      *unrepresented[i] = NULL;
   }
   cJSON **opaque[] = {&system.raw, &system.tool_input, &system.tool_result};
   for (size_t i = 0; i < sizeof opaque / sizeof *opaque; i++)
   {
      *opaque[i] = cJSON_CreateObject();
      assert(!agent_request_with_tools_supported(&request));
      cJSON_Delete(*opaque[i]);
      *opaque[i] = NULL;
   }
   system.tool_is_error = 1;
   assert(!agent_request_with_tools_supported(&request));
   system.tool_is_error = 0;
   system.context = (aimee_context_meta_t){.origin = AIMEE_CTX_ORIGIN_PLATFORM,
                                           .authority = AIMEE_CTX_AUTH_POLICY,
                                           .trust = AIMEE_CTX_TRUST_VERIFIED,
                                           .sensitivity = AIMEE_CTX_SENS_INTERNAL,
                                           .model_visible = 1,
                                           .revision_epoch = 42};
   strcpy(system.context.revision_domain, "policy");
   strcpy(system.context.revision_scope, "owned-role");
   aimee_context_meta_t context = system.context;
   assert(agent_request_with_tools_supported(&request) &&
          !memcmp(&context, &system.context, sizeof context));
   system.context.model_visible = 0;
   assert(agent_request_with_tools_supported(&request));
   request.max_tokens = 0;
   assert(!agent_request_with_tools_supported(&request));
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
      assert(agent_request_with_tools_supported(&request) == !i);
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
