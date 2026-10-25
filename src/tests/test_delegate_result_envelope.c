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

/* Exercise both real terminal consumers against one captured durable write. */
static char recorded_status[32], recorded_result[4096];
static int recorded_cursor, recorded_has_cost, coord_done, coord_failed, coord_notified;
static double recorded_cost;
static void record_result(const char *status, const char *result)
{
   snprintf(recorded_status, sizeof recorded_status, "%s", status);
   snprintf(recorded_result, sizeof recorded_result, "%s", result ? result : "");
}
int db1_agent_job_complete(int id, const char *status, int cursor, const char *result, int has_cost,
                           double cost)
{
   assert(id == 7);
   record_result(status, result);
   recorded_cursor = cursor;
   recorded_has_cost = has_cost;
   recorded_cost = cost;
   return 0;
}
int db1_agent_job_get(int id, db1_agent_job_t *job)
{
   assert(id == 7);
   memset(job, 0, sizeof *job);
   job->cursor_turn = 17;
   return 0;
}
void db1_agent_job_free(db1_agent_job_t *job)
{
   (void)job;
}
int db1_coord_job_complete_task_owned(int id, const char *owner, const char *result)
{
   assert(id == 8 && !strcmp(owner, "claim-owner"));
   coord_done++;
   record_result("done", result);
   return 0;
}
int db1_coord_job_fail_task_owned(int id, const char *owner, const char *error)
{
   assert(id == 8 && !strcmp(owner, "claim-owner"));
   coord_failed++;
   record_result("failed", error);
   return 0;
}
int db1_coord_job_release_task_bounded_owned(int id, const char *owner, int maximum)
{
   assert(id == 8 && !strcmp(owner, "claim-owner") && maximum == 3);
   return 0;
}
int config_concurrency_preempt_requeue_max(void)
{
   return 3;
}
void server_coord_dispatcher_notify(void)
{
   coord_notified++;
}
void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
{
   (void)level;
   (void)module;
   (void)fmt;
}
static cJSON *terminal_envelope(const aimee_response_t *ir, const char *status, const char *message)
{
   cJSON *out = cJSON_CreateObject();
   assert(out);
   cJSON_AddStringToObject(out, "status", status);
   cJSON_AddStringToObject(out, "response", "POISONED flat answer");
   if (message)
      cJSON_AddStringToObject(out, "message", message);
   if (ir)
   {
      cJSON *value = aimee_ir_response_json(ir);
      assert(value);
      cJSON_AddItemToObject(out, "ir", value);
   }
   cJSON_AddNumberToObject(out, "turns", 2);
   cJSON_AddNumberToObject(out, "tool_calls", 1);
   cJSON_AddBoolToObject(out, "cost_known", 1);
   cJSON_AddNumberToObject(out, "cost_usd", 0.25);
   return out;
}
static void require_consumers(cJSON *out, const char *background, int done, const char *contains)
{
   char *before = cJSON_PrintUnformatted(out);
   assert(before);
   compute_ctx_t ctx = {.background_job_id = 7, .coord_task_id = 8};
   snprintf(ctx.coord_claim_owner, sizeof ctx.coord_claim_owner, "claim-owner");
   coord_done = coord_failed = coord_notified = 0;
   compute_update_background_job(&ctx, out);
   assert(!strcmp(recorded_status, background));
   assert(!strstr(recorded_result, "POISONED"));
   if (contains)
      assert(strstr(recorded_result, contains));
   assert(recorded_cursor == cJSON_GetObjectItemCaseSensitive(out, "turns")->valueint &&
          recorded_has_cost && recorded_cost == 0.25);
   compute_update_coord_task(&ctx, out);
   assert(coord_done == done && coord_failed == !done && coord_notified == 1);
   assert(!strstr(recorded_result, "POISONED"));
   if (contains)
      assert(strstr(recorded_result, contains));
   char *after = cJSON_PrintUnformatted(out);
   assert(after && !strcmp(before, after));
   free(before);
   free(after);
   cJSON_Delete(out);
}
static void test_terminal_consumers(void)
{
   /* Baseline RED: a flat status=ok/response can grant durable completion. */
   require_consumers(terminal_envelope(NULL, "ok", NULL), "failed", 0, "canonical");
   aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = "Canonical durable answer"};
   aimee_response_t ir = {.role = "assistant",
                          .content = &block,
                          .n_content = 1,
                          .stop_reason = AIMEE_STOP_END_TURN,
                          .raw_stop_reason = "end_turn"};
   require_consumers(terminal_envelope(&ir, "ok", NULL), "done", 1, block.text);
   cJSON *poison = terminal_envelope(&ir, "ok", NULL);
   cJSON_ReplaceItemInObjectCaseSensitive(poison, "response",
                                          cJSON_CreateString("<tool_call>{bad}</tool_call>"));
   require_consumers(poison, "done", 1, block.text);
   cJSON *malformed = terminal_envelope(&ir, "ok", NULL);
   cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(malformed, "ir"),
                                          "schema", cJSON_CreateString("unknown"));
   require_consumers(malformed, "failed", 0, "canonical");
   require_consumers(terminal_envelope(&ir, "error", "SDK execution failed"), "failed", 0,
                     "SDK execution failed");
   require_consumers(terminal_envelope(NULL, "error", "source lineage refused"), "failed", 0,
                     "source lineage refused");
   cJSON *empty = terminal_envelope(NULL, "error", "native launch refused");
   cJSON_ReplaceItemInObjectCaseSensitive(empty, "response", cJSON_CreateString(""));
   require_consumers(empty, "failed", 0, "native launch refused");
   ir.stop_reason = AIMEE_STOP_MAX_TOKENS;
   ir.raw_stop_reason = "max_tokens";
   require_consumers(terminal_envelope(&ir, "error", "budget exhausted"), "partial", 0,
                     "Canonical durable answer");
   require_consumers(terminal_envelope(&ir, "ok", NULL), "failed", 0, "canonical");
   ir.error_message = "MAX with canonical error";
   require_consumers(terminal_envelope(&ir, "error", "not partial"), "failed", 0, "not partial");
   ir.error_message = NULL;
   ir.stop_reason = AIMEE_STOP_CONTENT_FILTER;
   ir.raw_stop_reason = "refusal";
   aimee_refusal_details_t refusal = {.category = "provider-category", .explanation = "diagnostic"};
   ir.refusal = &refusal;
   require_consumers(terminal_envelope(&ir, "error", "provider refused"), "failed", 0,
                     "provider refused");
   ir.refusal = NULL;
   ir.stop_reason = AIMEE_STOP_ERROR;
   ir.raw_stop_reason = "error";
   ir.error_message = "canonical API failure";
   require_consumers(terminal_envelope(&ir, "ok", NULL), "failed", 0, "canonical API failure");
   ir.error_message = NULL;
   ir.stop_reason = AIMEE_STOP_MAX_TOKENS;
   ir.raw_stop_reason = "max_tokens";
   block = (aimee_block_t){.type = AIMEE_BLK_TOOL_USE,
                           .tool_id = "pending",
                           .tool_name = "read_file",
                           .tool_input = cJSON_CreateObject()};
   require_consumers(terminal_envelope(&ir, "error", "tool pending"), "failed", 0, "tool pending");
   ir.stop_reason = AIMEE_STOP_END_TURN;
   ir.raw_stop_reason = "end_turn";
   require_consumers(terminal_envelope(&ir, "ok", NULL), "failed", 0, "canonical");
   cJSON_Delete(block.tool_input);
   ir.stop_reason = AIMEE_STOP_MAX_TOKENS;
   ir.raw_stop_reason = "max_tokens";
   block = (aimee_block_t){.type = AIMEE_BLK_THINKING, .text = "not authored answer"};
   require_consumers(terminal_envelope(&ir, "error", "reasoning only"), "failed", 0,
                     "reasoning only");
   block = (aimee_block_t){.type = AIMEE_BLK_IMAGE,
                           .media_type = "image/png",
                           .media_ref = "https://example.invalid/image"};
   ir.stop_reason = AIMEE_STOP_END_TURN;
   ir.raw_stop_reason = "end_turn";
   require_consumers(terminal_envelope(&ir, "ok", NULL), "done", 1, "");
   block = (aimee_block_t){.type = AIMEE_BLK_TEXT,
                           .text = "<tool_call>{\"name\":\"read_file\"}</tool_call>"};
   require_consumers(terminal_envelope(&ir, "ok", NULL), "failed", 0, "degenerate");
   block.text = "I'll inspect the repository.\n```sh\nrg TODO .\n```";
   cJSON *plan = terminal_envelope(&ir, "ok", NULL);
   cJSON_ReplaceItemInObjectCaseSensitive(plan, "turns", cJSON_CreateNumber(0));
   cJSON_ReplaceItemInObjectCaseSensitive(plan, "tool_calls", cJSON_CreateNumber(0));
   require_consumers(plan, "failed", 0, "unexecuted");
   require_consumers(terminal_envelope(NULL, "cancelled", "operator cancelled"), "cancelled", 0,
                     "operator cancelled");
   cJSON *cursor = terminal_envelope(NULL, "error", "before model");
   cJSON_DeleteItemFromObjectCaseSensitive(cursor, "turns");
   compute_ctx_t ctx = {.background_job_id = 7};
   compute_update_background_job(&ctx, cursor);
   assert(recorded_cursor == 17);
   cJSON_Delete(cursor);
   cJSON *unknown_cost = terminal_envelope(NULL, "error", "before model");
   cJSON_ReplaceItemInObjectCaseSensitive(unknown_cost, "cost_known", cJSON_CreateBool(0));
   compute_update_background_job(&ctx, unknown_cost);
   assert(!recorded_has_cost && recorded_cost == 0);
   cJSON_Delete(unknown_cost);
   cJSON *preempt = terminal_envelope(NULL, "preempted", NULL);
   ctx.coord_task_id = 8;
   snprintf(ctx.coord_claim_owner, sizeof ctx.coord_claim_owner, "claim-owner");
   coord_done = coord_failed = coord_notified = 0;
   compute_update_coord_task(&ctx, preempt);
   assert(!coord_done && !coord_failed && coord_notified == 1);
   cJSON_Delete(preempt);
}
/* The real worker uses this projection before drift/handoff checks. */
static void worker_projection(const aimee_response_t *ir, int success, int handoff, int admitted,
                              const char *display)
{
   agent_result_t result = {.success = success, .response = strdup("POISONED display")};
   assert(result.response);
   result.canonical_response = *ir;
   cJSON *snapshot = aimee_ir_response_json(ir);
   assert(snapshot);
   char *before = cJSON_PrintUnformatted(snapshot);
   cJSON_Delete(snapshot);
   int rc = delegate_project_completed_result(&result, handoff);
   assert((rc == 0) == admitted);
   assert(!strcmp(result.response, admitted ? display : "POISONED display"));
   assert(result.success == success);
   snapshot = aimee_ir_response_json(&result.canonical_response);
   char *after = cJSON_PrintUnformatted(snapshot);
   assert(before && after && !strcmp(before, after));
   cJSON_Delete(snapshot);
   free(before);
   free(after);
   free(result.response);
}
static void test_worker_projection(void)
{
   aimee_block_t blocks[] = {{.type = AIMEE_BLK_IMAGE,
                              .media_type = "image/png",
                              .media_ref = "https://example.invalid/generated.png"},
                             {.type = AIMEE_BLK_TEXT, .text = "canonical authored answer"},
                             {.type = AIMEE_BLK_THINKING, .text = "inert reasoning"}};
   aimee_response_t ir = {.role = "assistant",
                          .content = blocks,
                          .n_content = 1,
                          .stop_reason = AIMEE_STOP_END_TURN,
                          .raw_stop_reason = "end_turn"};
   worker_projection(&ir, 1, 0, 1, "");
   worker_projection(&ir, 1, 1, 0, NULL);
   ir.n_content = 3;
   worker_projection(&ir, 1, 0, 1, blocks[1].text);
   worker_projection(&ir, 1, 1, 0, NULL);
   ir.content = blocks + 1;
   ir.n_content = 2;
   worker_projection(&ir, 1, 0, 1, blocks[1].text);
   worker_projection(&ir, 1, 1, 1, blocks[1].text);
   worker_projection(&ir, 0, 0, 0, NULL);
   ir.stop_reason = AIMEE_STOP_MAX_TOKENS;
   ir.raw_stop_reason = "max_tokens";
   worker_projection(&ir, 1, 0, 0, NULL);
   ir.stop_reason = AIMEE_STOP_CONTENT_FILTER;
   ir.raw_stop_reason = "refusal";
   aimee_refusal_details_t refusal = {.category = "future_category", .explanation = "retained"};
   ir.refusal = &refusal;
   worker_projection(&ir, 1, 0, 0, NULL);
   ir.refusal = NULL;
   ir.stop_reason = AIMEE_STOP_ERROR;
   ir.raw_stop_reason = "error";
   ir.error_message = "host/provider failure";
   worker_projection(&ir, 1, 0, 0, NULL);
   ir.error_message = NULL;
   aimee_block_t tool = {.type = AIMEE_BLK_TOOL_USE,
                         .tool_id = "pending",
                         .tool_name = "read_file",
                         .tool_input = cJSON_CreateObject()};
   assert(tool.tool_input);
   ir.content = &tool;
   ir.n_content = 1;
   ir.stop_reason = AIMEE_STOP_TOOL_USE;
   ir.raw_stop_reason = "tool_use";
   worker_projection(&ir, 1, 0, 0, NULL);
   cJSON_Delete(tool.tool_input);
}
/* Exercise the same admission/whole-attempt merge used by delegate_worker,
 * then the shipping envelope builder. No provider, source or repair executor
 * is replaced with an affirmative execution receipt. */
