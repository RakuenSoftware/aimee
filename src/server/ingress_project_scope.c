/* Resolve the active agent workspace into the canonical project identity used
 * by both code and memory. Kept out of ingress_preinject.c so its unit tests can
 * provide a deterministic seam without linking the full workspace subsystem. */
#include "ingress_preinject.h"
#include "util.h"
#include <aimee/workspace/workspace.h>

#include <limits.h>
#include <string.h>
#include <unistd.h>

int ingress_preinject_resolve_active_scope(char *workspace, size_t workspace_len, char *project,
                                           size_t project_len)
{
   if (!workspace || workspace_len == 0 || !project || project_len == 0)
      return -1;
   workspace[0] = '\0';
   project[0] = '\0';

   char cwd_buf[PATH_MAX];
   const char *cwd = run_cmd_get_cwd();
   if ((!cwd || !cwd[0]) && getcwd(cwd_buf, sizeof(cwd_buf)))
      cwd = cwd_buf;
   if (!cwd || !cwd[0] ||
       workspace_repo_identity(cwd, project, project_len, workspace, workspace_len) != 0 ||
       !project[0])
   {
      workspace[0] = '\0';
      project[0] = '\0';
      return -1;
   }
   if (!workspace[0])
   {
      strncpy(workspace, project, workspace_len - 1);
      workspace[workspace_len - 1] = '\0';
   }
   return 0;
}
