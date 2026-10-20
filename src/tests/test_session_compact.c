/* Owned canonical session compaction: protected instructions, complete cycles,
 * literal evidence, atomic admission and unbounded retained tails. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aimee.h"
#include "session_compact.h"
#include "util.h"
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#define PASS(name) printf("  PASS: %s\n", name)

static cJSON *make_msg(const char *role, const char *text)
{
   aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = (char *)text};
   aimee_message_t message = {.role = (char *)role, .blocks = &block, .n_blocks = 1};
   cJSON *json = aimee_ir_message_json(&message);
   assert(json);
   return json;
}
static cJSON *make_canonical_text(const char *role, const char *text)
{
   return make_msg(role, text);
}
static cJSON *make_long_conversation(int count)
{
   cJSON *messages = cJSON_CreateArray();
   cJSON_AddItemToArray(messages, make_msg("user", "task"));
   for (int i = 0; i < count; i++)
      cJSON_AddItemToArray(messages, make_msg("assistant", "evidence"));
   return messages;
}

static cJSON *make_record_conversation(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("system", "You are a coder."));
   cJSON_AddItemToArray(arr, make_msg("user", "Investigate the retry path."));
   cJSON_AddItemToArray(
       arr, make_msg("assistant", "[verdict] The retry budget is capped upstream at 4a7f19c2b8e30d1"
                                  "5f6a2c9b40e7d3814aa9c5162; nothing downstream can raise it."));
   cJSON_AddItemToArray(
       arr,
       make_msg("assistant", "[hazard] Touching #778 without the migration will strand rows."));
   for (int i = 0; i < 8; i++)
   {
      cJSON_AddItemToArray(arr, make_msg("user", "continue"));
      cJSON_AddItemToArray(arr, make_msg("assistant", "[wip] Reading src/modules/git/retry.c."));
   }
   return arr;
}

static void test_pressure_unknown_window(void)
{
   /* context_window=0: always OK */
   int p = session_compact_pressure(90000, 10000, 0, NULL);
   assert(p == SESSION_PRESSURE_OK);
   PASS("pressure_unknown_window");
}

static void test_pressure_ok(void)
{
   /* 50% usage => OK */
   int p = session_compact_pressure(5000, 0, 10000, NULL);
   assert(p == SESSION_PRESSURE_OK);
   PASS("pressure_ok");
}

static void test_pressure_warn(void)
{
   /* 72% usage => WARN (default threshold 70%) */
   int p = session_compact_pressure(7200, 0, 10000, NULL);
   assert(p == SESSION_PRESSURE_WARN);
   PASS("pressure_warn");
}

static void test_pressure_compact(void)
{
   /* 85% usage => COMPACT (default threshold 80%) */
   int p = session_compact_pressure(8500, 0, 10000, NULL);
   assert(p == SESSION_PRESSURE_COMPACT);
   PASS("pressure_compact");
}

static void test_pressure_custom_thresholds(void)
{
   session_compact_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.warn_pct = 50;
   cfg.compact_pct = 60;

   /* 55%: warn with custom threshold */
   assert(session_compact_pressure(5500, 0, 10000, &cfg) == SESSION_PRESSURE_WARN);
   /* 65%: compact with custom threshold */
   assert(session_compact_pressure(6500, 0, 10000, &cfg) == SESSION_PRESSURE_COMPACT);
   /* 40%: ok */
   assert(session_compact_pressure(4000, 0, 10000, &cfg) == SESSION_PRESSURE_OK);
   PASS("pressure_custom_thresholds");
}

static void test_estimate_tokens_null(void)
{
   assert(session_compact_estimate_tokens(NULL) == 0);
   PASS("estimate_tokens_null");
}

static void test_estimate_tokens_basic(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "hello world"));
   int tokens = session_compact_estimate_tokens(arr);
   /* Should be positive and proportional to content */
   assert(tokens > 0);
   cJSON_Delete(arr);
   PASS("estimate_tokens_basic");
}

static void test_estimate_tokens_grows_with_content(void)
{
   cJSON *small = cJSON_CreateArray();
   cJSON_AddItemToArray(small, make_msg("user", "hi"));

   cJSON *large = cJSON_CreateArray();
   char *bigtext = malloc(10001);
   memset(bigtext, 'x', 10000);
   bigtext[10000] = '\0';
   cJSON_AddItemToArray(large, make_msg("user", bigtext));
   free(bigtext);

   int t_small = session_compact_estimate_tokens(small);
   int t_large = session_compact_estimate_tokens(large);
   assert(t_large > t_small);

   cJSON_Delete(small);
   cJSON_Delete(large);
   PASS("estimate_tokens_grows_with_content");
}

