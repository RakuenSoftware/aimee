/* test_openai_shape.c: unit tests for the OpenAI-compatible JSON shaping
 * helpers (pure — no sockets, no network, no agent execution). */
#include "openai_shape.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
   printf("openai_shape: ");
   char resp[4096];

   /* --- parse chat: model + flattened transcript + stream flag --- */
   {
      char model[64] = "";
      char *prompt = NULL;
      int stream = -1;
      int rc =
          openai_parse_chat_request("{\"model\":\"gpt-4\",\"messages\":["
                                    "{\"role\":\"system\",\"content\":\"be brief\"},"
                                    "{\"role\":\"user\",\"content\":\"hello\"}],\"stream\":false}",
                                    model, sizeof(model), &prompt, &stream);
      assert(rc == 0);
      assert(strcmp(model, "gpt-4") == 0);
      assert(stream == 0);
      assert(prompt && strstr(prompt, "be brief") && strstr(prompt, "hello"));
      free(prompt);
   }

   /* --- parse chat: stream:true is honoured --- */
   {
      char model[64] = "";
      char *prompt = NULL;
      int stream = -1;
      int rc = openai_parse_chat_request(
          "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"stream\":true}", model,
          sizeof(model), &prompt, &stream);
      assert(rc == 0);
      assert(stream == 1);
      free(prompt);
   }

   /* --- parse chat: empty messages -> error, prompt NULL --- */
   {
      char model[64] = "";
      char *prompt = (char *)0x1;
      int stream = 0;
      int rc =
          openai_parse_chat_request("{\"messages\":[]}", model, sizeof(model), &prompt, &stream);
      assert(rc == -1);
      assert(prompt == NULL);
   }

   /* --- parse chat: invalid JSON -> error --- */
   {
      char model[64] = "";
      char *prompt = (char *)0x1;
      int rc = openai_parse_chat_request("{not json", model, sizeof(model), &prompt, NULL);
      assert(rc == -1);
      assert(prompt == NULL);
   }

   /* --- parse chat: missing model defaults to "aimee" --- */
   {
      char model[64] = "";
      char *prompt = NULL;
      int rc = openai_parse_chat_request("{\"messages\":[{\"role\":\"user\",\"content\":\"x\"}]}",
                                         model, sizeof(model), &prompt, NULL);
      assert(rc == 0);
      assert(strcmp(model, "aimee") == 0);
      free(prompt);
   }

   /* --- parse completion: prompt extracted; missing prompt -> error --- */
   {
      char model[64] = "";
      char *prompt = NULL;
      int stream = 0;
      int rc = openai_parse_completion_request("{\"model\":\"m\",\"prompt\":\"finish this\"}",
                                               model, sizeof(model), &prompt, &stream);
      assert(rc == 0);
      assert(strcmp(model, "m") == 0);
      assert(prompt && strcmp(prompt, "finish this") == 0);
      free(prompt);

      char *p2 = (char *)0x1;
      int rc2 =
          openai_parse_completion_request("{\"model\":\"m\"}", model, sizeof(model), &p2, NULL);
      assert(rc2 == -1);
      assert(p2 == NULL);
   }

   /* --- models list --- */
   {
      const char *ids[] = {"aimee", "openai"};
      int len = openai_format_models_list(ids, 2, "aimee", resp, sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"object\":\"list\""));
      assert(strstr(resp, "\"id\":\"aimee\""));
      assert(strstr(resp, "\"id\":\"openai\""));
      assert(strstr(resp, "\"owned_by\":\"aimee\""));
   }

   /* --- chat.completion envelope --- */
   {
      int len = openai_format_chat_completion("cmpl-1", "aimee", "hi there", 123, 5, 2, resp,
                                              sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"object\":\"chat.completion\""));
      assert(strstr(resp, "\"role\":\"assistant\""));
      assert(strstr(resp, "\"content\":\"hi there\""));
      assert(strstr(resp, "\"finish_reason\":\"stop\""));
      assert(strstr(resp, "\"total_tokens\":7"));
   }

   /* --- text_completion envelope --- */
   {
      int len = openai_format_text_completion("cmpl-2", "aimee", "hi there", 123, 1, 1, resp,
                                              sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"object\":\"text_completion\""));
      assert(strstr(resp, "\"text\":\"hi there\""));
   }

   /* --- error envelope --- */
   {
      int len = openai_format_error(resp, sizeof(resp), "invalid_request_error", "bad model");
      assert(len > 0);
      assert(strstr(resp, "\"type\":\"invalid_request_error\""));
      assert(strstr(resp, "bad model"));
   }

   /* --- optional sampling-field readers --- */
   {
      /* present + in range */
      assert(openai_request_double("{\"temperature\":0.4}", "temperature", 0.7, 2.0) == 0.4);
      assert(openai_request_int("{\"max_tokens\":128}", "max_tokens", 2048, 32768) == 128);
      /* absent -> default */
      assert(openai_request_double("{}", "temperature", 0.7, 2.0) == 0.7);
      assert(openai_request_int("{}", "max_tokens", 2048, 32768) == 2048);
      /* zero temperature is valid; negative falls back */
      assert(openai_request_double("{\"temperature\":0}", "temperature", 0.7, 2.0) == 0.0);
      assert(openai_request_double("{\"temperature\":-1}", "temperature", 0.7, 2.0) == 0.7);
      /* out of range / non-numeric / invalid JSON -> default */
      assert(openai_request_double("{\"temperature\":9}", "temperature", 0.7, 2.0) == 0.7);
      assert(openai_request_int("{\"max_tokens\":0}", "max_tokens", 2048, 32768) == 2048);
      assert(openai_request_int("{\"max_tokens\":\"x\"}", "max_tokens", 2048, 32768) == 2048);
      assert(openai_request_int("{bad", "max_tokens", 2048, 32768) == 2048);
   }

   /* --- embeddings: parse string input --- */
   {
      char model[64] = "";
      char **inputs = NULL;
      int n = -1;
      int rc = openai_parse_embeddings_request("{\"model\":\"m\",\"input\":\"hello\"}", model,
                                               sizeof(model), &inputs, &n);
      assert(rc == 0 && n == 1 && strcmp(model, "m") == 0);
      assert(inputs && strcmp(inputs[0], "hello") == 0);
      openai_free_inputs(inputs, n);
   }

   /* --- embeddings: parse array input (skips empties), default model --- */
   {
      char model[64] = "";
      char **inputs = NULL;
      int n = -1;
      int rc = openai_parse_embeddings_request("{\"input\":[\"a\",\"\",\"b\"]}", model,
                                               sizeof(model), &inputs, &n);
      assert(rc == 0 && n == 2 && strcmp(model, "aimee") == 0);
      assert(strcmp(inputs[0], "a") == 0 && strcmp(inputs[1], "b") == 0);
      openai_free_inputs(inputs, n);
   }

   /* --- embeddings: missing/empty input and invalid JSON -> error --- */
   {
      char model[64] = "";
      char **inputs = (char **)0x1;
      int n = -1;
      assert(openai_parse_embeddings_request("{\"input\":[]}", model, sizeof(model), &inputs, &n) ==
             -1);
      assert(inputs == NULL && n == 0);
      assert(openai_parse_embeddings_request("{nope", model, sizeof(model), &inputs, &n) == -1);
   }

   /* --- embeddings: format response --- */
   {
      float v0[] = {0.1f, 0.2f, 0.3f};
      float v1[] = {0.4f, 0.5f, 0.6f};
      const float *vecs[] = {v0, v1};
      int dims[] = {3, 3};
      int len = openai_format_embeddings("aimee", vecs, dims, 2, 5, resp, sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"object\":\"list\""));
      assert(strstr(resp, "\"object\":\"embedding\""));
      assert(strstr(resp, "\"index\":1"));
      assert(strstr(resp, "\"embedding\":["));
      assert(strstr(resp, "\"total_tokens\":5"));
   }

   /* --- responses: parse string input --- */
   {
      char model[64] = "";
      char prev[128] = "x";
      char *prompt = NULL;
      int stream = 9;
      int rc = openai_parse_responses_request(
          "{\"model\":\"m\",\"input\":\"hello\",\"previous_response_id\":\"resp_1\"}", model,
          sizeof(model), &prompt, prev, sizeof(prev), &stream);
      assert(rc == 0);
      assert(strcmp(model, "m") == 0);
      assert(strcmp(prev, "resp_1") == 0);
      assert(stream == 0);
      assert(prompt && strcmp(prompt, "user: hello\n") == 0);
      free(prompt);
   }

   /* --- responses: parse array of message items (string + {role,content parts}) --- */
   {
      char model[64] = "";
      char prev[128] = "x";
      char *prompt = NULL;
      const char *body =
          "{\"input\":[\"hi\",{\"role\":\"system\",\"content\":\"be terse\"},"
          "{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"two\"}]}]}";
      int rc = openai_parse_responses_request(body, model, sizeof(model), &prompt, prev,
                                              sizeof(prev), NULL);
      assert(rc == 0);
      assert(strcmp(model, "aimee") == 0); /* default */
      assert(prev[0] == '\0');             /* absent */
      assert(prompt && strcmp(prompt, "user: hi\nsystem: be terse\nuser: two\n") == 0);
      free(prompt);
   }

   /* --- responses: empty/missing input and invalid JSON -> error --- */
   {
      char model[64] = "";
      char *prompt = (char *)1;
      assert(openai_parse_responses_request("{\"input\":[]}", model, sizeof(model), &prompt, NULL,
                                            0, NULL) == -1);
      assert(prompt == NULL);
      assert(openai_parse_responses_request("{}", model, sizeof(model), &prompt, NULL, 0, NULL) ==
             -1);
      assert(openai_parse_responses_request("{bad", model, sizeof(model), &prompt, NULL, 0, NULL) ==
             -1);
   }

   /* --- responses: format response object --- */
   {
      int len = openai_format_response("resp_42", "aimee", "the answer", 1700000000, 7, 3, resp,
                                       sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"object\":\"response\""));
      assert(strstr(resp, "\"id\":\"resp_42\""));
      assert(strstr(resp, "\"type\":\"output_text\""));
      assert(strstr(resp, "\"text\":\"the answer\""));
      assert(strstr(resp, "\"role\":\"assistant\""));
      assert(strstr(resp, "\"total_tokens\":10"));
   }

   /* --- request_bool: only literal true counts --- */
   {
      assert(openai_request_bool("{\"stream\":true}", "stream") == 1);
      assert(openai_request_bool("{\"stream\":false}", "stream") == 0);
      assert(openai_request_bool("{\"x\":1}", "stream") == 0);      /* absent */
      assert(openai_request_bool("{\"stream\":1}", "stream") == 0); /* number, not bool */
      assert(openai_request_bool("{bad", "stream") == 0);           /* invalid JSON */
      assert(openai_request_bool(NULL, "stream") == 0);
   }

   /* --- chat.completion.chunk frames: role / content / finish --- */
   {
      /* role frame: delta has role, finish_reason null */
      int len = openai_format_chat_chunk("chatcmpl-1", "aimee", 1700000000, 1, NULL, 0, resp,
                                         sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"object\":\"chat.completion.chunk\""));
      assert(strstr(resp, "\"id\":\"chatcmpl-1\""));
      assert(strstr(resp, "\"role\":\"assistant\""));
      assert(strstr(resp, "\"finish_reason\":null"));
      assert(!strstr(resp, "\"content\"")); /* no content on the role frame */

      /* content frame: delta has content, no role, finish_reason null */
      len = openai_format_chat_chunk("chatcmpl-1", "aimee", 1700000000, 0, "hello", 0, resp,
                                     sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"content\":\"hello\""));
      assert(!strstr(resp, "\"role\""));
      assert(strstr(resp, "\"finish_reason\":null"));

      /* terminal frame: empty delta, finish_reason stop */
      len = openai_format_chat_chunk("chatcmpl-1", "aimee", 1700000000, 0, NULL, 1, resp,
                                     sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"delta\":{}"));
      assert(strstr(resp, "\"finish_reason\":\"stop\""));
   }

   /* --- text_completion chunk frames: content / terminal --- */
   {
      int len =
          openai_format_text_chunk("cmpl-1", "aimee", 1700000000, "lorem", 0, resp, sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"object\":\"text_completion\""));
      assert(strstr(resp, "\"id\":\"cmpl-1\""));
      assert(strstr(resp, "\"text\":\"lorem\""));
      assert(strstr(resp, "\"finish_reason\":null"));

      len = openai_format_text_chunk("cmpl-1", "aimee", 1700000000, "", 1, resp, sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"text\":\"\""));
      assert(strstr(resp, "\"finish_reason\":\"stop\""));
   }

   /* --- responses streaming events: created / delta / completed --- */
   {
      int len = openai_format_responses_created("resp_9", "aimee", 1700000000, resp, sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"type\":\"response.created\""));
      assert(strstr(resp, "\"object\":\"response\""));
      assert(strstr(resp, "\"status\":\"in_progress\""));
      assert(strstr(resp, "\"output\":[]")); /* no output yet on creation */

      len = openai_format_responses_delta("resp_9-msg", "lorem", resp, sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"type\":\"response.output_text.delta\""));
      assert(strstr(resp, "\"item_id\":\"resp_9-msg\""));
      assert(strstr(resp, "\"delta\":\"lorem\""));

      len = openai_format_responses_completed("resp_9", "aimee", "the answer", 1700000000, 7, 3,
                                              resp, sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"type\":\"response.completed\""));
      assert(strstr(resp, "\"status\":\"completed\""));
      assert(strstr(resp, "\"type\":\"output_text\""));
      assert(strstr(resp, "\"text\":\"the answer\""));
      assert(strstr(resp, "\"total_tokens\":10"));
   }

   /* --- Codex parity: message output-item events --- */
   {
      int len = openai_format_responses_msg_item_added("resp_9-msg", 0, resp, sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"type\":\"response.output_item.added\""));
      assert(strstr(resp, "\"type\":\"message\""));
      assert(strstr(resp, "\"status\":\"in_progress\""));
      assert(strstr(resp, "\"role\":\"assistant\""));

      len =
          openai_format_responses_msg_item_done("resp_9-msg", "hello world", 0, resp, sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"type\":\"response.output_item.done\""));
      assert(strstr(resp, "\"status\":\"completed\""));
      assert(strstr(resp, "\"type\":\"output_text\""));
      assert(strstr(resp, "\"text\":\"hello world\""));
   }

   /* --- Codex parity: function_call output-item + argument events --- */
   {
      int len = openai_format_responses_fc_item_added("resp_9-fc-0", "call_42", "exec_command", 0,
                                                      resp, sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"type\":\"response.output_item.added\""));
      assert(strstr(resp, "\"type\":\"function_call\""));
      assert(strstr(resp, "\"call_id\":\"call_42\""));
      assert(strstr(resp, "\"name\":\"exec_command\""));

      len = openai_format_responses_fc_args_delta("resp_9-fc-0", 0, "{\"cmd\":\"ls\"}", resp,
                                                  sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"type\":\"response.function_call_arguments.delta\""));
      assert(strstr(resp, "\"item_id\":\"resp_9-fc-0\""));

      len = openai_format_responses_fc_args_done("resp_9-fc-0", 0, "{\"cmd\":\"ls\"}", resp,
                                                 sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"type\":\"response.function_call_arguments.done\""));

      len = openai_format_responses_fc_item_done("resp_9-fc-0", "call_42", "exec_command",
                                                 "{\"cmd\":\"ls\"}", 0, resp, sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"type\":\"response.output_item.done\""));
      assert(strstr(resp, "\"type\":\"function_call\""));
      assert(strstr(resp, "\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\""));
   }

   /* --- Codex parity: completed with a function_call output item --- */
   {
      struct cJSON *out = cJSON_CreateArray();
      cJSON_AddItemToArray((cJSON *)out, openai_responses_function_call_item(
                                             "resp_9-fc-0", "call_42", "exec_command",
                                             "{\"cmd\":\"ls\"}", "completed"));
      int len = openai_format_responses_completed_items("resp_9", "aimee", 1700000000, out, 5, 2,
                                                        resp, sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"type\":\"response.completed\""));
      assert(strstr(resp, "\"type\":\"function_call\""));
      assert(strstr(resp, "\"call_id\":\"call_42\""));
      assert(strstr(resp, "\"total_tokens\":7"));
   }

   /* --- Codex parity: terminal error events --- */
   {
      int len = openai_format_responses_failed("resp_9", "aimee", 1700000000, "server_error",
                                               "upstream model request failed", resp, sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"type\":\"response.failed\""));
      assert(strstr(resp, "\"status\":\"failed\""));
      assert(strstr(resp, "\"error\":{"));
      assert(strstr(resp, "\"code\":\"server_error\""));
      assert(strstr(resp, "\"message\":\"upstream model request failed\""));

      len = openai_format_responses_incomplete("resp_9", "aimee", 1700000000, "max_output_tokens",
                                               resp, sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"type\":\"response.incomplete\""));
      assert(strstr(resp, "\"status\":\"incomplete\""));
      assert(strstr(resp, "\"incomplete_details\":{\"reason\":\"max_output_tokens\"}"));
   }

   /* --- Codex parity: Responses request -> OpenAI chat conversion --- */
   {
      const char *body =
          "{\"model\":\"aimee\",\"instructions\":\"be helpful\",\"stream\":true,\"input\":["
          "{\"type\":\"message\",\"role\":\"developer\",\"content\":[{\"type\":\"input_text\","
          "\"text\":\"dev note\"}]},"
          "{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":"
          "\"do it\"}]},"
          "{\"type\":\"function_call\",\"name\":\"exec_command\",\"arguments\":\"{\\\"cmd\\\":"
          "\\\"ls\\\"}\",\"call_id\":\"call_1\"},"
          "{\"type\":\"function_call_output\",\"call_id\":\"call_1\",\"output\":\"file.txt\"}"
          "],\"tools\":[{\"type\":\"function\",\"name\":\"exec_command\",\"description\":\"run\","
          "\"parameters\":{\"type\":\"object\"}},{\"type\":\"web_search\"}]}";
      char model[64] = "";
      char *instructions = NULL;
      struct cJSON *messages = NULL;
      struct cJSON *tools = NULL;
      int stream = -1;
      int rc = openai_parse_responses_to_chat(body, model, sizeof(model), &instructions, &messages,
                                              &tools, &stream);
      assert(rc == 0);
      assert(strcmp(model, "aimee") == 0);
      assert(stream == 1);
      assert(instructions && strcmp(instructions, "be helpful") == 0);

      assert(cJSON_GetArraySize((cJSON *)messages) == 4);
      /* developer -> system */
      cJSON *m0 = cJSON_GetArrayItem((cJSON *)messages, 0);
      assert(strcmp(cJSON_GetObjectItem(m0, "role")->valuestring, "system") == 0);
      assert(strcmp(cJSON_GetObjectItem(m0, "content")->valuestring, "dev note") == 0);
      /* function_call -> assistant tool_calls */
      cJSON *m2 = cJSON_GetArrayItem((cJSON *)messages, 2);
      assert(strcmp(cJSON_GetObjectItem(m2, "role")->valuestring, "assistant") == 0);
      cJSON *tcs = cJSON_GetObjectItem(m2, "tool_calls");
      assert(tcs && cJSON_GetArraySize(tcs) == 1);
      cJSON *tc = cJSON_GetArrayItem(tcs, 0);
      assert(strcmp(cJSON_GetObjectItem(tc, "id")->valuestring, "call_1") == 0);
      assert(strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(tc, "function"), "name")->valuestring,
                    "exec_command") == 0);
      /* function_call_output -> role:tool */
      cJSON *m3 = cJSON_GetArrayItem((cJSON *)messages, 3);
      assert(strcmp(cJSON_GetObjectItem(m3, "role")->valuestring, "tool") == 0);
      assert(strcmp(cJSON_GetObjectItem(m3, "tool_call_id")->valuestring, "call_1") == 0);
      assert(strcmp(cJSON_GetObjectItem(m3, "content")->valuestring, "file.txt") == 0);
      /* tools: only the `function` tool kept, reshaped to chat form; web_search dropped */
      assert(tools && cJSON_GetArraySize((cJSON *)tools) == 1);
      cJSON *t0 = cJSON_GetArrayItem((cJSON *)tools, 0);
      assert(strcmp(cJSON_GetObjectItem(t0, "type")->valuestring, "function") == 0);
      assert(strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(t0, "function"), "name")->valuestring,
                    "exec_command") == 0);

      free(instructions);
      cJSON_Delete((cJSON *)messages);
      cJSON_Delete((cJSON *)tools);
   }

   printf("ok\n");
   return 0;
}
