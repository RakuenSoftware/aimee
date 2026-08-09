/* test_delegate_liveness.c: unit tests for delegate liveness detection utilities.
 *
 * Tests liveness_is_empty_response(), liveness_format_empty_diagnostic(),
 * and liveness_circuit_breaker_tripped(). */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "liveness.h"

/* --- liveness_is_empty_response --- */

static void test_empty_null(void)
{
   assert(liveness_is_empty_response(NULL) == 1);
   printf("  PASS: NULL input is empty\n");
}

static void test_empty_empty_string(void)
{
   assert(liveness_is_empty_response("") == 1);
   printf("  PASS: empty string is empty\n");
}

static void test_empty_whitespace_only(void)
{
   assert(liveness_is_empty_response("   ") == 1);
   assert(liveness_is_empty_response("\t\n\r") == 1);
   assert(liveness_is_empty_response("  \n  ") == 1);
   printf("  PASS: whitespace-only strings are empty\n");
}

static void test_empty_normal_content(void)
{
   assert(liveness_is_empty_response("hello") == 0);
   assert(liveness_is_empty_response("  hello  ") == 0);
   assert(liveness_is_empty_response("done.") == 0);
   printf("  PASS: normal content is not empty\n");
}

static void test_degenerate_punctuation_only(void)
{
   assert(liveness_is_degenerate_response("%") == 1);
   assert(liveness_is_degenerate_response("\"") == 1);
   assert(liveness_is_degenerate_response("\"\n\"\n\"\n\"\n\"\n\"\n\"\n\"\n") == 1);
   printf("  PASS: punctuation-only output is degenerate\n");
}

static void test_degenerate_repetitive_numeric_pattern(void)
{
   assert(liveness_is_degenerate_response(
              "2.1.2.1.1.1.1.1.1.1.1.1.1.1.1.1.1.1.1.1.1.1.1.1.1.1.1.1.") == 1);
   assert(liveness_is_degenerate_response(
              "1111111111111111111111111111111111111111111111111111111111111111") == 1);
   printf("  PASS: repetitive numeric output is degenerate\n");
}

static void test_degenerate_bare_tool_markup(void)
{
   assert(liveness_is_degenerate_response("$env_get{name=\"AIMEE_DELEGATE_TASK\"}") == 1);
   assert(liveness_is_degenerate_response("  $bash{cmd=\"date\"}\n") == 1);
   printf("  PASS: bare tool-call markup is degenerate\n");
}

static void test_degenerate_raw_xml_tool_fragment(void)
{
   assert(liveness_is_degenerate_response(
              "38/c746e546/src\n</parameter>\n</function>\n</tool_call>\n") == 1);
   assert(liveness_is_degenerate_response("<tool_call>\n</function>\n") == 1);
   assert(liveness_is_degenerate_response("This answer discusses the raw markup.\n"
                                          "<tool_call>\n"
                                          "</tool_call>\n") == 0);
   assert(liveness_is_degenerate_response("I saw a literal </tool_call> marker in logs.") == 0);
   printf("  PASS: raw XML tool-call fragments are degenerate\n");
}

static void test_degenerate_raw_invoke_tool_fragment(void)
{
   assert(liveness_is_degenerate_response("<invoke name=\"bash\">\n"
                                          "<parameter name=\"command\">echo done</parameter>\n"
                                          "</invoke>\n") == 1);
   assert(liveness_is_degenerate_response("I saw a literal <invoke name=\"bash\"> marker.") == 0);
   printf("  PASS: raw invoke tool-call fragments are degenerate\n");
}

