/* workspace_handle.c — pure builder for the /v1 workspace handle manifest.
 * See workspace_handle.h. Kept free of server/IO deps so it can be unit-tested
 * standalone; the server handler (handle_workspace_get) gathers the stat + VCS
 * inputs and calls this to shape the response. */
#include "workspace_handle.h"
#include "cJSON.h"

cJSON *workspace_manifest_json(const char *root, const char *provider, int exists, int is_dir,
                               const char *vcs_remote, const char *vcs_head, const char *vcs_branch)
{
   cJSON *m = cJSON_CreateObject();
   if (!m)
      return NULL;

   cJSON_AddStringToObject(m, "root", root ? root : "");
   cJSON_AddStringToObject(m, "provider", provider ? provider : "shared");
   cJSON_AddBoolToObject(m, "exists", exists ? 1 : 0);
   cJSON_AddBoolToObject(m, "is_dir", is_dir ? 1 : 0);

   cJSON *vcs = cJSON_AddObjectToObject(m, "vcs");
   cJSON_AddStringToObject(vcs, "remote", vcs_remote ? vcs_remote : "");
   cJSON_AddStringToObject(vcs, "head", vcs_head ? vcs_head : "");
   cJSON_AddStringToObject(vcs, "branch", vcs_branch ? vcs_branch : "");

   return m;
}
