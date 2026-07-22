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

#include "db2_test_shim.h"
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

/* ---- 1. build_prompt embeds goal and prior_err ---- */
static void test_build_prompt_includes_goal(void)
{
   char *out = NULL;
   int rc = roadmap_decompose_build_prompt("Add OAuth login", "standard", "balanced", NULL, &out);
   assert(rc == 0 && out != NULL);
   assert(strstr(out, "Add OAuth login") != NULL);
   assert(strstr(out, "standard") != NULL);
   assert(strstr(out, "balanced") != NULL);
   /* no prior error → no retry text */
   assert(strstr(out, "previous attempt") == NULL);
   free(out);
   printf("  build_prompt_includes_goal: ok\n");
}

static void test_build_prompt_includes_prior_err(void)
{
   char *out = NULL;
   int rc =
       roadmap_decompose_build_prompt("Add OAuth login", NULL, NULL, "missing owned_files", &out);
   assert(rc == 0 && out != NULL);
   assert(strstr(out, "missing owned_files") != NULL);
   assert(strstr(out, "previous attempt") != NULL);
   /* NULL depth/profile → defaults */
   assert(strstr(out, "standard") != NULL);
   assert(strstr(out, "balanced") != NULL);
   free(out);
   printf("  build_prompt_includes_prior_err: ok\n");
}

static void test_build_prompt_null_goal_fails(void)
{
   char *out = NULL;
   assert(roadmap_decompose_build_prompt(NULL, NULL, NULL, NULL, &out) == -1);
   assert(out == NULL);
   assert(roadmap_decompose_build_prompt("", NULL, NULL, NULL, &out) == -1);
   assert(out == NULL);
   printf("  build_prompt_null_goal_fails: ok\n");
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
static int stub_good(const char *prompt, char **out, void *ud)
{
   (void)prompt;
   (void)ud;
   *out = strdup(GOOD_DECOMP);
   return *out ? 0 : -1;
}

/* Returns invalid JSON on first call, then the good decomp. */
static int stub_bad_then_good(const char *prompt, char **out, void *ud)
{
   (void)prompt;
   int *call = (int *)ud;
   (*call)++;
   if (*call == 1)
   {
      *out = strdup("{\"bad\":\"missing units\"}");
      return *out ? 0 : -1;
   }
   *out = strdup(GOOD_DECOMP);
   return *out ? 0 : -1;
}

/* Always fails. */
static int stub_fail(const char *prompt, char **out, void *ud)
{
   (void)prompt;
   (void)out;
   (void)ud;
   return -1;
}

/* ---- 5. decompose_run succeeds on first attempt ---- */
static void test_run_succeeds_first(void)
{
   db2_test_shim_open();
   char *out = NULL;
   char err[256] = "";
   int rc = roadmap_decompose_run("Add OAuth login", NULL, NULL, stub_good, NULL, 3, &out, err,
                                  sizeof(err));
   assert(rc == 0 && out != NULL);
   assert(strstr(out, "goal") != NULL);
   free(out);
   db2_test_shim_close();
   printf("  run_succeeds_first: ok\n");
}

/* ---- 6. decompose_run retries, succeeds on 2nd attempt ---- */
static void test_run_retries_and_succeeds(void)
{
   db2_test_shim_open();
   int call = 0;
   char *out = NULL;
   char err[256] = "";
   int rc = roadmap_decompose_run("Add OAuth login", NULL, NULL, stub_bad_then_good, &call, 3, &out,
                                  err, sizeof(err));
   assert(rc == 0 && out != NULL);
   assert(call == 2); /* first call invalid, second call valid */
   free(out);
   db2_test_shim_close();
   printf("  run_retries_and_succeeds: ok\n");
}

/* ---- 7. decompose_run fails after all attempts ---- */
static void test_run_exhausts_attempts(void)
{
   db2_test_shim_open();
   char *out = NULL;
   char err[256] = "";
   int rc = roadmap_decompose_run("goal", NULL, NULL, stub_fail, NULL, 2, &out, err, sizeof(err));
   assert(rc == -1 && out == NULL);
   assert(err[0] != '\0'); /* failure reason populated */
   db2_test_shim_close();
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
