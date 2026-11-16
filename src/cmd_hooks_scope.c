/* cmd_hooks_scope.c: scope-aware section builder shared by cmd_hooks and
 * cmd_session_start. See headers/cmd_hooks_scope.h for rationale. */
#include "aimee.h"
#include "headers/cmd_hooks_scope.h"
#include "config_accessors.h"
#include <aimee/workspace/workspace.h>
#include "cJSON.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void hook_scope_labels_for_cwd(const char *cwd, char *workspace_out, size_t workspace_len,
                               char *project_out, size_t project_len)
{
   if (workspace_out && workspace_len > 0)
      workspace_out[0] = '\0';
   if (project_out && project_len > 0)
      project_out[0] = '\0';
   if (!cwd || !cwd[0])
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

   /* Path-independent project identity, kept consistent with the recall side
    * (memory_scope_labels_for_cwd) so scope tags written here match on lookup
    * across clones/machines. A configured workspace remains the visibility
    * parent; it must not be replaced by the repository identity. */
   char repo_project[MAX_PATH_LEN];
   char repo_workspace[MAX_PATH_LEN];
   if (workspace_repo_identity(cwd, repo_project, sizeof(repo_project), repo_workspace,
                               sizeof(repo_workspace)) == 0)
   {
      if (workspace_out && workspace_len > 0)
      {
         if (configured_root[0])
         {
            const char *slash = strrchr(configured_root, '/');
            snprintf(workspace_out, workspace_len, "%s", slash ? slash + 1 : configured_root);
         }
         else
            snprintf(workspace_out, workspace_len, "%s", repo_workspace);
      }
      if (project_out && project_len > 0)
         snprintf(project_out, project_len, "%s", repo_project);
      return;
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
      if (workspace_active_root_from_cwd(cwd, active_root, sizeof(active_root)) == 0 &&
          active_root[0])
      {
         const char *slash = strrchr(active_root, '/');
         const char *name = slash ? slash + 1 : active_root;
         snprintf(project_out, project_len, "%s", name ? name : "");
      }
   }
}