static void test_record_uses_register_classification(void)
{
   cJSON *arr = make_record_conversation();
   session_compact_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.from_record = 1;

   session_compact_result_t result;
   assert(session_compact(arr, &cfg, &result) == 0);
   assert(result.compacted == 1);

   const char *decisions = strstr(result.summary, "## Key Decisions");
   const char *blocked = strstr(result.summary, "## Blocked");
   assert(decisions && blocked);
   assert(strstr(decisions, "retry budget is capped upstream") == NULL);
   assert(strstr(blocked, "without the migration will strand rows") == NULL);

   /* A transient [wip] turn is work-in-progress, not a settled fact. */
   assert(strstr(decisions, "Reading src/modules/git/retry.c") == NULL);

   cJSON_Delete(arr);
   PASS("record_uses_register_classification");
}

static void test_record_conserves_coordinates_verbatim(void)
{
   cJSON *arr = make_record_conversation();
   session_compact_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.from_record = 1;

   session_compact_result_t result;
   assert(session_compact(arr, &cfg, &result) == 0);
   assert(result.compacted == 1);

   assert(strstr(result.summary, "4a7f19c2b8e30d15f6a2c9b40e7d3814aa9c5162") == NULL);

   cJSON_Delete(arr);
   PASS("record_conserves_coordinates_verbatim");
}

static void test_record_flag_selects_derivation(void)
{
   cJSON *legacy_arr = make_record_conversation();
   session_compact_result_t legacy;
   session_compact_config_t legacy_cfg;
   memset(&legacy_cfg, 0, sizeof(legacy_cfg));
   legacy_cfg.from_record = 0;
   assert(session_compact(legacy_arr, &legacy_cfg, &legacy) == 0);

   cJSON *record_arr = make_record_conversation();
   session_compact_result_t record;
   session_compact_config_t record_cfg;
   memset(&record_cfg, 0, sizeof(record_cfg));
   record_cfg.from_record = 1;
   assert(session_compact(record_arr, &record_cfg, &record) == 0);

   assert(legacy.compacted == 1 && record.compacted == 1);
   assert(strcmp(legacy.summary, record.summary) == 0);

   cJSON_Delete(legacy_arr);
   cJSON_Delete(record_arr);
   PASS("record_flag_selects_derivation");
}

static void test_record_off_keeps_legacy_sections(void)
{
   cJSON *arr = make_record_conversation();
   session_compact_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.from_record = 0;

   session_compact_result_t result;
   assert(session_compact(arr, &cfg, &result) == 0);
   assert(result.compacted == 1);

   const char *sections[] = {"## Active Task",       "## Goal",           "## Constraints",
                             "## Completed Actions", "## Active State",   "## In Progress",
                             "## Blocked",           "## Key Decisions",  "## Resolved Questions",
                             "## Pending User Asks", "## Relevant Files", NULL};
   for (int i = 0; sections[i]; i++)
      assert(strstr(result.summary, sections[i]) != NULL);
   assert(strstr(result.summary, "Flashback JSON:") != NULL);

   cJSON_Delete(arr);
   PASS("record_off_keeps_legacy_sections");
}

static void test_record_keeps_section_contract(void)
{
   cJSON *arr = make_record_conversation();
   session_compact_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.from_record = 1;

   session_compact_result_t result;
   assert(session_compact(arr, &cfg, &result) == 0);
   assert(result.compacted == 1);

   const char *sections[] = {"## Active Task",       "## Goal",           "## Constraints",
                             "## Completed Actions", "## Active State",   "## In Progress",
                             "## Blocked",           "## Key Decisions",  "## Resolved Questions",
                             "## Pending User Asks", "## Relevant Files", NULL};
   for (int i = 0; sections[i]; i++)
      assert(strstr(result.summary, sections[i]) != NULL);

   cJSON_Delete(arr);
   PASS("record_keeps_section_contract");
}

