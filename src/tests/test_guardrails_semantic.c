/* test_guardrails_semantic.c: unit tests for guardrails_semantic.c and
 * db1/guardrail_events.c.
 *
 * Tests: score-band mapping, sidecar fallback on bad output, bounded input
 * extraction, and DB1 event round-trip. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db1.h"
#include "guardrail_events.h"
#include "guardrails_semantic.h"
#include "platform_test_util.h"

/* ── gsem_policy ──────────────────────────────────────────────────────────── */

static void test_policy_allow(void)
{
   gsem_output_t out;
   memset(&out, 0, sizeof(out));
   out.parse_ok = 1;
   out.overall = 0.10;
   assert(strcmp(gsem_policy(&out, 0.40, 0.70, 0.90), "allow") == 0);
}

static void test_policy_warn(void)
{
   gsem_output_t out;
   memset(&out, 0, sizeof(out));
   out.parse_ok = 1;
   out.overall = 0.55;
   assert(strcmp(gsem_policy(&out, 0.40, 0.70, 0.90), "warn") == 0);
}

static void test_policy_prompt(void)
{
   gsem_output_t out;
   memset(&out, 0, sizeof(out));
   out.parse_ok = 1;
   out.overall = 0.75;
   assert(strcmp(gsem_policy(&out, 0.40, 0.70, 0.90), "prompt") == 0);
}

static void test_policy_block(void)
{
   gsem_output_t out;
   memset(&out, 0, sizeof(out));
   out.parse_ok = 1;
   out.overall = 0.92;
   assert(strcmp(gsem_policy(&out, 0.40, 0.70, 0.90), "block") == 0);
}

static void test_policy_fallback_on_parse_failure(void)
{
   gsem_output_t out;
   memset(&out, 0, sizeof(out));
   out.parse_ok = 0;
   out.overall = 0.99; /* high score but parse_ok=0 → must allow */
   assert(strcmp(gsem_policy(&out, 0.40, 0.70, 0.90), "allow") == 0);
}

static void test_policy_null_fallback(void)
{
   assert(strcmp(gsem_policy(NULL, 0.40, 0.70, 0.90), "allow") == 0);
}

static void test_format_advisory_messages(void)
{
   gsem_output_t out;
   memset(&out, 0, sizeof(out));
   out.overall = 0.54;
   snprintf(out.labels, sizeof(out.labels), "off_scope_refactor");

   char msg[256];
   assert(gsem_format_advisory_message(msg, sizeof(msg), "warn", &out) == 1);
   assert(strcmp(msg, "semantic guardrail: elevated risk (score=0.54, "
                      "labels=[off_scope_refactor]) - proceeding") == 0);

   out.overall = 0.83;
   snprintf(out.labels, sizeof(out.labels), "verification_bypass,test_removal");
   assert(gsem_format_advisory_message(msg, sizeof(msg), "prompt", &out) == 1);
   assert(strcmp(msg, "semantic guardrail: high risk (score=0.83, "
                      "labels=[verification_bypass,test_removal]) - proceeding "
                      "(advisory mode)") == 0);

   msg[0] = '\0';
   assert(gsem_format_advisory_message(msg, sizeof(msg), "allow", &out) == 0);
   assert(msg[0] == '\0');
}

/* ── gsem_assess fallback ─────────────────────────────────────────────────── */

static void test_assess_empty_command(void)
{
   gsem_input_t in;
   memset(&in, 0, sizeof(in));
   snprintf(in.tool_name, sizeof(in.tool_name), "Edit");

   gsem_output_t out;
   int rc = gsem_assess(&in, "", &out);
   assert(rc != 0);
   assert(out.parse_ok == 0);
   assert(strcmp(out.recommendation, "allow") == 0);
}

static void test_assess_null_command(void)
{
   gsem_input_t in;
   memset(&in, 0, sizeof(in));
   gsem_output_t out;
   int rc = gsem_assess(&in, NULL, &out);
   assert(rc != 0);
   assert(out.parse_ok == 0);
}

