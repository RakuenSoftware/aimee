#include "aimee.h"
#include "agent_protocol.h"
#include "cJSON.h"
#include "gw_stage_completion.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *json(const char *text)
{
   cJSON *out = cJSON_Parse(text);
   assert(out);
   return out;
}

static void test_intervenes_on_confirmed_deferred_siblings(void)
{
   cJSON *messages = json("[{\"role\":\"user\",\"content\":\"repair the unsafe path\"},"
                          "{\"role\":\"assistant\",\"tool_calls\":[{\"type\":\"function\","
                          "\"function\":{\"name\":\"apply_patch\",\"arguments\":\"{}\"}}]}]");
   cJSON *tools = json(
       "[{\"type\":\"function\",\"function\":{\"name\":\"exec_command\","
       "\"parameters\":{\"type\":\"object\",\"properties\":{\"cmd\":{\"type\":\"string\"}}}}}]");
   parsed_response_t parsed = {0};
   parsed.content = strdup(
       "Heads-up: the same unsafe path pattern exists in two report handlers. Want me to apply "
       "the same containment check there?");

   assert(gw_response_completion_armed(messages, tools) == 1);
   assert(gw_response_run_completion(&parsed, messages, tools, "tool_calls") == 1);
   assert(parsed.is_tool_call == 1 && parsed.call_count == 1);
   assert(strcmp(parsed.calls[0].name, "exec_command") == 0);
   assert(strstr(parsed.calls[0].arguments, "\"cmd\"") != NULL);
   assert(strstr(parsed.calls[0].arguments, "\"true\"") != NULL);
   assert(strncmp(parsed.calls[0].id, AIMEE_COMPLETION_CALL_PREFIX,
                  strlen(AIMEE_COMPLETION_CALL_PREFIX)) == 0);
   assert(strcmp(parsed.stop_reason, "tool_calls") == 0);

   free(parsed.content);
   free(parsed.calls[0].arguments);
   cJSON_Delete(messages);
   cJSON_Delete(tools);
}

static void test_does_not_intervene_without_confirmed_defect_or_edit(void)
{
   cJSON *no_edit = json("[{\"role\":\"user\",\"content\":\"review this code\"}]");
   cJSON *edited = json("[{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\","
                        "\"name\":\"apply_patch\",\"input\":{}}]}]");
   cJSON *tools = json("[{\"name\":\"Bash\",\"input_schema\":{\"type\":\"object\","
                       "\"properties\":{\"command\":{\"type\":\"string\"}}}}]");
   parsed_response_t parsed = {0};
   parsed.content = strdup("I fixed the requested file. Want me to update the docs too?");
   assert(gw_response_run_completion(&parsed, edited, tools, "tool_use") == 0);
   free(parsed.content);

   memset(&parsed, 0, sizeof(parsed));
   parsed.content = strdup("Related findings were not changed.");
   assert(gw_response_run_completion(&parsed, no_edit, tools, "tool_use") == 0);
   free(parsed.content);
   cJSON_Delete(no_edit);
   cJSON_Delete(edited);
   cJSON_Delete(tools);
}

static void test_one_marker_caps_intervention(void)
{
   cJSON *messages =
       json("[{\"role\":\"assistant\",\"tool_calls\":[{\"function\":{\"name\":"
            "\"apply_patch\",\"arguments\":\"{}\"}}]},"
            "{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"call_aimee_completion_7\","
            "\"function\":{\"name\":\"exec_command\",\"arguments\":\"{}\"}}]}]");
   cJSON *tools = json("[{\"type\":\"function\",\"name\":\"exec_command\","
                       "\"parameters\":{\"type\":\"object\",\"properties\":{\"cmd\":{}}}}]");
   parsed_response_t parsed = {0};
   parsed.content = strdup("Related finding: same defect. Not touched.");
   assert(gw_response_completion_armed(messages, tools) == 0);
   assert(gw_response_run_completion(&parsed, messages, tools, "tool_calls") == 0);
   free(parsed.content);
   cJSON_Delete(messages);
   cJSON_Delete(tools);
}

static void test_no_marker_allows_bounded_retry(void)
{
   cJSON *messages = json("[{\"role\":\"assistant\",\"tool_calls\":[{\"function\":{\"name\":"
                          "\"apply_patch\",\"arguments\":\"{}\"}}]}]");
   cJSON *tools = json("[{\"type\":\"function\",\"name\":\"exec_command\","
                       "\"parameters\":{\"type\":\"object\",\"properties\":{\"cmd\":{}}}}]");
   assert(gw_response_completion_armed(messages, tools) == 1);
   cJSON_Delete(messages);
   cJSON_Delete(tools);
}