static void test_canonical_compaction_preserves_instructions_and_pairs(void)
{
   cJSON *messages = cJSON_CreateArray();
   cJSON_AddItemToArray(messages, make_canonical_text("user", "original task"));
   cJSON_AddItemToArray(messages, make_canonical_text("assistant", "evidence one"));
   cJSON_AddItemToArray(messages, make_canonical_text("assistant", "evidence two"));
   cJSON *constraint = make_canonical_text("user", "Keep this exact constraint: do not delete a.c");
   cJSON *expected_constraint = cJSON_Duplicate(constraint, 1);
   cJSON_AddItemToArray(messages, constraint);
   cJSON_AddItemToArray(messages, make_canonical_text("assistant", "evidence three"));
   cJSON_AddItemToArray(messages, make_canonical_text("assistant", "evidence four"));
   cJSON_AddItemToArray(messages, make_canonical_text("assistant", "evidence five"));
   cJSON *call = cJSON_Parse(
       "{\"role\":\"assistant\",\"blocks\":[{\"type\":\"thinking\",\"text\":\"reason\","
       "\"signature\":\"sig\"},"
       "{\"type\":\"tool_use\",\"tool_id\":\"a\",\"name\":\"read_file\",\"input\":{}},"
       "{\"type\":\"tool_use\",\"tool_id\":\"b\",\"name\":\"read_file\",\"input\":{}}]}");
   cJSON *expected_call = cJSON_Duplicate(call, 1);
   cJSON_AddItemToArray(messages, call);
   cJSON_AddItemToArray(messages,
                        cJSON_Parse("{\"role\":\"tool\",\"blocks\":[{\"type\":\"tool_result\","
                                    "\"tool_id\":\"a\",\"content\":\"a\",\"is_error\":false}]}"));
   cJSON_AddItemToArray(messages,
                        cJSON_Parse("{\"role\":\"tool\",\"blocks\":[{\"type\":\"tool_result\","
                                    "\"tool_id\":\"b\",\"content\":\"b\",\"is_error\":false}]}"));
   cJSON_AddItemToArray(messages, make_canonical_text("user", "continue"));
   cJSON_AddItemToArray(messages, make_canonical_text("assistant", "done"));
   session_compact_config_t config = {.retain_tail = 3};
   session_compact_result_t result = {0};
   assert(session_compact(messages, &config, &result) == 0 && result.compacted);
   assert(result.messages_after == 9);
   assert(cJSON_Compare(expected_constraint, cJSON_GetArrayItem(messages, 2), 1));
   assert(cJSON_Compare(expected_call, cJSON_GetArrayItem(messages, 4), 1));
   assert(aimee_ir_history_repair(messages) == 0);
   for (int i = 0; i < cJSON_GetArraySize(messages); i++)
   {
      cJSON *message = cJSON_GetArrayItem(messages, i);
      assert(cJSON_IsArray(cJSON_GetObjectItem(message, "blocks")));
      assert(!cJSON_GetObjectItem(message, "content"));
      if (cJSON_IsTrue(cJSON_GetObjectItem(message, "_compaction_boundary")))
         assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(message, "role")), "assistant"));
   }
   cJSON_Delete(expected_constraint);
   cJSON_Delete(expected_call);
   cJSON_Delete(messages);
   PASS("canonical compaction retains instructions, signed reasoning and complete tool cycles");
}

static void test_canonical_tool_evidence_in_fallback(void)
{
   cJSON *messages = cJSON_CreateArray();
   cJSON_AddItemToArray(messages, make_canonical_text("user", "task"));
   cJSON_AddItemToArray(
       messages,
       cJSON_Parse("{\"role\":\"assistant\",\"blocks\":["
                   "{\"type\":\"tool_use\",\"tool_id\":\"a\",\"name\":\"read\",\"input\":{}},"
                   "{\"type\":\"tool_use\",\"tool_id\":\"b\",\"name\":\"read\",\"input\":{}}]}"));
   cJSON_AddItemToArray(
       messages,
       cJSON_Parse("{\"role\":\"tool\",\"blocks\":[{\"type\":\"text\",\"text\":\"first\"},"
                   "{\"type\":\"tool_result\",\"tool_id\":\"a\",\"is_error\":true,\"content\":"
                   "\"error in src/second.c\"},"
                   "{\"type\":\"tool_result\",\"tool_id\":\"b\",\"is_error\":false,\"content\":"
                   "\"changed src/third.c\"}]}"));
   for (int i = 0; i < 12; i++)
      cJSON_AddItemToArray(messages, make_canonical_text("assistant", "progress"));
   session_compact_config_t cfg = {.retain_tail = 3};
   session_compact_result_t result;
   assert(session_compact(messages, &cfg, &result) == 0 && result.compacted);
   const char *summary = result.summary;
   assert(strstr(summary, "src/second.c") && strstr(summary, "src/third.c"));
   assert(strstr(summary, "error in src/second.c"));
   cJSON_Delete(messages);
   PASS("canonical fallback keeps all tool evidence");
}

