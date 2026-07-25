/* agent_tools_completion.c: storage + emit for the tool-call COMPLETION audit
 * hook (see agent_tools.h). Deliberately dependency-free — it holds only a
 * function pointer — so a thin client, a test, or the dispatcher can link the
 * hook mechanism without pulling the whole tool dispatcher, and so this TU has
 * no bus dependency (the server-only bridge is the sole bus edge, D7).
 *
 * PROCESS-GLOBAL and NULL-by-default, like the vault/sandbox audit hooks: the
 * server installs one bridge at startup and it fires on every dispatch thread. */
#include "config.h" /* MAX_PATH_LEN, transitively needed by agent_types.h (header-only) */
#include "agent_tools.h"

static agent_tool_completion_cb_t g_completion_cb = NULL;
static void *g_completion_ud = NULL;

void agent_tools_set_tool_completion_cb(agent_tool_completion_cb_t cb, void *ud)
{
   g_completion_cb = cb;
   g_completion_ud = ud;
}

void agent_tools_emit_tool_completion(const char *tool, const agent_tool_completion_t *outcome)
{
   if (g_completion_cb && tool && outcome)
      g_completion_cb(tool, outcome, g_completion_ud);
}

void agent_tools_fire_tool_completion_for_test(const char *tool,
                                               const agent_tool_completion_t *outcome)
{
   agent_tools_emit_tool_completion(tool, outcome);
}
