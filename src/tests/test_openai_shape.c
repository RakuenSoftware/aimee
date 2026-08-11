/* test_openai_shape.c: unit tests for the OpenAI-compatible JSON shaping
 * helpers (pure — no sockets, no network, no agent execution). */
#include "openai_shape.h"
#include "agent_protocol.h" /* parsed_response_t — openai_shape.h only forward-declares it */
#include "aimee_errors.h"
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
      /* A plain HTTP-semantic error carries NO aimee code. */
      assert(!strstr(resp, "\"code\""));
   }

   /* --- aimee-coded error envelope --- */
   {
      int len = openai_format_error_code(resp, sizeof(resp), "server_error", "no agent configured",
                                         AIMEE_ERR_NO_PRIMARY);
      assert(len > 0);
      assert(strstr(resp, "\"type\":\"server_error\""));
      assert(strstr(resp, "no agent configured"));
      /* aimee-internal faults stamp error.code (>=1000) into the body. */
      assert(strstr(resp, "\"code\":1001"));
      /* code 0 is identical to the plain formatter — no code field. */
      openai_format_error_code(resp, sizeof(resp), "server_error", "x", 0);
      assert(!strstr(resp, "\"code\""));
      /* slug table stays in sync with the enum. */
      assert(strcmp(aimee_err_slug(AIMEE_ERR_NO_PRIMARY), "no_primary") == 0);
      assert(strcmp(aimee_err_slug(AIMEE_ERR_ROUTE_UNRESOLVED), "route_unresolved") == 0);
      assert(strcmp(aimee_err_slug(AIMEE_ERR_BREAKER_OPEN), "breaker_open") == 0);
      assert(strcmp(aimee_err_slug(AIMEE_ERR_CONCURRENCY_LIMIT), "concurrency_limit") == 0);
      assert(strcmp(aimee_err_slug(AIMEE_ERR_REQUEST_PIPELINE), "request_pipeline") == 0);
      assert(strcmp(aimee_err_slug(4242), "unknown") == 0);
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
      int len = openai_format_response("resp_42", "aimee", "the answer", 1700000000, 7, 3, 0, resp,
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

      len = openai_format_responses_completed("resp_9", "aimee", "the answer", 1700000000, 7, 3, 0,
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
      int len = openai_format_responses_fc_item_added("resp_9-fc-0", "call_42", "exec_command",
                                                      NULL, 0, resp, sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"type\":\"response.output_item.added\""));
      assert(strstr(resp, "\"type\":\"function_call\""));
      assert(strstr(resp, "\"call_id\":\"call_42\""));
      assert(strstr(resp, "\"name\":\"exec_command\""));
      /* A plain tool has no group: the key must be absent, not empty. */
      assert(!strstr(resp, "\"namespace\""));

      len = openai_format_responses_fc_args_delta("resp_9-fc-0", 0, "{\"cmd\":\"ls\"}", resp,
                                                  sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"type\":\"response.function_call_arguments.delta\""));
      assert(strstr(resp, "\"item_id\":\"resp_9-fc-0\""));

      len = openai_format_responses_fc_args_done("resp_9-fc-0", 0, "{\"cmd\":\"ls\"}", resp,
                                                 sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"type\":\"response.function_call_arguments.done\""));

      len = openai_format_responses_fc_item_done("resp_9-fc-0", "call_42", "exec_command", NULL,
                                                 "{\"cmd\":\"ls\"}", 0, resp, sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"type\":\"response.output_item.done\""));
      assert(strstr(resp, "\"type\":\"function_call\""));
      assert(strstr(resp, "\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\""));
      assert(!strstr(resp, "\"namespace\""));
   }

   /* --- A namespaced call keeps its group on BOTH item events ---
    *
    * A Codex client offers its MCP tools inside a `namespace` group; the provider
    * answers with the nested name BARE and the group beside it. The client routes
    * on the pair, so dropping the group makes the call unroutable -- it answers
    * "unsupported call: git". Both events carry it, because the client reads the
    * name from `added` before the arguments stream. */
   {
      int len = openai_format_responses_fc_item_added("resp_9-fc-0", "call_7", "git", "mcp__aimee",
                                                      0, resp, sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"name\":\"git\""));
      assert(strstr(resp, "\"namespace\":\"mcp__aimee\""));

      len = openai_format_responses_fc_item_done("resp_9-fc-0", "call_7", "git", "mcp__aimee",
                                                 "{\"command\":\"status\"}", 0, resp, sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"name\":\"git\""));
      assert(strstr(resp, "\"namespace\":\"mcp__aimee\""));

      /* Empty is the same as absent -- it must not claim a group. */
      len = openai_format_responses_fc_item_done("resp_9-fc-0", "call_7", "git", "",
                                                 "{\"command\":\"status\"}", 0, resp, sizeof(resp));
      assert(len > 0);
      assert(!strstr(resp, "\"namespace\""));
   }

   /* --- Codex parity: completed with a function_call output item --- */
   {
      struct cJSON *out = cJSON_CreateArray();
      cJSON_AddItemToArray((cJSON *)out, openai_responses_function_call_item(
                                             "resp_9-fc-0", "call_42", "exec_command", NULL,
                                             "{\"cmd\":\"ls\"}", "completed"));
      int len = openai_format_responses_completed_items("resp_9", "aimee", 1700000000, out, 5, 2, 4,
                                                        resp, sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"type\":\"response.completed\""));
      assert(strstr(resp, "\"type\":\"function_call\""));
      assert(strstr(resp, "\"call_id\":\"call_42\""));
      assert(strstr(resp, "\"total_tokens\":7"));
      /* The tool-call turn is the one the Codex gateway emits most, and it is
       * where the cached count went missing on the wire. */
      assert(strstr(resp, "\"input_tokens_details\":{\"cached_tokens\":4}"));
   }

   /* --- Cached input tokens survive to the client on every usage-bearing shape.
    *
    * A client bills what these blocks say. When the cached count is dropped, the
    * conversation is priced at the full uncached rate -- about 10x on the cached
    * portion -- and the symptom is indistinguishable from prompt caching being
    * broken. Asserted on each emitter separately because each builds its own
    * usage object, which is exactly how one path kept the field and the others
    * never had it. Zero is asserted as PRESENT, not absent: a client must be able
    * to tell a real cache miss from a gateway that does not report caching. --- */
   {
      int len = openai_format_responses_completed("resp_c", "aimee", "hi", 1700000000, 100, 5, 80,
                                                  resp, sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"input_tokens\":100"));
      assert(strstr(resp, "\"input_tokens_details\":{\"cached_tokens\":80}"));

      len = openai_format_response("resp_d", "aimee", "hi", 1700000000, 100, 5, 0, resp,
                                   sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"input_tokens_details\":{\"cached_tokens\":0}"));

      len = openai_format_run("run_e", "aimee", "hi", 1700000000, 100, 5, 64, "completed", resp,
                              sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"input_tokens_details\":{\"cached_tokens\":64}"));
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

   /* --- split a chat request into instructions / messages / tools ---
    * The tool relay on /v1/chat/completions depends on this: an agentic client
    * sends its own tools and executes the calls itself, so the loop only
    * converges if assistant `tool_calls` and `role:"tool"` turns survive the
    * round trip verbatim. */
   {
      char *instructions = NULL;
      struct cJSON *messages = NULL, *tools = NULL;
      int rc = openai_split_chat_request(
          "{\"model\":\"aimee\",\"messages\":["
          "{\"role\":\"system\",\"content\":\"be terse\"},"
          "{\"role\":\"user\",\"content\":\"weather in Paris?\"},"
          "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"c1\","
          "\"type\":\"function\",\"function\":{\"name\":\"get_weather\",\"arguments\":\"{}\"}}]},"
          "{\"role\":\"tool\",\"tool_call_id\":\"c1\",\"content\":\"12C\"}],"
          "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"get_weather\"}},"
          "{\"type\":\"web_search\"}]}",
          &instructions, &messages, &tools);
      assert(rc == 0);
      /* leading system turn lifted out of band */
      assert(instructions && strcmp(instructions, "be terse") == 0);
      assert(cJSON_GetArraySize((cJSON *)messages) == 3);
      /* the tool loop's two special turns round-trip untouched */
      cJSON *m1 = cJSON_GetArrayItem((cJSON *)messages, 1);
      assert(cJSON_GetObjectItem(m1, "tool_calls"));
      cJSON *m2 = cJSON_GetArrayItem((cJSON *)messages, 2);
      assert(strcmp(cJSON_GetObjectItem(m2, "role")->valuestring, "tool") == 0);
      assert(strcmp(cJSON_GetObjectItem(m2, "tool_call_id")->valuestring, "c1") == 0);
      /* only function tools survive */
      assert(tools && cJSON_GetArraySize((cJSON *)tools) == 1);
      free(instructions);
      cJSON_Delete((cJSON *)messages);
      cJSON_Delete((cJSON *)tools);
   }

   /* Absent (or wholly unsupported) tools must yield NULL, never `tools: []` —
    * an empty array is a different request to a provider than no array. */
   {
      char *instructions = NULL;
      struct cJSON *messages = NULL, *tools = NULL;
      assert(openai_split_chat_request("{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
                                       &instructions, &messages, &tools) == 0);
      assert(tools == NULL && instructions == NULL);
      cJSON_Delete((cJSON *)messages);

      assert(openai_split_chat_request("{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
                                       "\"tools\":[{\"type\":\"web_search\"}]}",
                                       &instructions, &messages, &tools) == 0);
      assert(tools == NULL);
      cJSON_Delete((cJSON *)messages);

      /* malformed and empty are rejected without handing back partial output */
      messages = tools = NULL;
      instructions = NULL;
      assert(openai_split_chat_request("not json", &instructions, &messages, &tools) == -1);
      assert(openai_split_chat_request("{\"messages\":[]}", &instructions, &messages, &tools) ==
             -1);
      assert(!instructions && !messages && !tools);
   }

   /* --- format a tool-call turn in chat.completion shape --- */
   {
      parsed_response_t p;
      memset(&p, 0, sizeof(p));
      p.is_tool_call = 1;
      p.call_count = 2;
      snprintf(p.calls[0].id, sizeof(p.calls[0].id), "call_a");
      snprintf(p.calls[0].name, sizeof(p.calls[0].name), "get_weather");
      p.calls[0].arguments = strdup("{\"city\":\"Paris\"}");
      snprintf(p.calls[1].id, sizeof(p.calls[1].id), "call_b");
      snprintf(p.calls[1].name, sizeof(p.calls[1].name), "get_time");
      p.calls[1].arguments = NULL; /* must degrade to "{}", not null */
      p.prompt_tokens = 10;
      p.completion_tokens = 5;

      int n = openai_format_chat_completion_tool_calls("chatcmpl-1", "aimee", &p, 1700000000, resp,
                                                       (int)sizeof(resp));
      assert(n > 0);
      cJSON *root = cJSON_Parse(resp);
      assert(root);
      cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "choices"), 0);
      /* the field agentic clients branch on before they will execute a tool */
      assert(strcmp(cJSON_GetObjectItem(choice, "finish_reason")->valuestring, "tool_calls") == 0);
      cJSON *msg = cJSON_GetObjectItem(choice, "message");
      assert(cJSON_IsNull(cJSON_GetObjectItem(msg, "content")));
      cJSON *tcs = cJSON_GetObjectItem(msg, "tool_calls");
      assert(cJSON_GetArraySize(tcs) == 2);
      cJSON *fn0 = cJSON_GetObjectItem(cJSON_GetArrayItem(tcs, 0), "function");
      assert(strcmp(cJSON_GetObjectItem(fn0, "name")->valuestring, "get_weather") == 0);
      /* `arguments` is a JSON *string* on the wire, not a nested object */
      assert(cJSON_IsString(cJSON_GetObjectItem(fn0, "arguments")));
      assert(strcmp(cJSON_GetObjectItem(fn0, "arguments")->valuestring, "{\"city\":\"Paris\"}") ==
             0);
      cJSON *fn1 = cJSON_GetObjectItem(cJSON_GetArrayItem(tcs, 1), "function");
      assert(strcmp(cJSON_GetObjectItem(fn1, "arguments")->valuestring, "{}") == 0);
      cJSON_Delete(root);
      free(p.calls[0].arguments);
   }

   /* Prose emitted alongside the calls is preserved rather than discarded. */
   {
      parsed_response_t p;
      memset(&p, 0, sizeof(p));
      p.is_tool_call = 1;
      p.call_count = 1;
      p.content = (char *)"let me check";
      snprintf(p.calls[0].id, sizeof(p.calls[0].id), "c1");
      snprintf(p.calls[0].name, sizeof(p.calls[0].name), "f");
      assert(openai_format_chat_completion_tool_calls("i", "aimee", &p, 1, resp,
                                                      (int)sizeof(resp)) > 0);
      cJSON *root = cJSON_Parse(resp);
      cJSON *msg = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(root, "choices"), 0),
                                       "message");
      assert(strcmp(cJSON_GetObjectItem(msg, "content")->valuestring, "let me check") == 0);
      cJSON_Delete(root);
   }

   /* Nothing to report, or nowhere to put it, are both refusals. */
   {
      parsed_response_t p;
      memset(&p, 0, sizeof(p));
      assert(openai_format_chat_completion_tool_calls("i", "m", &p, 1, resp, (int)sizeof(resp)) ==
             -1);
      p.call_count = 1;
      char tiny[8];
      assert(openai_format_chat_completion_tool_calls("i", "m", &p, 1, tiny, (int)sizeof(tiny)) ==
             -1);
   }

   /* --- streaming: tool calls as SSE deltas + a tool_calls finish frame ---
    * Agentic clients stream by default, so the buffered shape above is not
    * enough on its own. */
   {
      parsed_response_t p;
      memset(&p, 0, sizeof(p));
      p.is_tool_call = 1;
      p.call_count = 2;
      snprintf(p.calls[0].id, sizeof(p.calls[0].id), "call_a");
      snprintf(p.calls[0].name, sizeof(p.calls[0].name), "get_weather");
      p.calls[0].arguments = strdup("{\"city\":\"Paris\"}");
      snprintf(p.calls[1].id, sizeof(p.calls[1].id), "call_b");
      snprintf(p.calls[1].name, sizeof(p.calls[1].name), "get_time");

      assert(openai_format_chat_chunk_tool_calls("id1", "aimee", 1700000000, &p, resp,
                                                 (int)sizeof(resp)) > 0);
      cJSON *root = cJSON_Parse(resp);
      assert(root);
      cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "choices"), 0);
      /* a delta frame must NOT terminate the turn */
      assert(cJSON_IsNull(cJSON_GetObjectItem(choice, "finish_reason")));
      cJSON *tcs = cJSON_GetObjectItem(cJSON_GetObjectItem(choice, "delta"), "tool_calls");
      assert(cJSON_GetArraySize(tcs) == 2);
      /* clients accumulate by index, so it must be present and ordered */
      assert(cJSON_GetObjectItem(cJSON_GetArrayItem(tcs, 0), "index")->valuedouble == 0);
      assert(cJSON_GetObjectItem(cJSON_GetArrayItem(tcs, 1), "index")->valuedouble == 1);
      cJSON *fn1 = cJSON_GetObjectItem(cJSON_GetArrayItem(tcs, 1), "function");
      assert(strcmp(cJSON_GetObjectItem(fn1, "arguments")->valuestring, "{}") == 0);
      cJSON_Delete(root);
      free(p.calls[0].arguments);

      /* the terminal frame must say tool_calls, not stop */
      assert(openai_format_chat_chunk_finish("id1", "aimee", 1700000000, "tool_calls", resp,
                                             (int)sizeof(resp)) > 0);
      root = cJSON_Parse(resp);
      choice = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "choices"), 0);
      assert(strcmp(cJSON_GetObjectItem(choice, "finish_reason")->valuestring, "tool_calls") == 0);
      cJSON_Delete(root);

      /* default reason, and a refusal when there is nothing to send */
      assert(openai_format_chat_chunk_finish("i", "m", 1, NULL, resp, (int)sizeof(resp)) > 0);
      assert(strstr(resp, "\"finish_reason\":\"stop\""));
      p.call_count = 0;
      assert(openai_format_chat_chunk_tool_calls("i", "m", 1, &p, resp, (int)sizeof(resp)) == -1);
   }

   printf("ok\n");
   return 0;
}
