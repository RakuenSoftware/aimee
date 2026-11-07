/* Native response admission regressions after retirement of the flat shadow.
 * These use the production codecs and retain the owned response throughout. */
#include <aimee/translation/aimee_frontend.h>
#include <aimee/translation/aimee_backend.h>
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void refuse_frame(const char *frame, size_t length, aimee_wire_t wire)
{
   aimee_response_t refused = {0};
   char error[256] = "";
   int rc = aimee_backend_parse_wire(frame, length, wire, &refused, error, sizeof error);
   assert(rc == -1 && !refused.content && !refused.model && !refused.n_content && error[0]);
   aimee_response_free(&refused);
}

static void roundtrip(aimee_wire_t wire, aimee_stop_reason_t stop)
{
   aimee_response_t expected = {.stop_reason = stop,
                                .n_content = 41,
                                .model = strdup("native-model"),
                                .id = strdup("native-reply"),
                                .usage_in = 99,
                                .usage_out = 33,
                                .usage_cache_read = 22};
   expected.content = calloc((size_t)expected.n_content, sizeof *expected.content);
   assert(expected.content);
   char *text = malloc(20001);
   assert(text);
   memset(text, 'x', 20000);
   memcpy(text, "  exact whitespace\n", 19);
   text[19999] = '\n';
   text[20000] = 0;
   expected.content[0] = (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = text};
   for (int i = 1; i < expected.n_content; i++)
   {
      char id[512];
      memset(id, 'z', 400);
      snprintf(id + 400, sizeof id - 400, "-%d", i);
      expected.content[i] = (aimee_block_t){
          .type = AIMEE_BLK_TOOL_USE,
          .tool_id = strdup(id),
          .tool_name = strdup("lookup"),
          .tool_input =
              cJSON_Parse("{\"query\":\"雪\\nquoted \\\"value\\\"\",\"number\":9007199254740993}")};
      assert(expected.content[i].tool_input);
   }
   cJSON *native = wire == AIMEE_WIRE_ANTHROPIC     ? anthropic_frontend_render(&expected)
                   : wire == AIMEE_WIRE_OPENAI_CHAT ? openai_frontend_render(&expected)
                                                    : responses_frontend_render(&expected);
   assert(native);
   char *frame = cJSON_PrintUnformatted(native);
   assert(frame);
   aimee_response_t actual = {0};
   char error[256] = "";
   assert(!aimee_backend_parse_wire(frame, strlen(frame), wire, &actual, error, sizeof error));
   assert(actual.n_content == 41 && actual.stop_reason == stop);
   assert(actual.usage_in == 99 && actual.usage_out == 33 && actual.usage_cache_read == 22);
   assert(actual.model && !strcmp(actual.model, expected.model));
   assert(actual.content[0].type == AIMEE_BLK_TEXT && !strcmp(actual.content[0].text, text));
   for (int i = 1; i < actual.n_content; i++)
   {
      assert(actual.content[i].type == AIMEE_BLK_TOOL_USE);
      assert(!strcmp(actual.content[i].tool_id, expected.content[i].tool_id));
      assert(!strcmp(actual.content[i].tool_name, expected.content[i].tool_name));
      char *arguments = cJSON_PrintUnformatted(actual.content[i].tool_input);
      assert(arguments && strstr(arguments, "9007199254740993"));
      assert(cJSON_Compare(actual.content[i].tool_input, expected.content[i].tool_input, 1));
      free(arguments);
   }
   aimee_response_free(&actual);
   const char *model_at = strstr(frame, "native-model");
   assert(model_at);
   const char *escaped_nul = "before\\u0000after";
   size_t prefix = (size_t)(model_at - frame), suffix = strlen(model_at + strlen("native-model"));
   size_t malformed_length = prefix + strlen(escaped_nul) + suffix;
   char *malformed = malloc(malformed_length + 1);
   assert(malformed);
   memcpy(malformed, frame, prefix);
   memcpy(malformed + prefix, escaped_nul, strlen(escaped_nul));
   memcpy(malformed + prefix + strlen(escaped_nul), model_at + strlen("native-model"), suffix + 1);
   refuse_frame(malformed, malformed_length, wire);
   free(malformed);
   if (wire == AIMEE_WIRE_RESPONSES && stop == AIMEE_STOP_TOOL_USE)
   {
      cJSON *completed = cJSON_CreateObject();
      assert(completed);
      cJSON_AddStringToObject(completed, "type", "response.completed");
      cJSON_AddItemToObject(completed, "response", cJSON_Duplicate(native, 1));
      char *payload = cJSON_PrintUnformatted(completed);
      cJSON_Delete(completed);
      assert(payload);
      size_t capacity = strlen(payload) + 64;
      char *sse = malloc(capacity);
      assert(sse);
      int bytes = snprintf(sse, capacity, "event: response.completed\ndata: %s\n\n", payload);
      free(payload);
      assert(bytes > 0 && (size_t)bytes + 3 < capacity);
      assert(!aimee_backend_parse_wire(sse, (size_t)bytes, wire, &actual, error, sizeof error));
      assert(actual.n_content == expected.n_content);
      aimee_response_free(&actual);
      sse[bytes] = 0;
      sse[bytes + 1] = '{';
      sse[bytes + 2] = '}';
      /* A valid SSE prefix must not launder an invalid full byte response. */
      refuse_frame(sse, (size_t)bytes + 3, wire);
      free(sse);
   }
   aimee_response_free(&expected);
   cJSON_Delete(native);
   free(frame);
}
int main(void)
{
   for (int wire = AIMEE_WIRE_ANTHROPIC; wire <= AIMEE_WIRE_RESPONSES; wire++)
   {
      roundtrip((aimee_wire_t)wire, AIMEE_STOP_TOOL_USE);
      roundtrip((aimee_wire_t)wire, AIMEE_STOP_MAX_TOKENS);
      aimee_response_t refused = {0};
      char error[256] = "";
      assert(aimee_backend_parse_wire("not json", 8, (aimee_wire_t)wire, &refused, error,
                                      sizeof error));
      aimee_response_free(&refused);
   }
   puts("native response codecs: long text/IDs, dynamic tools, exact arguments, usage and stop "
        "reasons passed");
   return 0;
}
