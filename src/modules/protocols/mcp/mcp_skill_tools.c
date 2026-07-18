#include "mcp_skill_tools.h"

static cJSON *skill_tool(const char *name, const char *desc, cJSON *schema)
{
   cJSON *t = cJSON_CreateObject();
   cJSON_AddStringToObject(t, "name", name);
   cJSON_AddStringToObject(t, "description", desc);
   cJSON_AddItemToObject(t, "inputSchema", schema);
   return t;
}

void mcp_add_skill_tools(cJSON *tools)
{
   cJSON *s = cJSON_CreateObject();
   cJSON_AddStringToObject(s, "type", "object");
   cJSON *p = cJSON_AddObjectToObject(s, "properties");
   cJSON_AddStringToObject(cJSON_AddObjectToObject(p, "action"), "type", "string");
   cJSON_AddStringToObject(cJSON_AddObjectToObject(p, "name"), "type", "string");
   cJSON_AddStringToObject(cJSON_AddObjectToObject(p, "cwd"), "type", "string");
   cJSON_AddStringToObject(cJSON_AddObjectToObject(p, "content"), "type", "string");
   cJSON_AddStringToObject(cJSON_AddObjectToObject(p, "old_string"), "type", "string");
   cJSON_AddStringToObject(cJSON_AddObjectToObject(p, "new_string"), "type", "string");
   cJSON_AddStringToObject(cJSON_AddObjectToObject(p, "replace_all"), "type", "boolean");
   cJSON_AddStringToObject(cJSON_AddObjectToObject(p, "absorbed_into"), "type", "string");
   cJSON_AddStringToObject(cJSON_AddObjectToObject(p, "file_path"), "type", "string");
   cJSON *req = cJSON_CreateArray();
   cJSON_AddItemToArray(req, cJSON_CreateString("action"));
   cJSON_AddItemToArray(req, cJSON_CreateString("name"));
   cJSON_AddItemToObject(s, "required", req);
   cJSON_AddItemToArray(
       tools, skill_tool("skill_manage",
                         "Create, patch, edit, archive, or write support files for project "
                         "skills. Skills are never deleted; archive moves them aside and pinned "
                         "skills are protected.",
                         s));
}
