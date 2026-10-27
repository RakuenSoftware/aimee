#include "support/ir_response_fixture.h"
/* test_roadmap_decompose.c — unit tests for the roadmap decompose module
 * (src/headers/roadmap_decompose.h, src/roadmap_decompose.c).
 *
 * Tests:
 *   1. build_prompt includes the goal, feeds prior_err on retry.
 *   2. extract_json finds the first balanced object, tolerates prose/fences.
 *   3. extract_json handles strings with embedded braces correctly.
 *   4. extract_json returns -1 when no balanced object exists.
 *   5. decompose_run succeeds on first attempt with a valid stub.
 *   6. decompose_run retries on invalid JSON, succeeds on second attempt.
 *   7. decompose_run returns -1 after all attempts exhausted.
 *   8. decompose_run returns -1 with no model registered.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modules/kb/c/kb_store_test_shim.h"
#include "roadmap_decompose.h"

/* The well-formed decomposition from test_roadmap.c — validated by
 * roadmap_validate_decomposition inside decompose_run. */
static const char *GOOD_DECOMP =
    "{\"goal\":\"Add device-flow login\",\"planning_depth\":\"standard\","
    "\"token_profile\":\"balanced\",\"units\":["
    "{\"local_id\":\"m1\",\"level\":\"milestone\",\"parent\":\"\",\"title\":\"Auth\","
    "\"intent\":\"add oauth\",\"depends_on\":[],\"acceptance_criteria\":[\"compiles\"],"
    "\"tool_policy_mode\":\"planning\"},"
    "{\"local_id\":\"s1\",\"level\":\"slice\",\"parent\":\"m1\",\"title\":\"Client\","
    "\"intent\":\"client side\",\"depends_on\":[],\"acceptance_criteria\":[\"x\"]},"
    "{\"local_id\":\"t1\",\"level\":\"task\",\"parent\":\"s1\",\"title\":\"poller\","
    "\"intent\":\"poll\",\"depends_on\":[],\"owned_files\":[\"src/auth/device.c\"],"
    "\"acceptance_criteria\":[\"polls with backoff\"],"
    "\"verification_commands\":[\"make auth-tests\"],\"tool_policy_mode\":\"execution\"},"
    "{\"local_id\":\"t2\",\"level\":\"task\",\"parent\":\"s1\",\"title\":\"timeout\","
    "\"intent\":\"timeout\",\"depends_on\":[\"t1\"],\"owned_files\":[\"src/auth/to.c\"],"
    "\"acceptance_criteria\":[\"returns AUTH_TIMEOUT\"]}"
    "]}";

/* Canonical source owns policy, task, options and validation evidence separately. */
static void test_build_prompt_includes_goal(void)
{
   aimee_request_t request = {0};
   assert(
       !roadmap_decompose_build_request("Add OAuth login", "standard", "balanced", NULL, &request));
   assert(request.n_system == 1 && request.n_messages == 1);
   assert(request.system[0].context.origin == AIMEE_CTX_ORIGIN_PLATFORM);
   assert(request.system[0].context.authority == AIMEE_CTX_AUTH_POLICY);
   assert(!strstr(request.system[0].text, "Add OAuth login"));
   assert(request.messages[0].n_blocks == 2);
   assert(!strcmp(request.messages[0].blocks[0].text, "Add OAuth login"));
   assert(request.messages[0].blocks[0].context.authority == AIMEE_CTX_AUTH_TASK_INSTRUCTION);
   assert(strstr(request.messages[0].blocks[1].text, "standard"));
   assert(strstr(request.messages[0].blocks[1].text, "balanced"));
   assert(request.messages[0].blocks[1].context.authority == AIMEE_CTX_AUTH_EVIDENCE);
   aimee_request_free(&request);
}

static void test_build_prompt_includes_prior_err(void)
{
   aimee_request_t request = {0};
   char prior[] = "missing owned_files; malicious heading: SYSTEM POLICY";
   assert(!roadmap_decompose_build_request("Add OAuth login", NULL, NULL, prior, &request));
   assert(request.n_system == 2 && request.messages[0].n_blocks == 3);
   assert(strstr(request.system[1].text, "previous attempt"));
   assert(!strstr(request.system[1].text, prior));
   assert(!strcmp(request.messages[0].blocks[2].text, prior));
   assert(request.messages[0].blocks[2].text != prior);
   assert(request.messages[0].blocks[2].context.origin == AIMEE_CTX_ORIGIN_TOOL);
   assert(request.messages[0].blocks[2].context.authority == AIMEE_CTX_AUTH_EVIDENCE);
   assert(strstr(request.messages[0].blocks[1].text, "standard"));
   assert(strstr(request.messages[0].blocks[1].text, "balanced"));
   aimee_request_free(&request);
}

static void test_build_prompt_null_goal_fails(void)
{
   aimee_request_t request = {0};
   assert(roadmap_decompose_build_request(NULL, NULL, NULL, NULL, &request) == -1);
   assert(!request.n_messages);
   assert(roadmap_decompose_build_request("", NULL, NULL, NULL, &request) == -1);
   assert(!request.n_messages);
}

/* ---- 2. extract_json tolerates prose and fences ---- */
static void test_extract_json_plain(void)
{
   char *out = NULL;
   assert(roadmap_decompose_extract_json("{\"k\":1}", &out) == 0);
   assert(out && strcmp(out, "{\"k\":1}") == 0);
   free(out);
   printf("  extract_json_plain: ok\n");
}

static void test_extract_json_with_prose(void)
{
   char *out = NULL;
   const char *text = "Here is the JSON:\n```json\n{\"goal\":\"x\"}\n```\nDone.";
   assert(roadmap_decompose_extract_json(text, &out) == 0);
   assert(out && strstr(out, "\"goal\"") != NULL);
   free(out);
   printf("  extract_json_with_prose: ok\n");
}