static void test_handoff_repair_accounting(void)
{
   const struct
   {
      int prior_in, repair_in, prior_unknown, repair_unknown, rc, partial;
      int admitted, unknown;
   } cases[] = {
       {0, 7, 0, 0, 0, 0, 1, 0},  {7, 0, 0, 0, 0, 0, 1, 1},  {0, 0, 1, 0, 0, 0, 1, 1},
       {0, 7, 0, 0, -1, 0, 0, 1}, {0, 0, 0, 1, -1, 0, 0, 1}, {7, 0, 0, 0, -1, 0, 0, 0},
       {0, 7, 0, 0, 0, 1, 0, 1},  {0, 0, 0, 0, 0, 0, 1, 0},
   };
   for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
   {
      agent_result_t result = {0}, repair = {0};
      assert(agent_result_from_host_text(&result, "original invalid handoff") == 0);
      assert(agent_result_from_host_text(&repair, "admitted repaired handoff") == 0);
      result.success = repair.success = 1;
      snprintf(result.model, sizeof result.model, "original-model");
      snprintf(repair.model, sizeof repair.model, "repair-model");
      result.prompt_tokens = result.canonical_response.usage_in = cases[i].prior_in;
      repair.prompt_tokens = repair.canonical_response.usage_in = cases[i].repair_in;
      result.completion_tokens = result.canonical_response.usage_out = cases[i].prior_in;
      repair.completion_tokens = repair.canonical_response.usage_out = cases[i].repair_in;
      repair.canonical_response.raw = cJSON_Parse("{\"repair_observation\":7}");
      assert(repair.canonical_response.raw);
      result.usage_unobserved = result.canonical_response.usage_unobserved = cases[i].prior_unknown;
      repair.usage_unobserved = repair.canonical_response.usage_unobserved =
          cases[i].repair_unknown;
      result.turns = 1;
      repair.turns = 2;
      result.tool_calls = 2;
      repair.tool_calls = 3;
      result.successful_tool_calls = 1;
      repair.successful_tool_calls = 2;
      result.rescue_recoveries = 1;
      repair.rescue_recoveries = 2;
      result.latency_ms = 11;
      repair.latency_ms = 19;
      if (cases[i].partial)
         repair.canonical_response.stop_reason = AIMEE_STOP_MAX_TOKENS;
      assert(delegate_merge_handoff_repair(&result, &repair, cases[i].rc) == cases[i].admitted);
      assert(result.success == 1);
      assert(!strcmp(result.response,
                     cases[i].admitted ? "admitted repaired handoff" : "original invalid handoff"));
      assert(!strcmp(result.model, cases[i].admitted ? "repair-model" : "original-model"));
      assert(result.prompt_tokens == cases[i].prior_in + cases[i].repair_in);
      assert(result.completion_tokens == cases[i].prior_in + cases[i].repair_in);
      assert(cJSON_GetObjectItemCaseSensitive(cases[i].admitted ? result.canonical_response.raw
                                                                : repair.canonical_response.raw,
                                              "repair_observation")
                 ->valueint == 7);
      assert(result.turns == 3 && result.tool_calls == 5 && result.successful_tool_calls == 3);
      assert(result.latency_ms == 30 && result.rescue_recoveries == 3);
      assert(agent_result_usage_is_unobserved(&result) == cases[i].unknown);
      assert(result.canonical_response.usage_unobserved == cases[i].unknown);
      assert(result.canonical_response.usage_in == (cases[i].unknown    ? 0
                                                    : cases[i].admitted ? cases[i].repair_in
                                                                        : cases[i].prior_in));
      cJSON *wire = delegate_build_result_response("owned-repair", cases[i].admitted ? 0 : 2,
                                                   &result, NULL, "reviewer", NULL, 0, 0, NULL, "");
      assert(wire);
      assert(cJSON_GetObjectItemCaseSensitive(wire, "prompt_tokens")->valueint ==
             result.prompt_tokens);
      assert(cJSON_GetObjectItemCaseSensitive(wire, "turns")->valueint == 3);
      assert(cJSON_GetObjectItemCaseSensitive(wire, "tool_calls")->valueint == 5);
      assert(cJSON_GetObjectItemCaseSensitive(wire, "latency_ms")->valueint == 30);
      aimee_response_t decoded = {0};
      assert(aimee_ir_response_parse(cJSON_GetObjectItemCaseSensitive(wire, "ir"), &decoded) == 0);
      assert(decoded.usage_unobserved == cases[i].unknown);
      aimee_response_free(&decoded);
      cJSON_Delete(wire);
      agent_result_clear_response(&result);
      agent_result_clear_response(&repair);
   }
   /* Cache/reasoning-only prior work must also invalidate latest-model pricing.
    * A failed attempt is accounted without replacing its parent's body. */
   agent_result_t result = {0}, repair = {0};
   assert(agent_result_from_host_text(&result, "original") == 0);
   assert(agent_result_from_host_text(&repair, "repair") == 0);
   result.success = repair.success = 1;
   result.cache_hit = 1;
   repair.cache_read_tokens = 4;
   repair.cache_write_tokens = 5;
   repair.canonical_response.usage_reasoning = 6;
   repair.success = 0;
   assert(delegate_merge_handoff_repair(&result, &repair, 0) == 0);
   assert(result.cache_read_tokens == 4 && result.cache_write_tokens == 5);
   assert(!result.cache_hit && result.canonical_response.usage_unobserved);
   assert(!strcmp(result.response, "original"));
   agent_result_clear_response(&result);
   agent_result_clear_response(&repair);
}

