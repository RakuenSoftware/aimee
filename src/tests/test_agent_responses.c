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
