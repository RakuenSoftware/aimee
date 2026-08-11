/* mcp_group_tool.c: one MCP tool per command GROUP, multiplexed by `command`.
 *
 * `aimee memory get` on the CLI and `memory` with command=get over MCP are the
 * same command, spelled the same way. The flat spellings -- memory_get,
 * search_memory, memory_recall -- were three different conventions for one group,
 * and a mechanical CLI->MCP mapping could not even tell "absent" from "spelled
 * backwards" (memory.recall is memory_recall; memory.search is search_memory,
 * verb first). One name, one shape.
 *
 * This follows the house pattern rather than inventing one: `git` and `index` are
 * already single tools multiplexed by `command`, and mcp_tool_profile.c records
 * why that matters -- a tool reachable only via find_tools -> describe_tool ->
 * call_tool is one agents will not pay for, so a group with fifteen verbs must
 * cost ONE entry in tools/list, not fifteen.
 *
 * The tool is BUILT FROM THE REGISTRY. The command enum is whatever the owning
 * module registered with an MCP surface, so a verb cannot appear in tools/list
 * without being registered, and cannot be registered for MCP and go missing from
 * the surface. That equivalence is the entire point of the table.
 */
#include "aimee/protocols/mcp/mcp_group_tool.h"
#include "command_registry.h"
#include "aimee/protocols/mcp/mcp_tools.h"
#include "cJSON.h"
#include "dstr.h"
#include <string.h>

cJSON *mcp_group_tool_build(const char *group, const char *summary)
{
   if (!group || !group[0])
      return NULL;

   cJSON *enum_arr = cJSON_CreateArray();
   dstr_t verbs;
   dstr_init(&verbs);
   int n = 0;
   for (size_t i = 0; i < aimee_command_count(); i++)
   {
      const aimee_command_t *c = aimee_command_at(i);
      if (!c || strcmp(c->group, group) != 0 || !(c->surfaces & AIMEE_SURFACE_MCP))
         continue;
      cJSON_AddItemToArray(enum_arr, cJSON_CreateString(c->verb));
      dstr_appendf(&verbs, "%s%s", n ? ", " : "", c->verb);
      n++;
   }
   if (n == 0)
   {
      /* No verb of this group is on the MCP surface. Emitting a tool whose
       * `command` accepts nothing would be worse than emitting none: the agent
       * would spend a call discovering it can do nothing. */
      cJSON_Delete(enum_arr);
      dstr_free(&verbs);
      return NULL;
   }

   cJSON *s = cJSON_CreateObject();
   cJSON_AddStringToObject(s, "type", "object");
   cJSON *p = cJSON_AddObjectToObject(s, "properties");
   cJSON *cmd = cJSON_AddObjectToObject(p, "command");
   cJSON_AddStringToObject(cmd, "type", "string");
   cJSON_AddItemToObject(cmd, "enum", enum_arr);

   dstr_t desc;
   dstr_init(&desc);
   dstr_appendf(&desc, "%s Set 'command' to one of: %s. Remaining parameters apply per command.",
                summary ? summary : "", verbs.data ? verbs.data : "");
   dstr_free(&verbs);

   cJSON_AddStringToObject(cmd, "description", "The operation to perform.");
   cJSON *req = cJSON_AddArrayToObject(s, "required");
   cJSON_AddItemToArray(req, cJSON_CreateString("command"));

   cJSON *tool = mcp_tool_new(group, desc.data ? desc.data : group, s);
   dstr_free(&desc);
   return tool;
}