static void test_assess_invalid_json_from_command(void)
{
   /* Echo garbage to stdout — should degrade gracefully */
   gsem_sidecar_reset_backoff();
   gsem_input_t in;
   memset(&in, 0, sizeof(in));
   snprintf(in.tool_name, sizeof(in.tool_name), "Edit");

   gsem_output_t out;
   int rc = gsem_assess(&in, "echo 'not-json'", &out);
   /* rc may be 0 (command ran) or -1 (parse failed) */
   (void)rc;
   assert(out.parse_ok == 0);
   assert(strcmp(out.recommendation, "allow") == 0);
}

static void test_assess_valid_sidecar_response(void)
{
   gsem_sidecar_reset_backoff();
   /* Provide a minimal well-formed sidecar response via echo */
   const char *cmd = "echo '{\"outputs\":{\"risk\":{\"overall\":0.72,\"action_risk\":0.6,"
                     "\"diff_risk\":0.8,\"drift_risk\":0.4,\"antipattern_similarity\":0.3},"
                     "\"labels\":[\"verification_bypass\"],\"recommendation\":\"prompt\"},"
                     "\"evidence\":{\"explanation\":\"test explanation\"}}'";

   gsem_input_t in;
   memset(&in, 0, sizeof(in));
   snprintf(in.tool_name, sizeof(in.tool_name), "Edit");

   gsem_output_t out;
   int rc = gsem_assess(&in, cmd, &out);
   assert(rc == 0);
   assert(out.parse_ok == 1);
   assert(out.overall > 0.71 && out.overall < 0.73);
   assert(out.diff_risk > 0.79 && out.diff_risk < 0.81);
   assert(strcmp(out.recommendation, "prompt") == 0);
   assert(strcmp(out.labels, "verification_bypass") == 0);
   assert(strcmp(out.explanation, "test explanation") == 0);
}

/* ── gsem_build_input ─────────────────────────────────────────────────────── */

static void test_build_input_bounded(void)
{
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "file_path", "/some/file.c");
   cJSON_AddStringToObject(root, "command", "rm -rf /");
   cJSON_AddStringToObject(root, "old_string", "old content");
   cJSON_AddStringToObject(root, "new_string", "new content");

   gsem_input_t in;
   gsem_build_input("Edit", root, "/cwd", "approve", &in);
   cJSON_Delete(root);

   assert(strcmp(in.tool_name, "Edit") == 0);
   assert(strcmp(in.cwd, "/cwd") == 0);
   assert(strcmp(in.session_mode, "approve") == 0);
   assert(strcmp(in.paths, "/some/file.c") == 0);
   assert(strcmp(in.shell_cmd, "rm -rf /") == 0);
   assert(strcmp(in.old_excerpt, "old content") == 0);
   assert(strcmp(in.new_excerpt, "new content") == 0);
}

static void test_build_input_execute_script_body(void)
{
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "language", "bash");
   cJSON_AddStringToObject(root, "body", "rm -rf /tmp/aimee-script-risk");

   gsem_input_t in;
   gsem_build_input("Bash", root, "/cwd", "approve", &in);
   cJSON_Delete(root);

   assert(strcmp(in.shell_cmd, "rm -rf /tmp/aimee-script-risk") == 0);
}

static void test_build_input_command_precedes_body(void)
{
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "command", "git push origin HEAD");
   cJSON_AddStringToObject(root, "body", "echo ignored");

   gsem_input_t in;
   gsem_build_input("Bash", root, "/cwd", "approve", &in);
   cJSON_Delete(root);

   assert(strcmp(in.shell_cmd, "git push origin HEAD") == 0);
}

static void test_build_input_null_json(void)
{
   gsem_input_t in;
   gsem_build_input("Bash", NULL, "/cwd", "plan", &in);
   assert(strcmp(in.tool_name, "Bash") == 0);
   assert(in.paths[0] == '\0');
   assert(in.shell_cmd[0] == '\0');
}

/* ── DB1 event round-trip ─────────────────────────────────────────────────── */

