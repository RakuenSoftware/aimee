/* test_tool_validation.c: unit tests for tool call validation and recovery
 *
 * Covers: tool_suggest (nearest-match), tool_validate (unknown tool,
 * disabled tool, missing required field, wrong type, malformed JSON,
 * alias normalization, type coercion, valid call, recovery hint).
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../headers/aimee.h"
#include "../headers/agent_exec.h"
#include "../headers/tool_args_coerce.h"
#include <aimee/tools/agent_tools.h>
#include "db1_client/db1.h"
#include "../modules/db2/c/db2.h"
#include "../modules/db2/c/db2_test_shim.h"
#include "../modules/db2/c/db2_internal.h"
#include "../modules/db2/c/db_postgres.h"

/* --- helpers --- */

static void setup_db(void)
{
   db2_test_shim_close();
   db2_test_shim_open();
}

static void teardown_db(void)
{
   db2_test_shim_close();
}

/* --- tool_suggest --- */

static void test_suggest_exact_match(void)
{
   const char *s = tool_suggest("bash");
   assert(s != NULL);
   assert(strcmp(s, "bash") == 0);
   printf("  PASS: tool_suggest exact 'bash'\n");
}

static void test_suggest_one_edit(void)
{
   /* 'red_file' differs from 'read_file' by 1 insertion */
   const char *s = tool_suggest("red_file");
   assert(s != NULL);
   assert(strcmp(s, "read_file") == 0);
   printf("  PASS: tool_suggest 'red_file' -> 'read_file'\n");
}

static void test_suggest_two_edits(void)
{
   /* 'read_flie' differs from 'read_file' by 2 substitutions */
   const char *s = tool_suggest("read_flie");
   assert(s != NULL);
   assert(strcmp(s, "read_file") == 0);
   printf("  PASS: tool_suggest 'read_flie' -> 'read_file'\n");
}

static void test_suggest_no_match(void)
{
   /* Completely different string — no close match within threshold */
   const char *s = tool_suggest("xyzabcdefghijklmn");
   assert(s == NULL);
   printf("  PASS: tool_suggest returns NULL for distant name\n");
}

/* --- tool_validate: error cases --- */

static void test_validate_unknown_tool_with_suggestion(void)
{
   setup_db();
   char err[256] = {0};
   /* 'read_flie' is close to 'read_file' */
   int rc = tool_validate("read_flie", "{\"path\":\"/tmp/x\"}", err, sizeof(err));
   assert(rc == -1);
   assert(strstr(err, "unknown") != NULL);
   assert(strstr(err, "read_file") != NULL);
   teardown_db();
   printf("  PASS: unknown tool returns -1 with nearest-match suggestion\n");
}

static void test_validate_unknown_tool_no_suggestion(void)
{
   setup_db();
   char err[256] = {0};
   int rc = tool_validate("zzz_completely_unknown_tool", "{}", err, sizeof(err));
   assert(rc == -1);
   assert(strstr(err, "unknown") != NULL);
   teardown_db();
   printf("  PASS: unknown tool with no close match returns -1\n");
}

static void test_validate_disabled_tool(void)
{
   setup_db();
   (void)aimee_pg_exec(db2_conn(), "UPDATE tool_registry SET enabled = 0 WHERE name = 'bash'", NULL,
                       0);
   char err[256] = {0};
   int rc = tool_validate("bash", "{\"command\":\"ls\"}", err, sizeof(err));
   assert(rc == -1);
   assert(strstr(err, "disabled") != NULL);
   teardown_db();
   printf("  PASS: disabled tool returns -1\n");
}

static void test_validate_missing_required_field(void)
{
   setup_db();
   char err[256] = {0};
   /* read_file requires 'path' */
   int rc = tool_validate("read_file", "{}", err, sizeof(err));
   assert(rc == -1);
   assert(strstr(err, "path") != NULL);
   teardown_db();
   printf("  PASS: missing required field returns -1\n");
}

static void test_validate_wrong_type(void)
{
   setup_db();
   char err[256] = {0};
   /* 'path' should be string; passing integer should fail */
   int rc = tool_validate("read_file", "{\"path\":42}", err, sizeof(err));
   assert(rc == -1);
   teardown_db();
   printf("  PASS: wrong field type returns -1\n");
}

static void test_validate_malformed_json(void)
{
   setup_db();
   char err[256] = {0};
   int rc = tool_validate("read_file", "{not valid json", err, sizeof(err));
   assert(rc == -1);
   teardown_db();
   printf("  PASS: malformed JSON returns -1\n");
}

static void test_validate_write_file_missing_content(void)
{
   setup_db();
   char err[256] = {0};
   /* write_file requires both path and content */
   int rc = tool_validate("write_file", "{\"path\":\"/tmp/out.txt\"}", err, sizeof(err));
   assert(rc == -1);
   assert(strstr(err, "content") != NULL);
   teardown_db();
   printf("  PASS: write_file missing 'content' returns -1\n");
}

