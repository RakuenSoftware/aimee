/* test_aimee_ir_session.c: what a turn did, measured from the IR.
 *
 * The measurement is the product behaviour here -- the thresholds decide
 * whether a session gets a nudge -- so each one is pinned at its boundary
 * rather than somewhere comfortably past it. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aimee/ir/aimee_ir.h>
#include "cJSON.h"

/* One assistant message whose blocks are the tool calls under test. */
static aimee_message_t g_msg;
static aimee_request_t g_req;
static aimee_block_t g_blocks[32];
static int g_n;

static void reset(void)
{
   for (int i = 0; i < g_n; i++)
   {
      free(g_blocks[i].tool_name);
      cJSON_Delete(g_blocks[i].tool_input);
   }
   memset(g_blocks, 0, sizeof g_blocks);
   memset(&g_msg, 0, sizeof g_msg);
   memset(&g_req, 0, sizeof g_req);
   g_n = 0;
}

static void call(const char *name, const char *key, const char *value)
{
   aimee_block_t *b = &g_blocks[g_n++];
   b->type = AIMEE_BLK_TOOL_USE;
   b->tool_name = strdup(name);
   if (key)
   {
      b->tool_input = cJSON_CreateObject();
      cJSON_AddStringToObject(b->tool_input, key, value);
   }
}

static void measure(aimee_ir_session_metrics_t *out)
{
   g_msg.role = (char *)"assistant";
   g_msg.blocks = g_blocks;
   g_msg.n_blocks = g_n;
   g_req.messages = &g_msg;
   g_req.n_messages = 1;
   aimee_ir_session_measure(&g_req, out);
}

int main(void)
{
   aimee_ir_session_metrics_t m;

   /* No request at all is a zeroed result, not a crash: the stage runs on
    * every turn, including ones with nothing to say. */
   aimee_ir_session_measure(NULL, &m);
   assert(m.tool_calls == 0 && m.redundant_tool_calls == 0 && m.intervention[0] == 0);

   /* Distinct calls are counted and none is redundant. */
   reset();
   call("read_file", "path", "a.c");
   call("read_file", "path", "b.c");
   measure(&m);
   assert(m.tool_calls == 2);
   assert(m.redundant_tool_calls == 0);

   /* Identical name AND arguments is the redundancy signal; the first
    * occurrence is not redundant, the repeat is. */
   reset();
   call("read_file", "path", "a.c");
   call("read_file", "path", "a.c");
   measure(&m);
   assert(m.tool_calls == 2);
   assert(m.redundant_tool_calls == 1);

   /* Four reads with no edit: searching without changing anything. */
   reset();
   call("read_file", "path", "a.c");
   call("read_file", "path", "b.c");
   call("search", "path", "c.c");
   call("find_symbol", "path", "d.c");
   measure(&m);
   assert(strcmp(m.intervention, "scope-search-before-change") == 0);

   /* An edit followed by three non-test calls: changed without testing. */
   reset();
   call("apply_patch", "path", "a.c");
   call("read_file", "path", "b.c");
   call("read_file", "path", "c.c");
   call("read_file", "path", "d.c");
   measure(&m);
   assert(strcmp(m.intervention, "change-without-test") == 0);

   /* An edit that IS followed by a test is not flagged -- the check must not
    * fire on the healthy shape it exists to contrast with. */
   reset();
   call("apply_patch", "path", "a.c");
   call("run_tests", "cmd", "make test");
   measure(&m);
   assert(m.intervention[0] == 0);

   reset();
   printf("aimee_ir_session: ok\n");
   return 0;
}
