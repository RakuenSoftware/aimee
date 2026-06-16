/* Stub of the source-authority TLS setters for tests that link server_compute.c
 * (delegate_run_ctx_enter/restore call these) but do not exercise the
 * code_search overlay. The real implementation lives in
 * posix/agent_source_authority.c, which pulls in kb_client/agent_tools. */
#include "agent_source_authority.h"
#include <stdlib.h>

void agent_source_authority_tls_set(int authority, const char *worktree_root, const char *paths)
{
   (void)authority;
   (void)worktree_root;
   (void)paths;
}

void agent_source_authority_tls_capture(agent_source_authority_snapshot_t *snap)
{
   if (snap)
   {
      snap->active = 0;
      snap->authority = 0;
      snap->root[0] = '\0';
      snap->paths = NULL;
   }
}

void agent_source_authority_tls_restore(agent_source_authority_snapshot_t *snap)
{
   if (snap)
   {
      free(snap->paths);
      snap->paths = NULL;
   }
}
