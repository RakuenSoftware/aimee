/* Request corpus through the active canonical frontend/backend codecs. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aimee/translation/aimee_frontend.h>
#include <aimee/translation/aimee_backend.h>
#include "cJSON.h"

/* The shapes a real Anthropic client actually sends. */
static const char *CORPUS[] = {
    /* plain single-turn */
    "{\"model\":\"m\",\"max_tokens\":16,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
    /* string system + multi-turn */
    "{\"model\":\"m\",\"max_tokens\":16,\"system\":\"be terse\","
    "\"messages\":[{\"role\":\"user\",\"content\":\"a\"},{\"role\":\"assistant\",\"content\":\"b\"}"
    ","
    "{\"role\":\"user\",\"content\":\"c\"}]}",
    /* block-array content */
    "{\"model\":\"m\",\"max_tokens\":16,\"messages\":[{\"role\":\"user\","
    "\"content\":[{\"type\":\"text\",\"text\":\"one\"},{\"type\":\"text\",\"text\":\"two\"}]}]}",
    /* tools */
    "{\"model\":\"m\",\"max_tokens\":16,\"tools\":[{\"name\":\"grep\",\"description\":\"d\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{}}}],"
    "\"messages\":[{\"role\":\"user\",\"content\":\"find\"}]}",
    /* tool_use + tool_result round-trip */
    "{\"model\":\"m\",\"max_tokens\":16,\"messages\":["
    "{\"role\":\"user\",\"content\":\"go\"},"
    "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"grep\","
    "\"input\":{\"q\":\"x\"}}]},"
    "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t1\","
    "\"content\":\"found\"}]}]}",
    /* temperature + stop sequences */
    "{\"model\":\"m\",\"max_tokens\":16,\"temperature\":0.2,\"messages\":"
    "[{\"role\":\"user\",\"content\":\"hi\"}]}",
    /* system as a block array with cache_control (the marker the ruling calls out) */
    "{\"model\":\"m\",\"max_tokens\":16,\"system\":[{\"type\":\"text\",\"text\":\"sys\","
    "\"cache_control\":{\"type\":\"ephemeral\"}}],"
    "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
    /* thinking block echoed back by a client */
    "{\"model\":\"m\",\"max_tokens\":16,\"messages\":[{\"role\":\"assistant\","
    "\"content\":[{\"type\":\"thinking\",\"thinking\":\"hmm\"},{\"type\":\"text\",\"text\":\"ok\"}]"
    "},"
    "{\"role\":\"user\",\"content\":\"next\"}]}",
    NULL,
};

int main(void)
{
   printf("canonical codec corpus:\n");

   int n = 0;
   for (int i = 0; CORPUS[i]; i++)
   {
      cJSON *req = cJSON_Parse(CORPUS[i]);
      assert(req && "corpus entry must be valid JSON");

      aimee_request_t request = {0};
      char error[128];
      assert(!aimee_frontend_parse_wire(CORPUS[i], strlen(CORPUS[i]), AIMEE_WIRE_ANTHROPIC,
                                        &request, error, sizeof error));
      assert(!aimee_ir_request_admit(&request));
      free(request.model);
      request.model = strdup("served-model");
      request.stream = 0;
      char *ir = aimee_backend_render_wire(&request, AIMEE_WIRE_OPENAI_CHAT);
      assert(ir);
      cJSON *j = cJSON_Parse(ir);
      assert(j && "IR body must be valid JSON");
      /* the agent's served model wins over the client's */
      assert(strcmp(cJSON_GetObjectItem(j, "model")->valuestring, "served-model") == 0);
      /* want_stream=0 must be honoured for every shape, not just the simple one --
       * this is the buffered-replay contract that the unparseable-reply bug broke. */
      assert(cJSON_GetObjectItem(j, "stream") == NULL);
      /* messages must survive: a shape that silently drops turns would "pass" a
       * NULL-check but corrupt the conversation. */
      cJSON *msgs = cJSON_GetObjectItem(j, "messages");
      assert(cJSON_IsArray(msgs) && cJSON_GetArraySize(msgs) >= 1);
      /* Absence remains absence. A retired migration shim used to invent a
       * temperature default here; actual parsed controls remain the authority. */
      cJSON *temp = cJSON_GetObjectItem(j, "temperature");
      cJSON *sent = cJSON_GetObjectItem(req, "temperature");
      assert(cJSON_IsNumber(temp) == cJSON_IsNumber(sent));
      if (cJSON_IsNumber(sent))
         assert(temp->valuedouble == sent->valuedouble);
      aimee_request_free(&request);

      cJSON_Delete(j);
      free(ir);
      cJSON_Delete(req);
      n++;
   }
   printf("  PASS: IR rendered all %d client shapes\n", n);

   aimee_request_t invalid = {0};
   char error[128];
   assert(aimee_frontend_parse_wire("null", 4, AIMEE_WIRE_ANTHROPIC, &invalid, error,
                                    sizeof error) < 0);
   aimee_request_free(&invalid);
   printf("canonical codec corpus: ok\n");
   return 0;
}