static void test_event_insert(const char *path)
{
   platform_test_remove_sqlite(path);
   assert(db1_init(path) == 0);

   guardrail_event_t e;
   memset(&e, 0, sizeof(e));
   snprintf(e.session_id, sizeof(e.session_id), "sess-g1");
   snprintf(e.tool_name, sizeof(e.tool_name), "Edit");
   e.overall_risk = 0.75;
   e.action_risk = 0.60;
   e.diff_risk = 0.80;
   e.drift_risk = 0.40;
   e.antipattern_similarity = 0.30;
   snprintf(e.recommendation, sizeof(e.recommendation), "prompt");
   snprintf(e.labels, sizeof(e.labels), "verification_bypass");
   snprintf(e.final_action, sizeof(e.final_action), "dry_run");
   snprintf(e.explanation, sizeof(e.explanation), "test");
   e.dry_run = 1;

   int rc = db1_guardrail_event_insert(&e);
   assert(rc == 0);

   db1_shutdown();
   platform_test_remove_sqlite(path);
}

static void test_gsem_record(const char *path)
{
   platform_test_remove_sqlite(path);
   assert(db1_init(path) == 0);

   gsem_input_t in;
   memset(&in, 0, sizeof(in));
   snprintf(in.tool_name, sizeof(in.tool_name), "Write");

   gsem_output_t out;
   memset(&out, 0, sizeof(out));
   out.parse_ok = 1;
   out.overall = 0.45;
   snprintf(out.recommendation, sizeof(out.recommendation), "warn");
   snprintf(out.labels, sizeof(out.labels), "off_scope");

   gsem_record("sess-g2", &in, &out, "warn", 0);

   out.overall = 0.75;
   snprintf(out.recommendation, sizeof(out.recommendation), "prompt");
   gsem_record("sess-g2", &in, &out, "prompt", 0);

   out.overall = 0.95;
   snprintf(out.recommendation, sizeof(out.recommendation), "block");
   gsem_record("sess-g2", &in, &out, "dry_run", 1);

   guardrail_event_counts_t counts;
   assert(db1_guardrail_event_counts_7d(&counts) == 0);
   assert(counts.warn == 1);
   assert(counts.prompt == 1);
   assert(counts.block == 0);
   assert(counts.dry_run == 1);

   int session_count = -1;
   assert(db1_guardrail_event_session_advisory_count("sess-g2", &session_count) == 0);
   assert(session_count == 2);

   db1_shutdown();
   platform_test_remove_sqlite(path);
}

static void test_backoff_suppresses_after_failure(void)
{
   gsem_sidecar_reset_backoff();

   gsem_input_t in;
   gsem_output_t out;
   memset(&in, 0, sizeof(in));

   /* First failure arms the backoff */
   int rc1 = gsem_assess(&in, "false", &out); /* 'false' always exits 1 */
   assert(rc1 == -1);
   assert(out.parse_ok == 0);

   /* Second call within backoff window is suppressed silently */
   int rc2 = gsem_assess(&in, "false", &out);
   assert(rc2 == -1);
   assert(out.parse_ok == 0);
   assert(gsem_sidecar_in_backoff() == 1);

   /* After manual reset, next call reaches the sidecar again */
   gsem_sidecar_reset_backoff();
   assert(gsem_sidecar_in_backoff() == 0);
}

int main(void)
{
   printf("guardrails_semantic: ");

   char path[] = "/tmp/test_gsem_XXXXXX.db";
   int fd = mkstemps(path, 3);
   if (fd >= 0)
      close(fd);

   test_policy_allow();
   test_policy_warn();
   test_policy_prompt();
   test_policy_block();
   test_policy_fallback_on_parse_failure();
   test_policy_null_fallback();
   test_format_advisory_messages();

   test_assess_empty_command();
   test_assess_null_command();
   test_assess_invalid_json_from_command();
   test_assess_valid_sidecar_response();

   test_build_input_bounded();
   test_build_input_execute_script_body();
   test_build_input_command_precedes_body();
   test_build_input_null_json();

   test_event_insert(path);
   test_gsem_record(path);

   test_backoff_suppresses_after_failure();

   printf("ok\n");
   return 0;
}
