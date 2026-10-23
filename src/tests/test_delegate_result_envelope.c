/* Exercise the actual private envelope builder, without exporting a test API
 * or starting a delegate. Unused compute handlers are discarded at link time. */
#include "../server/server_compute.c"
#include <assert.h>

int db1_token_audit_cost_for_delegation_ex(const char *id, double *cost)
{
   (void)id;
   (void)cost;
   return -1;
}
int db1_delegation_spawn_stop_reason(const char *id, char *out, size_t cap)
{
   (void)id;
   if (cap)
      out[0] = 0;
   return 0;
}
int delegation_error_guidance(const char *error, char *out, size_t cap)
{
   (void)error;
   if (cap)
      out[0] = 0;
   return 0;
}
void delegate_economics_add_agent_result_json(cJSON *out, const agent_config_t *config,
                                              const char *role, const agent_result_t *result,
                                              const agent_t *fallback)
{
   (void)out;
   (void)config;
   (void)role;
   (void)result;
   (void)fallback;
}
void delegate_handoff_add_validation_json(cJSON *out, const delegate_handoff_validation_t *value)
{
   (void)value;
   cJSON_AddBoolToObject(out, "handoff_checked", 1);
}

static cJSON *envelope(agent_result_t *result, int rc, const char *apply_error)
{
   cJSON *expected = aimee_ir_response_json(&result->canonical_response);
   char *before = expected ? cJSON_PrintUnformatted(expected) : NULL;
   cJSON *out = delegate_build_result_response("owned-delegation", rc, result, NULL, "reviewer",
                                               NULL, 0, 0, NULL, apply_error);
   assert(out);
   const cJSON *ir = cJSON_GetObjectItemCaseSensitive(out, "ir");
   char *after = ir ? cJSON_PrintUnformatted(ir) : NULL;
   assert((!before && !after) || (before && after && !strcmp(before, after)));
   free(before);
   free(after);
   cJSON_Delete(expected);
   assert(!strcmp(result->response, "poisoned display alias"));
   char *wire = cJSON_PrintUnformatted(out);
   assert(wire && !strstr(wire, "poisoned display alias"));
   free(wire);
   return out;
}
static void require_status(cJSON *out, const char *status, const char *text)
{
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(out, "status")), status));
   const char *display = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(out, "response"));
   assert(display && !strcmp(display, text));
   assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(out, "cost_known")));
   assert(!cJSON_HasObjectItem(out, "cost_usd"));
   cJSON_Delete(out);
}
int main(void)
{
   aimee_block_t blocks[] = {
       {.type = AIMEE_BLK_TEXT, .text = "canonical visible answer"},
       {.type = AIMEE_BLK_THINKING, .text = "private thinking is not display"},
       {.type = AIMEE_BLK_IMAGE,
        .media_type = "image/png",
        .media_ref = "https://example.invalid/image"},
       {.type = AIMEE_BLK_DOCUMENT,
        .media_type = "application/pdf",
        .media_ref = "https://example.invalid/doc"}};
   agent_result_t result = {.success = 1, .response = "poisoned display alias"};
   blocks[2].raw = cJSON_Parse("{\"opaque_vendor_extension\":{\"x\":3}}");
   assert(blocks[2].raw);
   result.canonical_response = (aimee_response_t){.role = "assistant",
                                                  .content = blocks,
                                                  .n_content = 4,
                                                  .stop_reason = AIMEE_STOP_END_TURN,
                                                  .raw_stop_reason = "end_turn"};
   result.canonical_response.content = &blocks[2];
   result.canonical_response.n_content = 1;
   cJSON *media = delegate_build_result_response("owned-delegation", 0, &result, NULL, "reviewer",
                                                 NULL, 0, 0, NULL, "");
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(media, "status")), "ok"));
   cJSON_Delete(media);
   result.canonical_response.content = blocks;
   result.canonical_response.n_content = 4;
   require_status(envelope(&result, 0, ""), "ok", "canonical visible answer");
   for (int i = 2; i <= 3; i++)
   {
      result.canonical_response.content = &blocks[i];
      result.canonical_response.n_content = 1;
      require_status(envelope(&result, 0, ""), "ok", "");
   }
   result.canonical_response.content = blocks;
   result.canonical_response.n_content = 2;
   require_status(envelope(&result, 0, ""), "ok", "canonical visible answer");
   const aimee_stop_reason_t failures[] = {AIMEE_STOP_MAX_TOKENS, AIMEE_STOP_UNKNOWN,
                                           AIMEE_STOP_CONTENT_FILTER, AIMEE_STOP_ERROR};
   for (size_t i = 0; i < sizeof failures / sizeof failures[0]; i++)
   {
      result.canonical_response.stop_reason = failures[i];
      result.canonical_response.raw_stop_reason = i == 2 ? "refusal" : "incomplete";
      result.canonical_response.error_message = i == 3 ? "retained API error" : NULL;
      require_status(envelope(&result, 0, ""), "error", "canonical visible answer");
   }
   result.canonical_response.error_message = NULL;
   aimee_refusal_details_t refusal = {.category = "bounded-category",
                                      .explanation = "retained detail"};
   result.canonical_response.stop_reason = AIMEE_STOP_CONTENT_FILTER;
   result.canonical_response.raw_stop_reason = "refusal";
   result.canonical_response.refusal = &refusal;
   require_status(envelope(&result, 0, ""), "error", "canonical visible answer");
   result.canonical_response.refusal = NULL;
   aimee_block_t pending = {.type = AIMEE_BLK_TOOL_USE,
                            .tool_id = "unexecuted-call",
                            .tool_name = "read_file",
                            .tool_input = cJSON_CreateObject()};
   assert(pending.tool_input);
   result.canonical_response.content = &pending;
   result.canonical_response.n_content = 1;
   result.canonical_response.stop_reason = AIMEE_STOP_TOOL_USE;
   result.canonical_response.raw_stop_reason = "tool_use";
   require_status(envelope(&result, 0, ""), "error", "");
   result.canonical_response.stop_reason = AIMEE_STOP_END_TURN;
   result.canonical_response.raw_stop_reason = "end_turn";
   require_status(envelope(&result, 0, ""), "error", "");
   cJSON_Delete(pending.tool_input);
   result.canonical_response.content = blocks;
   result.canonical_response.n_content = 2;
   result.success = 0;
   snprintf(result.error, sizeof result.error, "recorded host failure");
   cJSON *out = envelope(&result, 0, "retained apply failure");
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(out, "message")),
                  "recorded host failure"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(out, "apply_error")),
                  "retained apply failure"));
   require_status(out, "error", "canonical visible answer");
   result.success = 1;
   result.canonical_response.content = NULL;
   result.canonical_response.n_content = 1; /* malformed IR cannot expose a stale display */
   out = envelope(&result, 0, "");
   assert(!cJSON_HasObjectItem(out, "ir") && !cJSON_HasObjectItem(out, "response"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(out, "status")), "error"));
   cJSON_Delete(out);
   cJSON_Delete(blocks[2].raw);
   puts("delegate result envelope: complete media preserved; display is canonical TEXT only");
   return 0;
}
