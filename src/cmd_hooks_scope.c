/* cmd_hooks_scope.c: scope-aware section builder shared by cmd_hooks and
 * cmd_session_start. See headers/cmd_hooks_scope.h for rationale. */
#include "aimee.h"
#include "headers/cmd_hooks_scope.h"
#include "config_accessors.h"
#include "workspace.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int hook_session_id_safe(const char *sid)
{
   if (!sid || !sid[0])
      return 0;
   for (const unsigned char *p = (const unsigned char *)sid; *p; p++)
      if (!isalnum(*p) && *p != '-' && *p != '_')
         return 0;
   return 1;
}

int hook_payload_session_id(const cJSON *hook_json, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return 0;
   out[0] = '\0';

   const char *sid = NULL;
   const cJSON *jsid = hook_json ? cJSON_GetObjectItemCaseSensitive(hook_json, "session_id") : NULL;
   if (cJSON_IsString(jsid) && jsid->valuestring[0])
      sid = jsid->valuestring;

   static const char *env_keys[] = {"AIMEE_SESSION_ID", "CLAUDE_SESSION_ID", "CODEX_THREAD_ID",
                                    NULL};
   for (int i = 0; (!sid || !sid[0]) && env_keys[i]; i++)
      sid = getenv(env_keys[i]);

   if (!hook_session_id_safe(sid))
      return 0;

   snprintf(out, out_len, "%s", sid);
   return 1;
}

static int hook_payload_copy_cwd_from_object(const cJSON *obj, char *out, size_t out_len)
{
   if (!cJSON_IsObject(obj) || !out || out_len == 0)
      return 0;

   static const char *cwd_keys[] = {"cwd", "workdir", "working_dir", "working_directory", NULL};
   for (int i = 0; cwd_keys[i]; i++)
   {
      cJSON *jcwd = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, cwd_keys[i]);
      if (cJSON_IsString(jcwd) && jcwd->valuestring[0])
      {
         snprintf(out, out_len, "%s", jcwd->valuestring);
         return 1;
      }
   }
   return 0;
}

int hook_payload_cwd(const cJSON *hook_json, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return 0;
   out[0] = '\0';
   if (!hook_json)
      return 0;

   if (hook_payload_copy_cwd_from_object(hook_json, out, out_len))
      return 1;

   cJSON *tool_input = cJSON_GetObjectItemCaseSensitive((cJSON *)hook_json, "tool_input");
   if (hook_payload_copy_cwd_from_object(tool_input, out, out_len))
      return 1;

   if (cJSON_IsString(tool_input) && tool_input->valuestring[0])
   {
      cJSON *parsed = cJSON_Parse(tool_input->valuestring);
      int found = hook_payload_copy_cwd_from_object(parsed, out, out_len);
      cJSON_Delete(parsed);
      return found;
   }
   return 0;
}

void hook_scope_labels_for_cwd(const char *cwd, char *workspace_out, size_t workspace_len,
                               char *project_out, size_t project_len)
{
   if (workspace_out && workspace_len > 0)
      workspace_out[0] = '\0';
   if (project_out && project_len > 0)
      project_out[0] = '\0';
   if (!cwd || !cwd[0])
      return;

   /* Path-independent repository identity, kept consistent with the recall side
    * (memory_scope_labels_for_cwd) so scope tags written here match on lookup
    * across clones/machines. Falls through to the legacy workspace/basename
    * labels below when cwd is not inside a resolvable git repo. */
   if (workspace_repo_identity(cwd, project_out, project_len, workspace_out, workspace_len) == 0)
      return;

   char configured_root[MAX_PATH_LEN] = {0};
   size_t configured_root_len = 0;
   int workspace_count = config_workspace_count();
   for (int i = 0; i < workspace_count; i++)
   {
      const char *configured_workspace = config_workspaces(i);
      size_t wlen = strlen(configured_workspace);
      if (wlen <= configured_root_len)
         continue;
      if (strncmp(cwd, configured_workspace, wlen) == 0 && (cwd[wlen] == '/' || cwd[wlen] == '\0'))
      {
         snprintf(configured_root, sizeof(configured_root), "%s", configured_workspace);
         configured_root_len = wlen;
      }
   }

   if (workspace_out && workspace_len > 0 && configured_root[0])
   {
      const char *slash = strrchr(configured_root, '/');
      snprintf(workspace_out, workspace_len, "%s", slash ? slash + 1 : configured_root);
   }

   if (project_out && project_len > 0)
   {
      char active_root[MAX_PATH_LEN] = {0};
      /* A configured workspace is a visibility parent, not necessarily the
       * active project. Resolve the project independently from cwd so nested
       * repositories and non-repository working directories cannot inherit
       * the configured workspace directory's name as their project label. */
      if (workspace_active_root(NULL, cwd, active_root, sizeof(active_root)) == 0 && active_root[0])
      {
         const char *slash = strrchr(active_root, '/');
         const char *name = slash ? slash + 1 : active_root;
         snprintf(project_out, project_len, "%s", name ? name : "");
      }
   }
}
