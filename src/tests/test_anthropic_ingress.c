/* Anthropic ingress uses native entry -> owned IR -> explicit native egress. */
#include "headers/anthropic_ingress.h"
#include <aimee/translation/aimee_frontend.h>
#include <aimee/translation/aimee_backend.h>
#include <aimee/translation/aimee_ir_stream.h>
#include "cJSON.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *str(const cJSON *object, const char *key)
{
   const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
   assert(value);
   return value;
}

static void test_request_entry(void)
{
   const char *wire =
       "{\"model\":\"native\",\"max_tokens\":500,\"system\":[{\"type\":\"text\",\"text\":\"first\"}"
       ","
       "{\"type\":\"text\",\"text\":\"second\",\"cache_control\":{\"type\":\"ephemeral\",\"ttl\":"
       "\"1h\"}}],"
       "\"tools\":[{\"name\":\"lookup\",\"description\":\"lookup "
       "description\",\"input_schema\":{\"type\":\"object\"}}],"
       "\"tool_choice\":{\"type\":\"tool\",\"name\":\"lookup\"},\"messages\":["
       "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"question\"},"
       "{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":\"image/"
       "png\",\"data\":\"aGVsbG8=\"}}]},"
       "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"native-call\","
       "\"name\":\"lookup\","
       "\"input\":{\"large\":9007199254740993,\"tiny\":1e-400}}]},"
       "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"native-call\","
       "\"content\":\"evidence\"},{\"type\":\"text\",\"text\":\"user followup\"}]}]}";
   aimee_request_t request = {0};
   char error[256];
   assert(aimee_frontend_parse_wire(wire, strlen(wire), AIMEE_WIRE_ANTHROPIC, &request, error,
                                    sizeof error) == 0);
   assert(request.n_system == 2 && request.n_messages == 3 && request.n_tools == 1);
   assert(request.messages[0].n_blocks == 2);
   assert(request.messages[0].blocks[1].type == AIMEE_BLK_IMAGE);
   assert(request.messages[1].blocks[0].type == AIMEE_BLK_TOOL_USE);
   assert(!strcmp(request.messages[1].blocks[0].tool_id, "native-call"));
   assert(request.messages[2].blocks[0].type == AIMEE_BLK_TOOL_RESULT);
   assert(request.messages[2].blocks[1].type == AIMEE_BLK_TEXT);
   assert(!strcmp(request.messages[2].blocks[1].text, "user followup"));
   char *native = aimee_backend_render_wire(&request, AIMEE_WIRE_ANTHROPIC);
   assert(native && !strcmp(native, wire));
   free(native);
   cJSON *chat = openai_backend_build(&request);
   assert(chat);
   cJSON *messages = cJSON_GetObjectItemCaseSensitive(chat, "messages");
   assert(cJSON_GetArraySize(messages) == 6);
   assert(!strcmp(str(cJSON_GetArrayItem(messages, 0), "content"), "first"));
   assert(!strcmp(str(cJSON_GetArrayItem(messages, 1), "content"), "second"));
   cJSON *calls = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(messages, 3), "tool_calls");
   cJSON *call = cJSON_GetArrayItem(calls, 0);
   assert(!strcmp(str(call, "id"), "native-call"));
   const char *arguments = str(cJSON_GetObjectItemCaseSensitive(call, "function"), "arguments");
   assert(strstr(arguments, "9007199254740993") && strstr(arguments, "1e-400"));
   assert(!strcmp(str(cJSON_GetArrayItem(messages, 4), "role"), "tool"));
   assert(!strcmp(str(cJSON_GetArrayItem(messages, 5), "content"), "user followup"));
   cJSON_Delete(chat);
   aimee_request_free(&request);
}