static void test_structured_call_adds_authoritative_next_turn_policy(void)
{
   cJSON *messages =
       json("[{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"call_aimee_completion_9\","
            "\"function\":{\"name\":\"Bash\",\"arguments\":\"{\\\"command\\\":\\\"true\\\"}\"}}]},"
            "{\"role\":\"tool\",\"tool_call_id\":\"call_aimee_completion_9\","
            "\"content\":\"\"}]");
   char *prompt = gw_request_completion_system_prompt(messages, "existing policy");
   assert(prompt != NULL);
   assert(strstr(prompt, "existing policy") != NULL);
   assert(strstr(prompt, "<aimee-completion-policy>") != NULL);
   assert(strstr(prompt, "unless the user explicitly excluded it") != NULL);
   free(prompt);

   cJSON *ordinary = json("[{\"role\":\"user\",\"content\":\"hello\"}]");
   assert(gw_request_completion_system_prompt(ordinary, "base") == NULL);
   cJSON_Delete(ordinary);
   cJSON_Delete(messages);
}

static void test_shell_surface_adds_cli_first_policy(void)
{
   cJSON *tools = json("[{\"type\":\"function\",\"name\":\"exec_command\","
                       "\"parameters\":{\"type\":\"object\",\"properties\":{\"cmd\":{}}}}]");
   char *prompt = gw_request_tool_system_prompt(tools, "existing policy");
   assert(prompt != NULL);
   assert(strstr(prompt, "existing policy") != NULL);
   assert(strstr(prompt, "<aimee-tool-policy>") != NULL);
   assert(strstr(prompt, "CLI is the preferred registered") != NULL);
   assert(strstr(prompt, "MCP is the alternative") != NULL);
   assert(strstr(prompt, "root cause, not merely the named") != NULL);
   assert(strstr(prompt, "aimee index ast-grep") != NULL);
   assert(strstr(prompt, "preserving the existing success-path contract") != NULL);
   free(prompt);
   cJSON_Delete(tools);

   cJSON *no_shell = json("[{\"type\":\"function\",\"name\":\"read_file\"}]");
   assert(gw_request_tool_system_prompt(no_shell, "base") == NULL);
   cJSON_Delete(no_shell);
}

static void test_shell_redirect_counts_as_an_edit(void)
{
   cJSON *messages = json(
       "[{\"role\":\"assistant\",\"tool_calls\":[{\"function\":{\"name\":"
       "\"exec_command\",\"arguments\":\"{\\\"cmd\\\":\\\"cat > app/files.py <<EOF\\\"}\"}}]}]");
   cJSON *tools = json("[{\"type\":\"function\",\"name\":\"exec_command\","
                       "\"parameters\":{\"type\":\"object\",\"properties\":{\"cmd\":{}}}}]");
   assert(gw_response_completion_armed(messages, tools) == 1);
   cJSON_Delete(messages);
   cJSON_Delete(tools);
}

static void test_observed_scope_deferral_wording_intervenes(void)
{
   cJSON *messages = json(
       "[{\"role\":\"assistant\",\"tool_calls\":[{\"function\":{\"name\":"
       "\"exec_command\",\"arguments\":\"{\\\"cmd\\\":\\\"cat > app/files.py <<EOF\\\"}\"}}]}]");
   cJSON *tools = json("[{\"type\":\"function\",\"name\":\"exec_command\","
                       "\"parameters\":{\"type\":\"object\",\"properties\":{\"cmd\":{}}}}]");
   parsed_response_t parsed = {0};
   parsed.content = strdup(
       "One heads-up: app/reports.py has the same flagged traversal pattern but was outside the "
       "scope of this request - want me to apply the same fix there?");
   assert(gw_response_run_completion(&parsed, messages, tools, "tool_calls") == 1);
   free(parsed.content);
   free(parsed.calls[0].arguments);
   cJSON_Delete(messages);
   cJSON_Delete(tools);
}

int main(void)
{
   test_intervenes_on_confirmed_deferred_siblings();
   test_does_not_intervene_without_confirmed_defect_or_edit();
   test_one_marker_caps_intervention();
   test_no_marker_allows_bounded_retry();
   test_structured_call_adds_authoritative_next_turn_policy();
   test_shell_surface_adds_cli_first_policy();
   test_shell_redirect_counts_as_an_edit();
   test_observed_scope_deferral_wording_intervenes();
   puts("response completion stage: ok");
   return 0;
}
