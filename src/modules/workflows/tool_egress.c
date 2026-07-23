/* tool_egress.c: see tool_egress.h. Pure declaration table -- no engine/DB deps. */
#include "tool_egress.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

typedef struct
{
   const char *name;
   tool_egress_t cls;
   /* NULL for a canonical entry. For an alias, the canonical tool it spells
    * differently -- recorded so the startup invariant can verify the alias
    * actually resolves to a real tool with the same class, rather than trusting
    * a bare "this is an alias" flag. */
   const char *canonical;
} egress_entry_t;

/* Every built-in tool in server/agent_tools.c g_builtin_tools[] appears here
 * exactly once, plus a small number of explicit alias spellings. The startup
 * invariant (agent_tools_validate_egress_table) fails if the two sets diverge
 * in either direction, so this table cannot silently drift from the tool table.
 *
 * Classification rationale for the non-obvious entries is inline. */
static const egress_entry_t TOOLS[] = {
    /* --- command-dependent: gated by shell command inspection, not by name --- */
    {"bash", TOOL_EGRESS_COMMAND, NULL},
    {"execute_script", TOOL_EGRESS_COMMAND, NULL},
    /* verify/test/run_tests execute project-supplied command lines, so their
     * reach is whatever the project's test command does. */
    {"verify", TOOL_EGRESS_COMMAND, NULL},
    {"test", TOOL_EGRESS_COMMAND, NULL},
    {"run_tests", TOOL_EGRESS_COMMAND, NULL},
    {"run_background_process", TOOL_EGRESS_COMMAND, NULL},

    /* --- external: data can leave the trust boundary --- */
    /* Fetches an arbitrary third-party URL. The URL is itself an outbound
     * channel, independent of any request body. */
    {"web_read", TOOL_EGRESS_EXTERNAL, NULL},
    {"webread", TOOL_EGRESS_EXTERNAL, "web_read"},
    /* Sends the query text to a third-party search engine. */
    {"web_search", TOOL_EGRESS_EXTERNAL, NULL},
    {"websearch", TOOL_EGRESS_EXTERNAL, "web_search"},
    /* Publishes commits / opens a pull request on the forge. */
    {"git_push", TOOL_EGRESS_EXTERNAL, NULL},
    {"git_pr", TOOL_EGRESS_EXTERNAL, NULL},

    /* --- none: local, or traffic to a trusted internal service --- */
    {"read_file", TOOL_EGRESS_NONE, NULL},
    {"write_file", TOOL_EGRESS_NONE, NULL},
    {"edit_file", TOOL_EGRESS_NONE, NULL},
    {"list_files", TOOL_EGRESS_NONE, NULL},
    {"grep", TOOL_EGRESS_NONE, NULL},
    {"tool_output_get", TOOL_EGRESS_NONE, NULL},
    {"env_get", TOOL_EGRESS_NONE, NULL},
    /* Local repository reads: no remote contact. */
    {"git_log", TOOL_EGRESS_NONE, NULL},
    {"git_diff", TOOL_EGRESS_NONE, NULL},
    {"git_status", TOOL_EGRESS_NONE, NULL},
    {"git_branch", TOOL_EGRESS_NONE, NULL},
    /* Writes locally; publication is git_push/git_pr, classified above. */
    {"git_commit", TOOL_EGRESS_NONE, NULL},
    /* Local index / notes / symbol store. */
    {"code_search", TOOL_EGRESS_NONE, NULL},
    {"find_symbol", TOOL_EGRESS_NONE, NULL},
    {"read_symbol", TOOL_EGRESS_NONE, NULL},
    {"edit_symbol", TOOL_EGRESS_NONE, NULL},
    {"create_note", TOOL_EGRESS_NONE, NULL},
    {"list_notes", TOOL_EGRESS_NONE, NULL},
    {"search_notes", TOOL_EGRESS_NONE, NULL},
    /* search_docs and search_memory reach the knowledge-base service, which may
     * be a separate process or host, but is a TRUSTED INTERNAL component -- not
     * a destination outside the boundary. Reaching a socket is not the test;
     * leaving the boundary is. */
    {"search_docs", TOOL_EGRESS_NONE, NULL},
    {"search_memory", TOOL_EGRESS_NONE, NULL},
    /* Read/stop/enumerate already-running work; start a process is
     * run_background_process, classified COMMAND above. */
    {"get_background_output", TOOL_EGRESS_NONE, NULL},
    {"kill_background_process", TOOL_EGRESS_NONE, NULL},
    {"list_background_processes", TOOL_EGRESS_NONE, NULL},
};

#define TOOLS_N ((int)(sizeof(TOOLS) / sizeof(TOOLS[0])))

static int eq_ci(const char *a, const char *b)
{
   if (!a || !b)
      return 0;
   for (; *a && *b; a++, b++)
      if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
         return 0;
   return *a == *b;
}

tool_egress_t tool_egress_for(const char *tool_name)
{
   if (!tool_name || !tool_name[0])
      return TOOL_EGRESS_UNSET;
   for (int i = 0; i < TOOLS_N; i++)
      if (eq_ci(tool_name, TOOLS[i].name))
         return TOOLS[i].cls;
   return TOOL_EGRESS_UNSET;
}

int tool_egress_is_external(const char *tool_name)
{
   return tool_egress_for(tool_name) == TOOL_EGRESS_EXTERNAL ? 1 : 0;
}

int tool_egress_count(void)
{
   return TOOLS_N;
}

const char *tool_egress_name_at(int index)
{
   if (index < 0 || index >= TOOLS_N)
      return NULL;
   return TOOLS[index].name;
}

tool_egress_t tool_egress_class_at(int index)
{
   if (index < 0 || index >= TOOLS_N)
      return TOOL_EGRESS_UNSET;
   return TOOLS[index].cls;
}

int tool_egress_is_alias_at(int index)
{
   if (index < 0 || index >= TOOLS_N)
      return 0;
   return TOOLS[index].canonical != NULL;
}

const char *tool_egress_canonical_at(int index)
{
   if (index < 0 || index >= TOOLS_N)
      return NULL;
   return TOOLS[index].canonical;
}

int tool_egress_names_equal(const char *a, const char *b)
{
   return eq_ci(a, b);
}

const char *tool_egress_class_name(tool_egress_t cls)
{
   switch (cls)
   {
   case TOOL_EGRESS_NONE:
      return "none";
   case TOOL_EGRESS_EXTERNAL:
      return "external";
   case TOOL_EGRESS_COMMAND:
      return "command";
   case TOOL_EGRESS_UNSET:
   default:
      return "unset";
   }
}