static void test_response_entry(void)
{
   cJSON *native =
       cJSON_Parse("{\"id\":\"msg-native\",\"type\":\"message\",\"role\":\"assistant\","
                   "\"model\":\"native\",\"content\":[{\"type\":\"thinking\",\"thinking\":"
                   "\"private\",\"signature\":\"signed-native\"},"
                   "{\"type\":\"text\",\"text\":\"answer\"}],\"stop_reason\":\"max_tokens\","
                   "\"usage\":{\"input_tokens\":100,\"output_tokens\":20,\"cache_read_input_"
                   "tokens\":80,\"cache_creation_input_tokens\":10}}");
   assert(native);
   cJSON *content = cJSON_GetObjectItemCaseSensitive(native, "content");
   char name[401], id[501];
   memset(name, 'n', sizeof name - 1);
   memset(id, 'i', sizeof id - 1);
   name[sizeof name - 1] = id[sizeof id - 1] = 0;
   for (int i = 0; i < 40; i++)
   {
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "tool_use");
      char unique_id[520];
      snprintf(unique_id, sizeof unique_id, "%s-%d", id, i);
      cJSON_AddStringToObject(tool, "id", unique_id);
      cJSON_AddStringToObject(tool, "name", name);
      cJSON_AddItemToObject(tool, "input", cJSON_Parse("{\"large\":9007199254740993}"));
      cJSON_AddItemToArray(content, tool);
   }
   aimee_response_t response = {0};
   char error[256];
   assert(anthropic_backend_parse(native, &response, error, sizeof error) == 0);
   assert(response.n_content == 42 && aimee_ir_response_tool_count(&response) == 40);
   assert(response.stop_reason == AIMEE_STOP_MAX_TOKENS);
   assert(response.usage_in == 100 && response.usage_out == 20 && response.usage_cache_read == 80 &&
          response.usage_cache_write == 10);
   assert(!strcmp(response.content[0].thinking_signature, "signed-native"));
   cJSON *rendered = anthropic_frontend_render(&response);
   assert(rendered);
   assert(!strcmp(str(rendered, "stop_reason"), "max_tokens"));
   cJSON *blocks = cJSON_GetObjectItemCaseSensitive(rendered, "content");
   assert(cJSON_GetArraySize(blocks) == 42);
   assert(!strcmp(str(cJSON_GetArrayItem(blocks, 0), "signature"), "signed-native"));
   for (int i = 0; i < 40; i++)
   {
      cJSON *tool = cJSON_GetArrayItem(blocks, i + 2);
      assert(!strcmp(str(tool, "name"), name));
      assert(strlen(str(tool, "id")) >= 502);
      char *input = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(tool, "input"));
      assert(input && strstr(input, "9007199254740993"));
      free(input);
   }
   cJSON_Delete(rendered);
   aimee_response_free(&response);
   cJSON_Delete(native);
}

static void *headers_thread(void *unused)
{
   (void)unused;
   assert(!anthropic_ingress_request_version()[0] && !anthropic_ingress_request_beta()[0]);
   anthropic_ingress_set_request_headers("thread-version", "thread-beta");
   assert(!strcmp(anthropic_ingress_request_beta(), "thread-beta"));
   return NULL;
}

static void test_headers(void)
{
   anthropic_ingress_set_request_headers("2023-06-01", "prompt-caching-2024-07-31");
   pthread_t thread;
   assert(pthread_create(&thread, NULL, headers_thread, NULL) == 0);
   assert(pthread_join(thread, NULL) == 0);
   assert(!strcmp(anthropic_ingress_request_version(), "2023-06-01"));
   assert(!strcmp(anthropic_ingress_request_beta(), "prompt-caching-2024-07-31"));
   anthropic_ingress_set_request_headers(NULL, "");
   assert(!anthropic_ingress_request_version()[0] && !anthropic_ingress_request_beta()[0]);
}

int main(void)
{
   test_request_entry();
   test_response_entry();
   test_headers();
   puts("anthropic ingress: canonical boundaries and isolated transport metadata passed");
   return 0;
}
