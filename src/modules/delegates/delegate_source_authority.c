#include <aimee/delegates/delegate_source_authority.h>
#include "commands.h"
#include "log.h"
#include "platform_process.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void delegate_source_env_capture(delegate_source_env_snapshot_t *snap)
{
   if (!snap)
      return;
   memset(snap, 0, sizeof(*snap));
   const char *source_auth = getenv("AIMEE_DELEGATE_SOURCE_AUTHORITY");
   const char *source_paths = getenv("AIMEE_DELEGATE_SOURCE_PATHS");
   const char *worktree_root = getenv("AIMEE_DELEGATE_WORKTREE_ROOT");
   if (source_auth && source_auth[0])
      snprintf(snap->source_auth, sizeof(snap->source_auth), "%s", source_auth);
   if (source_paths && source_paths[0])
      snprintf(snap->source_paths, sizeof(snap->source_paths), "%s", source_paths);
   if (worktree_root && worktree_root[0])
      snprintf(snap->worktree_root, sizeof(snap->worktree_root), "%s", worktree_root);
}

void delegate_source_env_restore(const delegate_source_env_snapshot_t *snap)
{
   if (!snap)
      return;
   platform_setenv("AIMEE_DELEGATE_SOURCE_AUTHORITY", snap->source_auth);
   platform_setenv("AIMEE_DELEGATE_SOURCE_PATHS", snap->source_paths);
   platform_setenv("AIMEE_DELEGATE_WORKTREE_ROOT", snap->worktree_root);
}

void delegate_source_env_clear_for_worktree(const char *worktree_root)
{
   platform_setenv("AIMEE_DELEGATE_WORKTREE_ROOT", worktree_root ? worktree_root : "");
   platform_setenv("AIMEE_DELEGATE_SOURCE_AUTHORITY", "");
   platform_setenv("AIMEE_DELEGATE_SOURCE_PATHS", "");
}
