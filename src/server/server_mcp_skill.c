#include "server_mcp_skill.h"
#include "aimee.h"
#include <aimee/skills/skill.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static cJSON *skill_text_content(const char *text)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON *item = cJSON_CreateObject();
   cJSON_AddStringToObject(item, "type", "text");
   cJSON_AddStringToObject(item, "text", text ? text : "");
   cJSON_AddItemToArray(arr, item);
   return arr;
}

static cJSON *skill_result(int ok, const char *action, const char *name, const char *err)
{
   cJSON *obj = cJSON_CreateObject();
   cJSON_AddBoolToObject(obj, "ok", ok ? 1 : 0);
   cJSON_AddStringToObject(obj, "action", action ? action : "");
   cJSON_AddStringToObject(obj, "name", name ? name : "");
   if (err && err[0])
      cJSON_AddStringToObject(obj, "error", err);
   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   cJSON *content = skill_text_content(json ? json : "{}");
   free(json);
   return content;
}

cJSON *server_mcp_tool_skill_manage(cJSON *args)
{
   cJSON *ja = cJSON_GetObjectItemCaseSensitive(args, "action");
   cJSON *jn = cJSON_GetObjectItemCaseSensitive(args, "name");
   cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(args, "cwd");
   if (!cJSON_IsString(ja) || !cJSON_IsString(jn))
      return skill_result(0, "", "", "action and name are required");

   char cwd[MAX_PATH_LEN];
   const char *root = cJSON_IsString(jcwd) && jcwd->valuestring[0] ? jcwd->valuestring : NULL;
   if (!root)
   {
      if (!getcwd(cwd, sizeof(cwd)))
         cwd[0] = '\0';
      root = cwd;
   }

   const char *action = ja->valuestring;
   const char *name = jn->valuestring;
   char err[256] = "";
   int rc = -1;
   if (strcmp(action, "create") == 0 || strcmp(action, "edit") == 0)
   {
      cJSON *jc = cJSON_GetObjectItemCaseSensitive(args, "content");
      if (!cJSON_IsString(jc))
         return skill_result(0, action, name, "content is required");
      if (skill_lint_content(name, jc->valuestring, err, sizeof(err)) != 0)
         return skill_result(0, action, name, err[0] ? err : "skill lint failed");
      if (strcmp(action, "create") == 0)
         rc = skill_manage_create(root, name, jc->valuestring, "agent", err, sizeof(err));
      else
         rc = skill_manage_edit(root, name, jc->valuestring, "agent", err, sizeof(err));
   }
   else if (strcmp(action, "patch") == 0)
   {
      cJSON *jo = cJSON_GetObjectItemCaseSensitive(args, "old_string");
      cJSON *jnw = cJSON_GetObjectItemCaseSensitive(args, "new_string");
      cJSON *jall = cJSON_GetObjectItemCaseSensitive(args, "replace_all");
      if (!cJSON_IsString(jo) || !cJSON_IsString(jnw))
         return skill_result(0, action, name, "old_string and new_string are required");
      rc = skill_manage_patch(root, name, jo->valuestring, jnw->valuestring, cJSON_IsTrue(jall),
                              "agent", err, sizeof(err));
   }
   else if (strcmp(action, "archive") == 0)
   {
      cJSON *ji = cJSON_GetObjectItemCaseSensitive(args, "absorbed_into");
      rc = skill_manage_archive(root, name, cJSON_IsString(ji) ? ji->valuestring : NULL, err,
                                sizeof(err));
   }
   else if (strcmp(action, "write_file") == 0)
   {
      cJSON *jp = cJSON_GetObjectItemCaseSensitive(args, "file_path");
      cJSON *jc = cJSON_GetObjectItemCaseSensitive(args, "content");
      if (!cJSON_IsString(jp) || !cJSON_IsString(jc))
         return skill_result(0, action, name, "file_path and content are required");
      rc = skill_manage_write_file(root, name, jp->valuestring, jc->valuestring, "agent", err,
                                   sizeof(err));
   }
   else
      return skill_result(0, action, name, "unknown action");
   return skill_result(rc == 0, action, name, rc == 0 ? NULL : err);
}