static void test_native_and_mixed_refuse_atomically(void)
{
   const char *inputs[] = {
       "null",
       "{}",
       "[{\"role\":\"user\",\"content\":\"native\"}]",
       "[{\"type\":\"function_call\",\"call_id\":\"a\",\"name\":\"read\",\"arguments\":\"{}\"}]",
       "[{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"a\",\"name\":"
       "\"read\",\"input\":{}}]}]",
       "[{\"role\":\"user\",\"blocks\":[]},{\"role\":\"user\",\"content\":\"native\"}]",
       "[{\"role\":\"user\",\"blocks\":[],\"content\":\"native\"}]",
       "[{\"role\":\"user\",\"role\":\"assistant\",\"blocks\":[]}]",
       "[{\"role\":\"user\",\"blocks\":[{\"type\":\"tool_use\",\"name\":\"read\",\"tool_id\":\"a\","
       "\"input\":{}}]}]",
       "[{\"role\":\"invented\",\"blocks\":[]}]"};
   for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++)
   {
      cJSON *messages = cJSON_Parse(inputs[i]);
      assert(messages);
      char *before = cJSON_PrintUnformatted(messages);
      session_compact_result_t result;
      assert(session_compact(messages, NULL, &result) == -1);
      char *after = cJSON_PrintUnformatted(messages);
      assert(before && after && !strcmp(before, after));
      assert(!result.compacted);
      free(before);
      free(after);
      cJSON_Delete(messages);
   }
   assert(session_compact(NULL, NULL, NULL) == -1);
   cJSON *empty = cJSON_CreateArray();
   assert(session_compact(empty, NULL, NULL) == 0);
   cJSON_Delete(empty);
   PASS("native, mixed and malformed history refuses without mutation");
}

static void test_tail_over_256_retains_opaque_values(void)
{
   cJSON *messages = make_long_conversation(20);
   cJSON *expected = cJSON_CreateArray();
   for (int i = 0; i < 300; i++)
   {
      char text[64];
      snprintf(text, sizeof text, "retained evidence %d", i);
      cJSON *message = make_msg("assistant", text);
      if (i == 150)
      {
         cJSON_Delete(message);
         message = cJSON_Parse(
             "{\"role\":\"assistant\",\"blocks\":["
             "{\"type\":\"thinking\",\"text\":\"private\",\"signature\":\"native-signature\"},"
             "{\"type\":\"text\",\"text\":\"answer\",\"cache_control\":\"ephemeral\",\"cache_ttl\":"
             "\"1h\"}]} ");
      }
      cJSON_AddItemToArray(expected, cJSON_Duplicate(message, 1));
      cJSON_AddItemToArray(messages, message);
   }
   session_compact_config_t config = {.retain_tail = 300};
   session_compact_result_t result;
   assert(session_compact(messages, &config, &result) == 0 && result.compacted);
   assert(result.messages_before == 321 && result.messages_after == 302 &&
          result.messages_removed == 19);
   for (int i = 0; i < 300; i++)
      assert(
          cJSON_Compare(cJSON_GetArrayItem(expected, i), cJSON_GetArrayItem(messages, i + 2), 1));
   assert(cJSON_IsTrue(
       cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(messages, 1), "_compaction_boundary")));
   cJSON_Delete(expected);
   cJSON_Delete(messages);
   PASS("300-message tail keeps opaque reasoning, cache metadata and order");
}

static void test_same_message_count_can_reduce_context(void)
{
   cJSON *messages = cJSON_CreateArray();
   cJSON *expected = cJSON_CreateArray();
   cJSON_AddItemToArray(messages, make_msg("user", "original task"));
   char *long_text = malloc(30001);
   assert(long_text);
   memset(long_text, 'x', 30000);
   long_text[30000] = 0;
   for (int i = 0; i < 2; i++)
   {
      cJSON *instruction = make_msg("user", i ? "later constraint" : "first constraint");
      cJSON_AddItemToArray(expected, cJSON_Duplicate(instruction, 1));
      cJSON_AddItemToArray(messages, instruction);
      cJSON_AddItemToArray(messages, make_msg("assistant", long_text));
   }
   free(long_text);
   for (int i = 0; i < 3; i++)
      cJSON_AddItemToArray(messages, make_msg("assistant", "retained tail"));
   int before = session_compact_estimate_tokens(messages);
   session_compact_config_t config = {.retain_tail = 3};
   session_compact_result_t result;
   assert(session_compact(messages, &config, &result) == 0 && result.compacted);
   assert(result.messages_before == 8 && result.messages_after == 8 &&
          result.messages_removed == 0);
   assert(session_compact_estimate_tokens(messages) < before / 2);
   assert(cJSON_Compare(cJSON_GetArrayItem(expected, 0), cJSON_GetArrayItem(messages, 1), 1));
   assert(cJSON_Compare(cJSON_GetArrayItem(expected, 1), cJSON_GetArrayItem(messages, 3), 1));
   cJSON_Delete(expected);
   cJSON_Delete(messages);
   PASS("protected instructions survive while same-count compaction reduces context");
}

