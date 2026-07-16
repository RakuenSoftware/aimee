/* test_agent_responses.c: OpenAI /responses SSE/JSON parser tests, split out of
 * test_agent.c to keep that file under the 2000-line hard limit. These functions
 * are declared in test_agent.c and called from its main(); both objects link into
 * the single unit-test-agent binary. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "agent.h"
#include "agent_protocol.h"
#include "cJSON.h"

void test_responses_parser_keeps_all_output_text_parts(void)
{
   const char *body =
       "event: response.output_item.done\n"
       "data: {\"item\":{\"type\":\"message\",\"content\":["
       "{\"type\":\"output_text\",\"text\":\"I did not deploy this to `192.\"},"
       "{\"type\":\"output_text\",\"text\":\"168.0.83`.\"}]}}\n\n"
       "event: response.completed\n"
       "data: {\"response\":{\"usage\":{\"input_tokens\":11,\"output_tokens\":22}}}\n\n";

   parsed_response_t parsed;
   agent_parse_response_responses(body, &parsed);
   assert(parsed.content != NULL);
   assert(strcmp(parsed.content, "I did not deploy this to `192.168.0.83`.") == 0);
   assert(parsed.prompt_tokens == 11);
   assert(parsed.completion_tokens == 22);
   agent_free_parsed_response(&parsed);
}

void test_responses_parser_accumulates_output_text_deltas(void)
{
   const char *body =
       "event: response.output_text.delta\r\n"
       "data: {\"delta\":\"The useful model fact here is that Qwen3.\"}\r\n\r\n"
       "event: response.output_text.delta\r\n"
       "data: {\"delta\":\"6 keeps scaling KV cache with context length.\"}\r\n\r\n"
       "event: response.completed\r\n"
       "data: {\"response\":{\"output\":[],\"usage\":{\"input_tokens\":5,\"output_tokens\":6}}}"
       "\r\n\r\n";

   parsed_response_t parsed;
   agent_parse_response_responses(body, &parsed);
   assert(parsed.content != NULL);
   assert(strcmp(parsed.content,
                 "The useful model fact here is that Qwen3.6 keeps scaling KV cache with context "
                 "length.") == 0);
   assert(parsed.prompt_tokens == 5);
   assert(parsed.completion_tokens == 6);
   agent_free_parsed_response(&parsed);
}

/* Walk a Responses object's output[] for the first non-empty message output_text.
 * Returns a borrowed pointer into `resp`, or NULL. */
static const char *first_output_text(struct cJSON *resp)
{
   struct cJSON *output = cJSON_GetObjectItemCaseSensitive(resp, "output");
   struct cJSON *item = NULL;
   cJSON_ArrayForEach(item, output)
   {
      struct cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
      if (!type || !cJSON_IsString(type) || strcmp(type->valuestring, "message") != 0)
         continue;
      struct cJSON *content = cJSON_GetObjectItemCaseSensitive(item, "content");
      struct cJSON *part = NULL;
      cJSON_ArrayForEach(part, content)
      {
         struct cJSON *pt = cJSON_GetObjectItemCaseSensitive(part, "type");
         struct cJSON *tx = cJSON_GetObjectItemCaseSensitive(part, "text");
         if (pt && cJSON_IsString(pt) && strcmp(pt->valuestring, "output_text") == 0 && tx &&
             cJSON_IsString(tx) && tx->valuestring[0])
            return tx->valuestring;
      }
   }
   return NULL;
}

/* Codex streams the answer as output_text deltas and its response.completed event
 * carries "output":[] (empty). The response OBJECT the IR/shadow consume must still
 * carry the text, so agent_responses_sse_response_object folds the SSE-aggregated
 * text back into output[] -- otherwise responses_backend_parse sees zero text while
 * the legacy parser recovered it (the live wire=3 shadow mismatch). */