static void test_degenerate_raw_bracket_tool_fragment(void)
{
   assert(liveness_is_degenerate_response(
              "I'll read the proposal first.\n"
              "[TOOL_CALL]\n"
              "{tool => \"read_file\", args => { --path \"docs/proposal.md\" }}\n"
              "[/TOOL_CALL]\n") == 1);
   assert(liveness_is_degenerate_response(
              "[TOOL_CALL]\n"
              "{tool => \"bash\", args => { --note \"Get overview\" --command \"aimee index "
              "overview\"}}\n"
              "[/TOOL_CALL]\n"
              "[TOOL_CALL]\n"
              "{tool => \"bash\", args => { --note \"Search memory\" --command \"aimee memory "
              "search proposal\"}}\n"
              "[/TOOL_CALL]\n") == 1);
   assert(liveness_is_degenerate_response("I saw a literal [TOOL_CALL] marker in logs.") == 0);
   printf("  PASS: raw bracket tool-call fragments are degenerate\n");
}

static void test_degenerate_raw_diff_fragment(void)
{
   assert(liveness_is_degenerate_response(
              "+    return 1;\n"
              "+   if (tool_markup_lines >= 2)\n"
              "+      return 1;\n"
              "-   if (!liveness_is_empty_response(partial_text))\n"
              "+   if (!liveness_is_degenerate_response(partial_text))\n") == 1);
   assert(liveness_is_degenerate_response(
              "This review found no blocking issues.\n"
              "+   if (!liveness_is_empty_response(partial_text))\n"
              "+   if (!liveness_is_degenerate_response(partial_text))\n") == 0);
   printf("  PASS: raw diff fragments are degenerate without prose\n");
}

static void test_degenerate_preserves_signal(void)
{
   assert(liveness_is_degenerate_response("3. The file does not exist.") == 0);
   assert(liveness_is_degenerate_response("LOCAL254_OK") == 0);
   assert(liveness_is_degenerate_response("The literal $env_get{name=\"x\"} was printed.") == 0);
   assert(liveness_is_degenerate_response("42") == 0);
   printf("  PASS: text with semantic signal is not degenerate\n");
}

static void test_unexecuted_tool_plan_detected(void)
{
   const char *content = "I'll read the proposal first, then inspect the calibration code.\n"
                         "```\n"
                         "sed -n '1,220p' docs/proposals/accepted/example.md\n"
                         "```\n"
                         "```\n"
                         "rg -n \"calibration\" src tests\n"
                         "```\n";
   assert(liveness_is_unexecuted_tool_plan_response(content) == 1);
   printf("  PASS: unexecuted fenced command plan is detected\n");
}

static void test_unexecuted_tool_plan_preserves_findings(void)
{
   const char *content = "I found the calibration bug in src/db2/calibration.c.\n"
                         "```\n"
                         "make test\n"
                         "```\n"
                         "The failing path drops scope filters before bucketing audit rows.\n";
   assert(liveness_is_unexecuted_tool_plan_response(content) == 0);
   printf("  PASS: findings with command examples are not unexecuted plans\n");
}

static void test_reject_degenerate_owned_response(void)
{
   char *response = malloc(64);
   assert(response != NULL);
   strcpy(response, "[TOOL_CALL]\n{}\n[/TOOL_CALL]\n");
   char error[128] = {0};
   assert(liveness_reject_degenerate_response(&response, error, sizeof(error),
                                              "durable job produced tool markup") == 1);
   assert(response == NULL);
   assert(strcmp(error, "durable job produced tool markup") == 0);
   printf("  PASS: owned degenerate response is rejected and cleared\n");
}

static void test_reject_degenerate_preserves_good_response(void)
{
   char *response = malloc(64);
   assert(response != NULL);
   strcpy(response, "No blocking findings.");
   char error[128] = {0};
   assert(liveness_reject_degenerate_response(&response, error, sizeof(error), "bad") == 0);
   assert(response != NULL);
   assert(strcmp(response, "No blocking findings.") == 0);
   assert(error[0] == '\0');
   free(response);
   printf("  PASS: owned semantic response is preserved\n");
}

static void test_reject_degenerate_null_inputs(void)
{
   char error[128] = {0};
   assert(liveness_reject_degenerate_response(NULL, error, sizeof(error), "bad") == 0);
   assert(error[0] == '\0');

   char *response = NULL;
   assert(liveness_reject_degenerate_response(&response, error, sizeof(error), "bad") == 0);
   assert(response == NULL);
   assert(error[0] == '\0');
   printf("  PASS: reject helper ignores NULL response inputs\n");
}