static void test_readonly_signatures_and_repair(void)
{
   cJSON *messages = cJSON_CreateArray();
   cJSON_AddItemToArray(messages, make_msg("user", "task"));
   cJSON *call = cJSON_Parse("{\"role\":\"assistant\",\"blocks\":[]}");
   cJSON *result = cJSON_Parse("{\"role\":\"tool\",\"blocks\":[]}");
   cJSON *calls = cJSON_GetObjectItemCaseSensitive(call, "blocks");
   cJSON *results = cJSON_GetObjectItemCaseSensitive(result, "blocks");
   for (int i = 0; i < 70; i++)
   {
      char id[64];
      snprintf(id, sizeof id, "native-call-%d", i);
      cJSON *input = cJSON_CreateObject();
      cJSON_AddNumberToObject(input, "index", i);
      aimee_block_t tool = {
          .type = AIMEE_BLK_TOOL_USE, .tool_id = id, .tool_name = "read_file", .tool_input = input};
      cJSON_AddItemToArray(calls, aimee_ir_block_json(&tool));
      cJSON_Delete(input);
      cJSON *output = cJSON_CreateString("read evidence");
      tool = (aimee_block_t){.type = AIMEE_BLK_TOOL_RESULT, .tool_id = id, .tool_result = output};
      cJSON_AddItemToArray(results, aimee_ir_block_json(&tool));
      cJSON_Delete(output);
   }
   cJSON_AddItemToArray(messages, call);
   cJSON_AddItemToArray(messages, result);
   for (int i = 0; i < 12; i++)
      cJSON_AddItemToArray(messages, make_msg("assistant", "progress"));
   session_compact_config_t config = {.retain_tail = 3};
   session_compact_result_t report;
   assert(session_compact(messages, &config, &report) == 0 && report.compacted);
   assert(report.readonly_sig_count == 64 && report.readonly_sigs_dropped == 6);
   assert(strstr(report.summary, "70 tool calls"));
   cJSON_Delete(messages);
   char sig[64], other[64];
   assert(session_compact_tool_sig("read_file", "{\"n\":9007199254740993}", sig, sizeof sig));
   assert(session_compact_tool_sig("read_file", "{\"n\":9007199254740992}", other, sizeof other));
   assert(strcmp(sig, other));
   assert(!session_compact_tool_sig("write_file", "{}", sig, sizeof sig));
   assert(!session_compact_tool_sig("test", "{}", sig, sizeof sig));
   char focused[8192];
   assert(!session_compact_focused("Investigate the retry path", focused, sizeof focused));
   assert(strstr(focused, "Investigate the retry path"));
   PASS("all canonical calls counted, signature capacity reported, exact arguments distinguished");
}

static size_t fault_at, allocation_count;
static int allocation_faulted;
static void *fail_one_allocation(size_t size)
{
   allocation_count++;
   if (allocation_count == fault_at)
   {
      allocation_faulted = 1;
      return NULL;
   }
   return malloc(size);
}