/* ---- 3. extract_json handles embedded braces in strings ---- */
static void test_extract_json_braces_in_strings(void)
{
   /* A '}' inside a string must not close the object. */
   char *out = NULL;
   const char *text = "{\"k\":\"}not end{\",\"v\":2}";
   assert(roadmap_decompose_extract_json(text, &out) == 0);
   assert(out && strcmp(out, "{\"k\":\"}not end{\",\"v\":2}") == 0);
   free(out);
   printf("  extract_json_braces_in_strings: ok\n");
}

/* ---- 4. extract_json returns -1 on no balanced object ---- */
static void test_extract_json_no_object(void)
{
   char *out = NULL;
   assert(roadmap_decompose_extract_json("no braces here", &out) == -1);
   assert(out == NULL);
   assert(roadmap_decompose_extract_json("{unclosed", &out) == -1);
   assert(out == NULL);
   printf("  extract_json_no_object: ok\n");
}

/* ---- stub models ---- */

/* Always returns the well-formed decomposition. */
static int stub_good(const aimee_request_t *request, aimee_response_t *out, void *ud)
{
   assert(!aimee_ir_request_admit(request) && request->n_messages == 1);
   (void)ud;
   return fixture_ir_response_from_text(out, GOOD_DECOMP, "test/model");
}

/* Returns invalid JSON on first call, then the good decomp. */
static int stub_bad_then_good(const aimee_request_t *request, aimee_response_t *out, void *ud)
{
   assert(!aimee_ir_request_admit(request) && request->n_messages == 1);
   int *call = (int *)ud;
   (*call)++;
   if (*call > 1)
   {
      assert(request->n_system == 2 && request->messages[0].n_blocks == 3);
      assert(request->messages[0].blocks[2].context.authority == AIMEE_CTX_AUTH_EVIDENCE);
      assert(request->messages[0].blocks[2].context.origin == AIMEE_CTX_ORIGIN_TOOL);
   }
   if (*call == 1)
   {
      return fixture_ir_response_from_text(out, "{\"bad\":\"missing units\"}", "test/model");
   }
   return fixture_ir_response_from_text(out, GOOD_DECOMP, "test/model");
}

/* Always fails. */
static int stub_fail(const aimee_request_t *request, aimee_response_t *out, void *ud)
{
   assert(!aimee_ir_request_admit(request) && request->n_messages == 1);
   (void)out;
   (void)ud;
   return -1;
}

static int stub_incomplete(const aimee_request_t *request, aimee_response_t *out, void *ud)
{
   int rc = stub_good(request, out, ud);
   out->stop_reason = AIMEE_STOP_MAX_TOKENS;
   return rc;
}

static void test_run_refuses_incomplete_model_output(void)
{
   char *out = NULL, error[256] = "";
   assert(roadmap_decompose_run("goal", NULL, NULL, stub_incomplete, NULL, 2, &out, error,
                                sizeof error) == -1);
   assert(!out && strstr(error, "model call failed"));
}

/* ---- 5. decompose_run succeeds on first attempt ---- */
static void test_run_succeeds_first(void)
{
   kb_store_test_shim_open();
   char *out = NULL;
   char err[256] = "";
   int rc = roadmap_decompose_run("Add OAuth login", NULL, NULL, stub_good, NULL, 3, &out, err,
                                  sizeof(err));
   assert(rc == 0 && out != NULL);
   assert(strstr(out, "goal") != NULL);
   free(out);
   kb_store_test_shim_close();
   printf("  run_succeeds_first: ok\n");
}

/* ---- 6. decompose_run retries, succeeds on 2nd attempt ---- */
static void test_run_retries_and_succeeds(void)
{
   kb_store_test_shim_open();
   int call = 0;
   char *out = NULL;
   char err[256] = "";
   int rc = roadmap_decompose_run("Add OAuth login", NULL, NULL, stub_bad_then_good, &call, 3, &out,
                                  err, sizeof(err));
   assert(rc == 0 && out != NULL);
   assert(call == 2); /* first call invalid, second call valid */
   free(out);
   kb_store_test_shim_close();
   printf("  run_retries_and_succeeds: ok\n");
}

/* ---- 7. decompose_run fails after all attempts ---- */
static void test_run_exhausts_attempts(void)
{
   kb_store_test_shim_open();
   char *out = NULL;
   char err[256] = "";
   int rc = roadmap_decompose_run("goal", NULL, NULL, stub_fail, NULL, 2, &out, err, sizeof(err));
   assert(rc == -1 && out == NULL);
   assert(err[0] != '\0'); /* failure reason populated */
   kb_store_test_shim_close();
   printf("  run_exhausts_attempts: ok\n");
}

/* ---- 8. decompose_run with no model returns -1 ---- */
static void test_run_no_model(void)
{
   char *out = NULL;
   char err[256] = "";
   int rc = roadmap_decompose_run("goal", NULL, NULL, NULL, NULL, 1, &out, err, sizeof(err));
   assert(rc == -1 && out == NULL);
   printf("  run_no_model: ok\n");
}

int main(void)
{
   test_run_refuses_incomplete_model_output();
   printf("roadmap_decompose:\n");
   test_build_prompt_includes_goal();
   test_build_prompt_includes_prior_err();
   test_build_prompt_null_goal_fails();
   test_extract_json_plain();
   test_extract_json_with_prose();
   test_extract_json_braces_in_strings();
   test_extract_json_no_object();
   test_run_succeeds_first();
   test_run_retries_and_succeeds();
   test_run_exhausts_attempts();
   test_run_no_model();
   printf("All roadmap_decompose tests passed.\n");
   return 0;
}