static void test_reject_degenerate_zero_error_buffer(void)
{
   char *response = malloc(64);
   assert(response != NULL);
   strcpy(response, "[TOOL_CALL]\n{}\n[/TOOL_CALL]\n");
   assert(liveness_reject_degenerate_response(&response, NULL, 0, "bad") == 1);
   assert(response == NULL);
   printf("  PASS: reject helper tolerates missing error buffer\n");
}

static void test_reject_degenerate_default_message(void)
{
   char *response = malloc(64);
   assert(response != NULL);
   strcpy(response, "[TOOL_CALL]\n{}\n[/TOOL_CALL]\n");
   char error[128] = {0};
   assert(liveness_reject_degenerate_response(&response, error, sizeof(error), NULL) == 1);
   assert(response == NULL);
   assert(strstr(error, "raw tool-call markup") != NULL);
   printf("  PASS: reject helper writes default diagnostic\n");
}

static void test_empty_single_newline(void)
{
   assert(liveness_is_empty_response("\n") == 1);
   printf("  PASS: single newline is empty\n");
}

/* --- liveness_format_empty_diagnostic --- */

static void test_diagnostic_with_agent_name(void)
{
   char buf[512];
   liveness_format_empty_diagnostic("code-agent", buf, sizeof(buf));
   assert(strstr(buf, "code-agent") != NULL);
   assert(strstr(buf, "LIVENESS ERROR") != NULL);
   assert(strstr(buf, "empty response") != NULL);
   printf("  PASS: diagnostic includes agent name\n");
}

static void test_diagnostic_without_agent_name(void)
{
   char buf[512];
   liveness_format_empty_diagnostic(NULL, buf, sizeof(buf));
   assert(strstr(buf, "LIVENESS ERROR") != NULL);
   assert(strstr(buf, "empty response") != NULL);
   printf("  PASS: diagnostic works without agent name\n");
}

static void test_diagnostic_empty_agent_name(void)
{
   char buf[512];
   liveness_format_empty_diagnostic("", buf, sizeof(buf));
   assert(strstr(buf, "LIVENESS ERROR") != NULL);
   printf("  PASS: diagnostic works with empty agent name\n");
}

static void test_diagnostic_null_buf(void)
{
   /* Should not crash */
   liveness_format_empty_diagnostic("agent", NULL, 0);
   liveness_format_empty_diagnostic("agent", NULL, 100);
   printf("  PASS: diagnostic handles NULL buf safely\n");
}

static void test_final_tool_call_diagnostic_without_partial_text(void)
{
   char buf[1600];
   liveness_format_final_tool_call_diagnostic("review-agent", "read_file", NULL, "rg",
                                              "line 12 matched", 8, buf, sizeof(buf));
   assert(strstr(buf, "review-agent") != NULL);
   assert(strstr(buf, "final response turn") != NULL);
   assert(strstr(buf, "read_file") != NULL);
   assert(strstr(buf, "No additional tools were run") != NULL);
   assert(strstr(buf, "rg") != NULL);
   assert(strstr(buf, "line 12 matched") != NULL);
   printf("  PASS: final tool-call diagnostic includes attempted and last tool context\n");
}

static void test_final_tool_call_diagnostic_preserves_partial_text(void)
{
   char buf[1600];
   liveness_format_final_tool_call_diagnostic("delegate", "bash", "I found one likely bug.", "sed",
                                              "snippet", 3, buf, sizeof(buf));
   assert(strstr(buf, "I found one likely bug.") != NULL);
   assert(strstr(buf, "bash") != NULL);
   assert(strstr(buf, "No additional tools were run") != NULL);
   printf("  PASS: final tool-call diagnostic preserves partial model text\n");
}

