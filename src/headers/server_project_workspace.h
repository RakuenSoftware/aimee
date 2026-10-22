#ifndef AIMEE_SERVER_PROJECT_WORKSPACE_H
#define AIMEE_SERVER_PROJECT_WORKSPACE_H
#include "request_context.h"
#include "server.h"
#include "modules/git/git_project.h"
#include <string.h>

/* Only the normal host-attested PAM webuser path can exercise the shared GUI
 * project owner. Audit principals and request bodies never grant clone custody.
 * Native callbacks inherit this immutable attestation from the actual request. */
static inline int server_project_workspace_open_owned(const request_context_t *owner,
                                                      const char *cwd)
{
   if (!owner || owner->vault_transport != ATTEST_WEBCHAT_TRUSTED ||
       !(owner->capabilities & CAP_TOOL_EXECUTE) || strncmp(owner->vault_principal, "webuser:", 8))
      return -1;
   return git_project_open_workspace(owner->vault_principal, cwd);
}
#endif
