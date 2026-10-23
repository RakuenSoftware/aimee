/* Real private public-run adapter/renderer; no executor/provider starts. */
#include "../server/openai_chat.c"
#include <assert.h>

static int incomplete_events, completed_events, failed_events, text_events;
static void capture_sse(void *context, const char *event, const char *wire)
{
   (void)context;
   if (!strcmp(event, "response.incomplete"))
   {
      incomplete_events++;
      cJSON *frame = cJSON_Parse(wire);
      assert(frame);
      const cJSON *response = cJSON_GetObjectItemCaseSensitive(frame, "response");
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "status")),
                     "incomplete"));
      cJSON_Delete(frame);
   }
   completed_events += !strcmp(event, "response.completed");
   failed_events += !strcmp(event, "response.failed");
   text_events += !strcmp(event, "response.output_text.delta");
}
static void denied(int rc, agent_result_t *result)
{
   aimee_response_t output = {.model = strdup("untouched")};
   assert(output.model);
   assert(run_project_terminal_response(rc, result, &output) == -1);
   assert(!strcmp(output.model, "untouched") && !output.n_content);
   aimee_response_free(&output);
}
int main(void)
{
   aimee_block_t text = {.type = AIMEE_BLK_TEXT, .text = "original partial answer"};
   agent_result_t result = {.success = 0,
                            .response = "stale display is not public output",
                            .prompt_tokens = 31,
                            .completion_tokens = 13,
                            .cache_read_tokens = 5,
                            .cache_write_tokens = 2};
   snprintf(result.stop_reason, sizeof result.stop_reason, "%s",
            aimee_stop_reason_name(AIMEE_STOP_MAX_TOKENS));
   snprintf(result.error, sizeof result.error,
            "model returned a declined or incomplete canonical observation");
   result.canonical_response = (aimee_response_t){.role = "assistant",
                                                  .content = &text,
                                                  .n_content = 1,
                                                  .stop_reason = AIMEE_STOP_MAX_TOKENS,
                                                  .raw_stop_reason = "max_tokens",
                                                  .usage_in = 11,
                                                  .usage_out = 3,
                                                  .usage_cache_read = 1,
                                                  .usage_cache_write = 1};
   cJSON *source = aimee_ir_response_json(&result.canonical_response);
   char *before = cJSON_PrintUnformatted(source);
   cJSON_Delete(source);
   assert(before);
   aimee_response_t output = {
       0}; /* The runtime intentionally leaves canonical_out empty on failure. */
   assert(!run_project_terminal_response(-1, &result, &output));
   assert(!result.success && aimee_ir_response_complete_valid(&result.canonical_response) < 0);
   assert(output.stop_reason == AIMEE_STOP_MAX_TOKENS && output.content != &text);
   assert(!strcmp(output.content[0].text, text.text));
   assert(output.usage_in == 31 && output.usage_out == 13 && output.usage_cache_read == 5);
   source = aimee_ir_response_json(&result.canonical_response);
   char *after = cJSON_PrintUnformatted(source);
   assert(after && !strcmp(before, after));
   free(before);
   free(after);
   cJSON_Delete(source);
   run_job_t job = {0};
   snprintf(job.run_id, sizeof job.run_id, "run_owned-partial");
   snprintf(job.canonical_session, sizeof job.canonical_session, "canonical-owned-source");
   job.request.model = "selected-registration";
   cJSON *object = run_object(&job, NULL, &output, NULL);
   assert(object);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, "status")),
                  "incomplete"));
   const cJSON *details = cJSON_GetObjectItemCaseSensitive(object, "incomplete_details");
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(details, "reason")),
                  "max_output_tokens"));
   assert(!strcmp(
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, "canonical_session_id")),
       "canonical-owned-source"));
   const cJSON *usage = cJSON_GetObjectItemCaseSensitive(object, "usage");
   assert(cJSON_GetObjectItemCaseSensitive(usage, "input_tokens")->valueint == 31);
   assert(cJSON_GetObjectItemCaseSensitive(usage, "output_tokens")->valueint == 13);
   char *wire = cJSON_PrintUnformatted(object);
   assert(wire && strstr(wire, text.text) && !strstr(wire, result.response));
   assert(!cJSON_HasObjectItem(object, "error"));
   free(wire);
   cJSON_Delete(object);
   assert(!aimee_openai_emit_responses_tail(&output, job.run_id, job.request.model, 1, capture_sse,
                                            NULL));
   assert(incomplete_events == 1 && !completed_events && !failed_events && text_events);
   aimee_response_free(&output);
   denied(AGENT_RC_CONTEXT_REFUSED, &result);
   denied(AGENT_RC_CAPTURE_FAILED, &result);
   denied(AGENT_RC_AT_LIMIT, &result);
   snprintf(result.stop_reason, sizeof result.stop_reason, "capture_failed");
   denied(-1, &result);
   snprintf(result.stop_reason, sizeof result.stop_reason, "%s",
            aimee_stop_reason_name(AIMEE_STOP_MAX_TOKENS));
   result.canonical_response.error_message = "provider transport failure";
   denied(-1, &result);
   result.canonical_response.error_message = NULL;
   result.canonical_response.stop_reason = AIMEE_STOP_CONTENT_FILTER;
   result.canonical_response.raw_stop_reason = "refusal";
   denied(-1, &result);
   result.canonical_response.stop_reason = AIMEE_STOP_MAX_TOKENS;
   result.canonical_response.raw_stop_reason = "max_tokens";
   aimee_block_t call = {.type = AIMEE_BLK_TOOL_USE,
                         .tool_id = "not-dispatched",
                         .tool_name = "write_file",
                         .tool_input = cJSON_CreateObject()};
   result.canonical_response.content = &call;
   denied(-1, &result);
   cJSON_Delete(call.tool_input);
   result.canonical_response.content = NULL;
   denied(-1, &result); /* malformed observation */
   result.canonical_response.content = &text;
   result.canonical_response.stop_reason = AIMEE_STOP_END_TURN;
   result.canonical_response.raw_stop_reason = "end_turn";
   denied(-1, &result); /* A host failure cannot become a completed response. */
   result.success = 1;
   assert(!run_project_terminal_response(0, &result, &output));
   assert(output.stop_reason == AIMEE_STOP_END_TURN && !strcmp(output.content[0].text, text.text));
   aimee_response_free(&output);
   puts("public run terminal: MAX remains incomplete, canonical output preserved, business failure "
        "unchanged");
   return 0;
}