static void test_final_tool_call_diagnostic_filters_raw_tool_fragment(void)
{
   char buf[1600];
   const char *fragment = "38/c746e546/src\n</parameter>\n</function>\n</tool_call>\n";
   liveness_format_final_tool_call_diagnostic("delegate", "bash", fragment, "sed", "snippet", 3,
                                              buf, sizeof(buf));
   assert(strstr(buf, "</tool_call>") == NULL);
   assert(strstr(buf, "38/c746e546/src") == NULL);
   assert(strstr(buf, "bash") != NULL);
   assert(strstr(buf, "No additional tools were run") != NULL);
   printf("  PASS: final tool-call diagnostic filters raw tool fragments\n");
}

static void test_final_tool_call_diagnostic_null_buf(void)
{
   liveness_format_final_tool_call_diagnostic("agent", "read_file", "partial", "tool", "result", 1,
                                              NULL, 0);
   printf("  PASS: final tool-call diagnostic handles NULL buf safely\n");
}

/* --- liveness_circuit_breaker_tripped --- */

static void test_circuit_breaker_below_threshold(void)
{
   for (int i = 1; i < LIVENESS_REPEAT_ABORT_THRESHOLD; i++)
      assert(liveness_circuit_breaker_tripped(i) == 0);
   printf("  PASS: circuit breaker does not trip below threshold\n");
}

static void test_circuit_breaker_at_threshold(void)
{
   assert(liveness_circuit_breaker_tripped(LIVENESS_REPEAT_ABORT_THRESHOLD) == 1);
   printf("  PASS: circuit breaker trips at threshold\n");
}

static void test_circuit_breaker_above_threshold(void)
{
   assert(liveness_circuit_breaker_tripped(LIVENESS_REPEAT_ABORT_THRESHOLD + 1) == 1);
   assert(liveness_circuit_breaker_tripped(100) == 1);
   printf("  PASS: circuit breaker trips above threshold\n");
}

static void test_circuit_breaker_zero(void)
{
   assert(liveness_circuit_breaker_tripped(0) == 0);
   printf("  PASS: circuit breaker does not trip at zero\n");
}

static void test_circuit_breaker_negative(void)
{
   assert(liveness_circuit_breaker_tripped(-1) == 0);
   printf("  PASS: circuit breaker does not trip on negative input\n");
}

/* --- liveness_final_response_mode --- */

static void test_final_response_mode_requires_tools(void)
{
   assert(liveness_final_response_mode(8, 12, 0, 8) == LIVENESS_FINAL_RESPONSE_NONE);
   printf("  PASS: final response mode requires prior tool use\n");
}

static void test_final_response_mode_before_policy(void)
{
   assert(liveness_final_response_mode(7, 12, 3, 8) == LIVENESS_FINAL_RESPONSE_NONE);
   printf("  PASS: final response mode stays off before policy turn\n");
}

static void test_final_response_mode_soft_policy(void)
{
   assert(liveness_final_response_mode(8, 12, 3, 8) == LIVENESS_FINAL_RESPONSE_SOFT);
   assert(liveness_final_response_mode(10, 12, 3, 8) == LIVENESS_FINAL_RESPONSE_SOFT);
   printf("  PASS: final response mode marks role-policy turns soft\n");
}

static void test_final_response_mode_hard_last_turn(void)
{
   assert(liveness_final_response_mode(11, 12, 3, 8) == LIVENESS_FINAL_RESPONSE_HARD);
   assert(liveness_final_response_mode(4, 5, 1, -1) == LIVENESS_FINAL_RESPONSE_HARD);
   /* No call ever landed. That is precisely the delegate that needs the last
    * turn taken away from tools: every attempt was denied or emptied, so the
    * count is zero while the whole budget went on calling. Without this it
    * returned nothing at all. */
   assert(liveness_final_response_mode(11, 12, 0, 8) == LIVENESS_FINAL_RESPONSE_HARD);
   assert(liveness_final_response_mode(23, 24, 0, -1) == LIVENESS_FINAL_RESPONSE_HARD);
   printf("  PASS: final response mode marks max-turn boundary hard\n");
}