static void test_delegate_queue_input_is_ir(void)
{
   compute_ctx_t context = {0};
   context.req = cJSON_Parse("{\"prompt\":\"External CLI task text\"}");
   assert(context.req && delegate_input_admit(&context, 0) != 0);
   assert(!context.ir_input.role);
   assert(!delegate_input_admit(&context, 1));
   assert(!cJSON_GetObjectItem(context.req, "prompt"));
   assert(cJSON_IsObject(cJSON_GetObjectItem(context.req, "message")));
   assert(!strcmp(context.ir_input.role, "user"));
   assert(!strcmp(context.ir_input.blocks[0].text, "External CLI task text"));
   assert(!delegate_input_admit(&context, 0));
   aimee_message_free(&context.ir_input);
   cJSON_Delete(context.req);
   context = (compute_ctx_t){0};
   context.req = cJSON_Parse("{\"message\":{\"role\":\"assistant\",\"blocks\":[{\"type\":\"text\","
                             "\"text\":\"not a task\"}]}}");
   assert(context.req && delegate_input_admit(&context, 0) != 0);
   aimee_message_free(&context.ir_input);
   cJSON_Delete(context.req);
   puts("delegate queue: legacy text converts only at external ingress; worker requires user IR");
}

int main(void)
{
   test_delegate_queue_input_is_ir();
   test_handoff_repair_accounting();
   test_terminal_consumers();
   test_worker_projection();
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