static void test_allocation_refusal_is_atomic(void)
{
   /* Measure the successful path so added admission allocations are covered
    * without a brittle fixed fault-injection ceiling. */
   cJSON *baseline = make_long_conversation(20);
   cJSON_AddItemToArray(baseline,
                        make_msg("assistant", "error reading src/failure.c; using guarded retry"));
   allocation_count = 0;
   allocation_faulted = 0;
   fault_at = (size_t)-1;
   cJSON_Hooks baseline_hooks = {.malloc_fn = fail_one_allocation, .free_fn = free};
   cJSON_InitHooks(&baseline_hooks);
   session_compact_result_t baseline_report;
   int baseline_rc = session_compact(baseline, NULL, &baseline_report);
   cJSON_InitHooks(NULL);
   size_t limit = allocation_count + 1;
   assert(!baseline_rc && baseline_report.compacted && !allocation_faulted);
   cJSON_Delete(baseline);
   int reached_success = 0;
   for (fault_at = 1; fault_at <= limit; fault_at++)
   {
      cJSON *messages = make_long_conversation(20);
      cJSON_AddItemToArray(
          messages, make_msg("assistant", "error reading src/failure.c; using guarded retry"));
      char *before = cJSON_PrintUnformatted(messages);
      assert(before);
      allocation_count = 0;
      allocation_faulted = 0;
      cJSON_Hooks hooks = {.malloc_fn = fail_one_allocation, .free_fn = free};
      cJSON_InitHooks(&hooks);
      session_compact_result_t report;
      int rc = session_compact(messages, NULL, &report);
      cJSON_InitHooks(NULL);
      char *after = cJSON_PrintUnformatted(messages);
      assert(after);
      if (allocation_faulted)
      {
         if (rc != -1 || strcmp(before, after))
            fprintf(stderr, "allocation fault %zu: rc=%d changed=%d\n", fault_at, rc,
                    strcmp(before, after) != 0);
         assert(rc == -1 && !strcmp(before, after));
      }
      else
      {
         assert(rc == 0 && report.compacted);
         reached_success = 1;
      }
      free(before);
      free(after);
      cJSON_Delete(messages);
      if (reached_success)
         break;
   }
   assert(reached_success);
   PASS("every JSON allocation failure refuses without changing durable source history");
}

static void test_summary_utf8_boundaries(void)
{
   char topic[4001];
   topic[0] = 0;
   for (int i = 0; i < 300; i++)
      strcat(topic, "工具😀");
   char summary[8192];
   assert(!session_compact_focused(topic, summary, sizeof summary));
   assert(text_is_valid_utf8(summary));
   for (size_t size = 60; size < 400; size++)
   {
      memset(summary, 'x', sizeof summary);
      assert(!session_compact_focused(topic, summary, size));
      assert(strlen(summary) < size && text_is_valid_utf8(summary));
   }
   PASS("bounded summaries and caller buffers retain complete Unicode code points");
}

static void test_incomplete_cycles_refuse_without_repair(void)
{
   const char *cases[] = {"[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"tool_use\",\"tool_id\":"
                          "\"pending\",\"name\":\"read_file\",\"arguments\":{}}]}]",
                          "[{\"role\":\"tool\",\"blocks\":[{\"type\":\"tool_result\",\"tool_id\":"
                          "\"orphan\",\"content\":\"actual effect receipt\"}]}]"};
   for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
      for (int long_history = 0; long_history < 2; long_history++)
      {
         cJSON *history = cJSON_Parse(cases[i]);
         assert(history);
         if (long_history)
            for (int j = 0; j < 20; j++)
               cJSON_AddItemToArray(history, make_msg("user", "continue"));
         cJSON *before = cJSON_Duplicate(history, 1);
         assert(before);
         session_compact_result_t report;
         int rc = session_compact(history, NULL, &report);
         assert(rc < 0 && !report.compacted && !report.repairs);
         assert(cJSON_Compare(history, before, 1));
         cJSON_Delete(before);
         cJSON_Delete(history);
      }
   PASS("unresolved and orphaned cycles refuse atomically even below compaction threshold");
}
int main(void)
{
   test_incomplete_cycles_refuse_without_repair();
   test_pressure_unknown_window();
   test_pressure_ok();
   test_pressure_warn();
   test_pressure_compact();
   test_pressure_custom_thresholds();
   test_estimate_tokens_null();
   test_estimate_tokens_basic();
   test_estimate_tokens_grows_with_content();
   test_record_uses_register_classification();
   test_record_conserves_coordinates_verbatim();
   test_record_flag_selects_derivation();
   test_record_off_keeps_legacy_sections();
   test_record_keeps_section_contract();
   test_canonical_compaction_preserves_instructions_and_pairs();
   test_canonical_tool_evidence_in_fallback();
   test_native_and_mixed_refuse_atomically();
   test_allocation_refusal_is_atomic();
   test_summary_utf8_boundaries();
   test_tail_over_256_retains_opaque_values();
   test_same_message_count_can_reduce_context();
   test_readonly_signatures_and_repair();
   puts("owned session compaction: passed");
   return 0;
}