static void test_final_response_tool_policy(void)
{
   assert(liveness_final_response_allows_tools(LIVENESS_FINAL_RESPONSE_NONE) == 1);
   assert(liveness_final_response_allows_tools(LIVENESS_FINAL_RESPONSE_SOFT) == 0);
   assert(liveness_final_response_allows_tools(LIVENESS_FINAL_RESPONSE_HARD) == 0);
   printf("  PASS: final response soft and hard modes disable tools\n");
}

static void test_final_response_mode_primary_session_never_hard(void)
{
   /* Primary sessions pass max_turns=0 — HARD must never fire regardless of turn. */
   assert(liveness_final_response_mode(0, 0, 5, -1) == LIVENESS_FINAL_RESPONSE_NONE);
   assert(liveness_final_response_mode(999, 0, 5, -1) == LIVENESS_FINAL_RESPONSE_NONE);
   assert(liveness_final_response_mode(1000, 0, 5, -1) == LIVENESS_FINAL_RESPONSE_NONE);
   printf("  PASS: primary session (max_turns=0) never closes tool budget\n");
}

static void test_degenerate_leaked_xml_tool_call(void)
{
   /* A leaked <tool_call> with a JSON body (after prose) is degenerate... */
   assert(liveness_is_degenerate_response(
              "I need to read the file.<tool_call>\n{\"name\":\"read_file\"}\n</tool_call>") == 1);
   assert(liveness_is_degenerate_response("<tool_call>{\"name\":\"x\"}</tool_call>") == 1);
   /* ...but an empty <tool_call></tool_call> merely mentioned in prose is NOT,
    * and a real answer discussing the markup is NOT (regression guards). */
   assert(liveness_is_degenerate_response(
              "This answer discusses the raw markup.\n<tool_call>\n</tool_call>\n") == 0);
   assert(liveness_is_degenerate_response(
              "The <tool_call> tag wraps each call so the parser can find it.") == 0);
   printf("  degenerate_leaked_xml_tool_call: ok\n");
}

int main(void)
{
   printf("test_delegate_liveness\n");

   test_empty_null();
   test_empty_empty_string();
   test_empty_whitespace_only();
   test_empty_normal_content();
   test_empty_single_newline();
   test_degenerate_punctuation_only();
   test_degenerate_repetitive_numeric_pattern();
   test_degenerate_bare_tool_markup();
   test_degenerate_raw_xml_tool_fragment();
   test_degenerate_raw_invoke_tool_fragment();
   test_degenerate_raw_bracket_tool_fragment();
   test_degenerate_leaked_xml_tool_call();
   test_degenerate_raw_diff_fragment();
   test_degenerate_preserves_signal();
   test_unexecuted_tool_plan_detected();
   test_unexecuted_tool_plan_preserves_findings();
   test_reject_degenerate_owned_response();
   test_reject_degenerate_preserves_good_response();
   test_reject_degenerate_null_inputs();
   test_reject_degenerate_zero_error_buffer();
   test_reject_degenerate_default_message();

   test_diagnostic_with_agent_name();
   test_diagnostic_without_agent_name();
   test_diagnostic_empty_agent_name();
   test_diagnostic_null_buf();
   test_final_tool_call_diagnostic_without_partial_text();
   test_final_tool_call_diagnostic_preserves_partial_text();
   test_final_tool_call_diagnostic_filters_raw_tool_fragment();
   test_final_tool_call_diagnostic_null_buf();

   test_circuit_breaker_below_threshold();
   test_circuit_breaker_at_threshold();
   test_circuit_breaker_above_threshold();
   test_circuit_breaker_zero();
   test_circuit_breaker_negative();

   test_final_response_mode_requires_tools();
   test_final_response_mode_before_policy();
   test_final_response_mode_soft_policy();
   test_final_response_mode_hard_last_turn();
   test_final_response_tool_policy();
   test_final_response_mode_primary_session_never_hard();

   printf("  all tests passed\n");
   return 0;
}
