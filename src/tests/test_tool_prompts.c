/* test_tool_prompts.c: unit tests for agent_collect_tool_prompts */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"
#include "agent_exec.h"
#include "db.h"
#include "db1.h"
#include "modules/db2/c/db2.h"
#include "db_postgres.h"
#include "modules/db2/c/db2_test_shim.h"
#include "../modules/db2/c/db2_internal.h"

static void setup(void)
{
   db2_test_shim_close();
   assert(db1_init(":memory:") == 0);
   db2_test_shim_open();
   /* Schema apply seeds the default tool_registry (bash, read_file, ...).
    * Clear it so each case sees only the rows it explicitly inserts. */
   (void)aimee_pg_exec(db2_conn(), "DELETE FROM tool_registry", NULL, 0);
}

static void teardown(void)
{
   db2_test_shim_close();
   db1_shutdown();
}

/* Insert a tool into tool_registry (postgres) for testing. */
static void insert_tool(const char *name, const char *tool_prompt, int enabled)
{
   void *conn = db2_conn();
   assert(conn);
   const char *sql =
       tool_prompt
           ? "INSERT INTO tool_registry (name, description, input_schema, side_effect, idempotent,"
             " enabled, tool_prompt) VALUES (?1, 'Test tool', '{\"type\":\"object\"}', 'read', 1,"
             " ?2, ?3)"
           : "INSERT INTO tool_registry (name, description, input_schema, side_effect, idempotent,"
             " enabled) VALUES (?1, 'Test tool', '{\"type\":\"object\"}', 'read', 1, ?2)";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   assert(st);
   aimee_pg_bind_text(st, "?1", name);
   aimee_pg_bind_int(st, "?2", enabled);
   if (tool_prompt)
      aimee_pg_bind_text(st, "?3", tool_prompt);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

static void test_no_prompts_returns_null(void)
{
   setup();

   /* Tool with neither a DB prompt nor an embedded file — no output. */
   insert_tool("no_such_tool_abc", NULL, 1);

   char *result = agent_collect_tool_prompts();
   assert(result == NULL);

   teardown();
}

static void test_single_prompt_included(void)
{
   setup();

   insert_tool("bash", "Prefer non-interactive flags.", 1);

   char *result = agent_collect_tool_prompts();
   assert(result != NULL);
   assert(strstr(result, "## Tool Usage Notes") != NULL);
   assert(strstr(result, "### bash") != NULL);
   assert(strstr(result, "Prefer non-interactive flags.") != NULL);

   free(result);
   teardown();
}

static void test_multiple_prompts_all_included(void)
{
   setup();

   insert_tool("bash", "Use non-interactive flags.", 1);
   insert_tool("read_file", "Read minimum lines needed.", 1);
   insert_tool("write_file", "Verify directory exists first.", 1);

   char *result = agent_collect_tool_prompts();
   assert(result != NULL);
   assert(strstr(result, "## Tool Usage Notes") != NULL);
   assert(strstr(result, "### bash") != NULL);
   assert(strstr(result, "### read_file") != NULL);
   assert(strstr(result, "### write_file") != NULL);
   assert(strstr(result, "Use non-interactive flags.") != NULL);
   assert(strstr(result, "Read minimum lines needed.") != NULL);
   assert(strstr(result, "Verify directory exists first.") != NULL);

   free(result);
   teardown();
}

static void test_disabled_tool_excluded(void)
{
   setup();

   insert_tool("bash", "Use non-interactive flags.", 1);
   insert_tool("dangerous_tool", "This should not appear.", 0); /* disabled */

   char *result = agent_collect_tool_prompts();
   assert(result != NULL);
   assert(strstr(result, "### bash") != NULL);
   assert(strstr(result, "dangerous_tool") == NULL);
   assert(strstr(result, "This should not appear.") == NULL);

   free(result);
   teardown();
}

static void test_empty_prompt_excluded(void)
{
   setup();

   /* Use a tool name with no embedded fallback so an empty DB prompt
    * produces no output. */
   insert_tool("no_such_tool_abc", "", 1);

   char *result = agent_collect_tool_prompts();
   assert(result == NULL);

   teardown();
}

static void test_uninitialized_db_returns_null(void)
{
   /* Empty tool_registry → iter_prompts returns no rows → no output. */
   setup();
   char *result = agent_collect_tool_prompts();
   assert(result == NULL);
   teardown();
}

/* With no DB tool_prompt set, a tool whose name matches an embedded file
 * (src/tool_prompts/bash.md) should still have its prompt included. */
static void test_embedded_fallback_used_when_db_prompt_empty(void)
{
   setup();

   insert_tool("bash", NULL, 1); /* no DB prompt — should fall back */

   char *result = agent_collect_tool_prompts();
   assert(result != NULL);
   assert(strstr(result, "### bash") != NULL);
   assert(strstr(result, "non-interactive") != NULL);

   free(result);
   teardown();
}

/* A DB tool_prompt explicitly set takes precedence over the embedded file. */
static void test_db_prompt_overrides_embedded(void)
{
   setup();

   insert_tool("bash", "DB override text.", 1);

   char *result = agent_collect_tool_prompts();
   assert(result != NULL);
   assert(strstr(result, "DB override text.") != NULL);
   assert(strstr(result, "non-interactive") == NULL);

   free(result);
   teardown();
}

/* A tool with neither a DB prompt nor an embedded prompt produces no entry. */
static void test_no_embedded_and_no_db_prompt_skipped(void)
{
   setup();

   insert_tool("no_such_tool_xyz", NULL, 1);

   char *result = agent_collect_tool_prompts();
   assert(result == NULL);

   teardown();
}

static void test_header_appears_once(void)
{
   setup();

   insert_tool("bash", "Prompt A.", 1);
   insert_tool("grep", "Prompt B.", 1);

   char *result = agent_collect_tool_prompts();
   assert(result != NULL);

   /* Header should appear exactly once */
   char *first = strstr(result, "## Tool Usage Notes");
   assert(first != NULL);
   char *second = strstr(first + 1, "## Tool Usage Notes");
   assert(second == NULL);

   free(result);
   teardown();
}

int main(void)
{
   test_no_prompts_returns_null();
   test_single_prompt_included();
   test_multiple_prompts_all_included();
   test_disabled_tool_excluded();
   test_empty_prompt_excluded();
   test_uninitialized_db_returns_null();
   test_header_appears_once();
   test_embedded_fallback_used_when_db_prompt_empty();
   test_db_prompt_overrides_embedded();
   test_no_embedded_and_no_db_prompt_skipped();
   printf("tool_prompts: all tests passed\n");
   return 0;
}