/* --- tool_validate: success cases --- */

static void test_validate_valid_read_file(void)
{
   setup_db();
   char err[256] = {0};
   int rc = tool_validate("read_file", "{\"path\":\"/tmp/test.txt\"}", err, sizeof(err));
   assert(rc == 0);
   assert(err[0] == '\0');
   teardown_db();
   printf("  PASS: valid read_file call returns 0\n");
}

static void test_validate_valid_write_file(void)
{
   setup_db();
   char err[256] = {0};
   int rc = tool_validate("write_file", "{\"path\":\"/tmp/out.txt\",\"content\":\"hello\"}", err,
                          sizeof(err));
   assert(rc == 0);
   teardown_db();
   printf("  PASS: valid write_file call returns 0\n");
}

static void test_validate_valid_bash(void)
{
   setup_db();
   char err[256] = {0};
   int rc = tool_validate("bash", "{\"command\":\"echo hello\"}", err, sizeof(err));
   assert(rc == 0);
   teardown_db();
   printf("  PASS: valid bash call returns 0\n");
}

/* --- alias resolution across the real pipeline ---------------------------
 *
 * Alias resolution moved OUT of tool_validate. It used to happen on a copy the
 * validator discarded, so validation accepted {"filepath": ...} and the tool
 * then received no `path` at all -- validation was more permissive than
 * execution. It now happens once in the canonicalizer, before any gate, and
 * these tests exercise that order rather than the validator alone. */

/* What src/posix/agent_runtime.c does per call: canonicalize once, then let
 * every gate judge the bytes that will actually execute. */
static int canonicalize_then_validate(const char *tool, const char *raw, char *err, size_t err_len)
{
   char *canonical = tool_args_canonicalize_json(agent_tool_get_schema_cached(tool), raw);
   int rc = tool_validate(tool, canonical ? canonical : raw, err, err_len);
   free(canonical);
   return rc;
}

static void test_validate_alias_filepath_normalized(void)
{
   setup_db();
   char err[256] = {0};
   const char *raw = "{\"filepath\":\"/tmp/x\"}";
   int rc = canonicalize_then_validate("read_file", raw, err, sizeof(err));
   assert(rc == 0);

   /* Load-bearing: the alias is resolved in the bytes that go on to execute,
    * not merely inside the validator. */
   char *canonical = tool_args_canonicalize_json(agent_tool_get_schema_cached("read_file"), raw);
   assert(canonical != NULL);
   assert(strstr(canonical, "\"path\"") != NULL);
   assert(strstr(canonical, "filepath") == NULL);
   free(canonical);

   teardown_db();
   printf("  PASS: alias 'filepath' resolves to 'path' before the gates\n");
}

static void test_validate_alias_file_normalized(void)
{
   setup_db();
   char err[256] = {0};
   int rc = canonicalize_then_validate("read_file", "{\"file\":\"/tmp/x\"}", err, sizeof(err));
   assert(rc == 0);
   teardown_db();
   printf("  PASS: alias 'file' resolves to 'path' before the gates\n");
}

static void test_validate_alias_cmd_normalized(void)
{
   setup_db();
   char err[256] = {0};
   int rc = canonicalize_then_validate("bash", "{\"cmd\":\"ls\"}", err, sizeof(err));
   assert(rc == 0);
   teardown_db();
   printf("  PASS: alias 'cmd' resolves to 'command' before the gates\n");
}

/* tool_validate itself is now pure: it judges, it does not rewrite. If it still
 * normalized internally we would be back to validating one shape and running
 * another, which is the defect the canonicalizer exists to close. */
static void test_validate_does_not_rewrite_arguments(void)
{
   setup_db();
   char err[256] = {0};
   int rc = tool_validate("read_file", "{\"filepath\":\"/doc/a.txt\"}", err, sizeof(err));
   assert(rc != 0); /* `path` is required and an un-canonicalized call lacks it */
   teardown_db();
   printf("  PASS: tool_validate judges without rewriting\n");
}

int main(void)
{
   printf("test_tool_validation\n");

   test_suggest_exact_match();
   test_suggest_one_edit();
   test_suggest_two_edits();
   test_suggest_no_match();

   test_validate_unknown_tool_with_suggestion();
   test_validate_unknown_tool_no_suggestion();
   test_validate_disabled_tool();
   test_validate_missing_required_field();
   test_validate_wrong_type();
   test_validate_malformed_json();
   test_validate_write_file_missing_content();

   test_validate_valid_read_file();
   test_validate_valid_write_file();
   test_validate_valid_bash();

   test_validate_alias_filepath_normalized();
   test_validate_alias_file_normalized();
   test_validate_alias_cmd_normalized();
   test_validate_does_not_rewrite_arguments();

   printf("All tool_validation tests passed.\n");
   return 0;
}