void test_responses_object_folds_in_delta_text(void)
{
   const char *body =
       "event: response.output_text.delta\r\n"
       "data: {\"delta\":\"deployed to \"}\r\n\r\n"
       "event: response.output_text.delta\r\n"
       "data: {\"delta\":\"192.168.1.254\"}\r\n\r\n"
       "event: response.completed\r\n"
       "data: {\"response\":{\"output\":[],\"usage\":{\"input_tokens\":5,\"output_tokens\":6}}}"
       "\r\n\r\n";

   struct cJSON *resp = agent_responses_sse_response_object(body);
   assert(resp != NULL);
   const char *txt = first_output_text(resp);
   assert(txt != NULL);
   assert(strcmp(txt, "deployed to 192.168.1.254") == 0);
   cJSON_Delete(resp);
}

/* Guard against double-injection: when the completed object ALREADY carries the
 * message text (non-codex responses that repeat it in output[]), the extractor must
 * leave a single output_text -- not append a duplicate. */
void test_responses_object_keeps_existing_text(void)
{
   const char *body =
       "event: response.output_text.delta\r\n"
       "data: {\"delta\":\"hello there\"}\r\n\r\n"
       "event: response.completed\r\n"
       "data: {\"response\":{\"output\":[{\"type\":\"message\",\"content\":["
       "{\"type\":\"output_text\",\"text\":\"hello there\"}]}],"
       "\"usage\":{\"input_tokens\":1,\"output_tokens\":2}}}\r\n\r\n";

   struct cJSON *resp = agent_responses_sse_response_object(body);
   assert(resp != NULL);
   struct cJSON *output = cJSON_GetObjectItemCaseSensitive(resp, "output");
   int message_items = 0;
   struct cJSON *item = NULL;
   cJSON_ArrayForEach(item, output)
   {
      struct cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
      if (type && cJSON_IsString(type) && strcmp(type->valuestring, "message") == 0)
         message_items++;
   }
   assert(message_items == 1); /* not duplicated */
   const char *txt = first_output_text(resp);
   assert(txt != NULL && strcmp(txt, "hello there") == 0);
   cJSON_Delete(resp);
}

void test_responses_parser_uses_output_text_done(void)
{
   const char *body =
       "event: response.content_part.done\n"
       "data: {\"part\":{\"type\":\"output_text\",\"text\":\"One caveat: the endpoint I could "
       "reach at `192.168.1.103:8080`.\"}}\n\n"
       "event: response.output_text.done\n"
       "data: {\"text\":\"One caveat: the endpoint I could reach at `192.168.1.103:8080`.\"}\n\n"
       "event: response.completed\n"
       "data: "
       "{\"response\":{\"output\":[],\"usage\":{\"input_tokens\":7,\"output_tokens\":8}}}\n\n";

   parsed_response_t parsed;
   agent_parse_response_responses(body, &parsed);
   assert(parsed.content != NULL);
   assert(strcmp(parsed.content,
                 "One caveat: the endpoint I could reach at `192.168.1.103:8080`.") == 0);
   assert(parsed.prompt_tokens == 7);
   assert(parsed.completion_tokens == 8);
   agent_free_parsed_response(&parsed);
}

void test_responses_parser_separates_message_items(void)
{
   const char *body =
       "event: response.output_text.delta\n"
       "data: {\"delta\":\"PR.\"}\n\n"
       "event: response.output_text.delta\n"
       "data: {\"delta\":\"GitHub\"}\n\n"
       "event: response.output_item.done\n"
       "data: {\"item\":{\"type\":\"message\",\"content\":["
       "{\"type\":\"output_text\",\"text\":\"PR.\"}]}}\n\n"
       "event: response.output_item.done\n"
       "data: {\"item\":{\"type\":\"message\",\"content\":["
       "{\"type\":\"output_text\",\"text\":\"GitHub\"}]}}\n\n"
       "event: response.completed\n"
       "data: {\"response\":{\"output\":[],\"usage\":{\"input_tokens\":9,\"output_tokens\":10}}}"
       "\n\n";

   parsed_response_t parsed;
   agent_parse_response_responses(body, &parsed);
   assert(parsed.content != NULL);
   assert(strcmp(parsed.content, "PR.\n\nGitHub") == 0);
   assert(parsed.prompt_tokens == 9);
   assert(parsed.completion_tokens == 10);
   agent_free_parsed_response(&parsed);
}
