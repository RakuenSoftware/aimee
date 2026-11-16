#ifndef DEC_DELEGATE_SOURCE_AUTHORITY_H
#define DEC_DELEGATE_SOURCE_AUTHORITY_H 1

#include "aimee.h"

typedef struct delegate_source_env_snapshot_s
{
   char source_auth[16];
   char source_paths[4096];
   char worktree_root[MAX_PATH_LEN];
} delegate_source_env_snapshot_t;

void delegate_source_env_capture(delegate_source_env_snapshot_t *snap);
void delegate_source_env_restore(const delegate_source_env_snapshot_t *snap);
void delegate_source_env_clear_for_worktree(const char *worktree_root);

#endif /* DEC_DELEGATE_SOURCE_AUTHORITY_H */
