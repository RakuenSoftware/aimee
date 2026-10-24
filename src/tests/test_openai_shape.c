/* test_openai_shape.c: unit tests for the OpenAI-compatible JSON shaping
 * helpers (pure — no sockets, no network, no agent execution). */
#include "openai_shape.h"
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

   /* --- truncation reaches the client in its own vocabulary --- */
   {
      assert(openai_format_chat_completion_reason("cmpl-t", "aimee", "part", 123, 5, 2, "length",
                                                  resp, sizeof(resp)) > 0);
      assert(strstr(resp, "\"finish_reason\":\"length\""));
      assert(openai_format_response_status("resp-t", "aimee", "part", 123, 5, 2, 0,
                                           "max_output_tokens", resp, sizeof(resp)) > 0);
      assert(strstr(resp, "\"status\":\"incomplete\""));
      assert(strstr(resp, "\"incomplete_details\":{\"reason\":\"max_output_tokens\"}"));
      assert(openai_format_response_status("resp-c", "aimee", "all", 123, 5, 2, 0, NULL, resp,
                                           sizeof(resp)) > 0);
      assert(strstr(resp, "\"status\":\"completed\"") && !strstr(resp, "incomplete_details"));
   }

   /* --- text_completion envelope --- */
   {
      int len = openai_format_text_completion("cmpl-2", "aimee", "hi there", 123, 1, 1, 0, resp,
                                              sizeof(resp));
      assert(len > 0);
      assert(strstr(resp, "\"object\":\"text_completion\""));
      assert(strstr(resp, "\"text\":\"hi there\""));
      assert(openai_format_text_completion("cmpl-unknown", "aimee", "answer", 123, 0, 0, 1, resp,
                                           sizeof(resp)) > 0);
      cJSON *unknown = cJSON_Parse(resp);
      assert(unknown && !cJSON_GetObjectItemCaseSensitive(unknown, "usage"));
      cJSON_Delete(unknown);
      assert(openai_format_text_completion("cmpl-zero", "aimee", "answer", 123, 0, 0, 0, resp,
                                           sizeof(resp)) > 0);
      cJSON *zero = cJSON_Parse(resp);
      const cJSON *usage = cJSON_GetObjectItemCaseSensitive(zero, "usage");
      assert(cJSON_IsObject(usage));
      assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens")));
      assert(cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens")->valuedouble == 0);
      cJSON_Delete(zero);
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
      char diagnostic[512];
      const char *rejected =
          "{\"detail\":\"The 'gpt-6-astra-high' model is not supported when using Codex "
          "with a ChatGPT account.\",\"secret\":\"must-not-be-forwarded\"}";
      openai_upstream_error_message(400, rejected, diagnostic, sizeof(diagnostic));
      assert(strstr(diagnostic, "HTTP 400"));
      assert(strstr(diagnostic, "gpt-6-astra-high"));
      assert(!strstr(diagnostic, "must-not-be-forwarded"));
      int n = openai_format_responses_failed("resp_bad_model", "aimee", 1700000000, "server_error",
                                             diagnostic, resp, sizeof(resp));
      assert(n > 0);
      cJSON *event = cJSON_Parse(resp);
      assert(event);
      cJSON *response = cJSON_GetObjectItem(event, "response");
      assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(response, "status")), "failed") == 0);
      cJSON *error = cJSON_GetObjectItem(response, "error");
      assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(error, "message")), diagnostic) == 0);
      cJSON_Delete(event);

      const int statuses[] = {400, 401, 403, 404, 429, 500, 502, 503};
      for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++)
      {
         openai_upstream_error_message(statuses[i],
                                       "{\"error\":{\"message\":\"provider rejected request\"}}",
                                       diagnostic, sizeof(diagnostic));
         assert(strstr(diagnostic, "provider rejected request"));
      }
      const char *invalid[] = {NULL,
                               "",
                               "<html>private proxy detail</html>",
                               "not JSON",
                               "{}",
                               "{\"detail\":123}",
                               "{\"error\":{\"message\":[]}}"};
      for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
      {
         openai_upstream_error_message(502, invalid[i], diagnostic, sizeof(diagnostic));
         assert(strcmp(diagnostic, "upstream model HTTP 502: no usable error detail") == 0);
      }
      openai_upstream_error_message(-1, rejected, diagnostic, sizeof(diagnostic));
      assert(strcmp(diagnostic, "upstream model transport failed") == 0);
      openai_upstream_error_message(400, "{\"detail\":\"bad\\nmodel\\u001b[31m\"}", diagnostic,
                                    sizeof(diagnostic));
      assert(!strchr(diagnostic, '\n') && !strchr(diagnostic, '\033'));
      char tiny[2] = {'x', 'x'};
      openai_upstream_error_message(400, rejected, tiny, sizeof(tiny));
      assert(tiny[1] == '\0');
      openai_upstream_error_message(400, rejected, tiny, 0);
      openai_upstream_error_message(400, rejected, NULL, 0);

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

   /* Canonical tool output is covered by unit-test-openai-chat-policed using
    * the production translation boundary, not the retired flat helper. */
   printf("ok\n");
   return 0;
}
