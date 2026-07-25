#ifndef DEC_DELEGATE_SOURCE_AUTHORITY_H
#define DEC_DELEGATE_SOURCE_AUTHORITY_H 1

#include "aimee.h"

typedef struct delegate_source_env_snapshot_s
{
   char source_auth[16];
   char source_paths[4096];
   char worktree_root[MAX_PATH_LEN];
} delegate_source_env_snapshot_t;

void delegate_source_path_add(char paths[][MAX_PATH_LEN], int *count, int max, const char *path);
void delegate_source_authority_env_set(const char paths[][MAX_PATH_LEN], int count,
                                       const char *worktree_root);
char *delegate_append_source_authority_note(const char *base, int source_count);
void delegate_source_env_capture(delegate_source_env_snapshot_t *snap);
void delegate_source_env_restore(const delegate_source_env_snapshot_t *snap);
void delegate_source_env_clear_for_worktree(const char *worktree_root);
void delegate_check_file_drift(const char *wt_path, const char **named_files, int named_count);

#endif /* DEC_DELEGATE_SOURCE_AUTHORITY_H */
