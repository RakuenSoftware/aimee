#ifndef GIT_VERIFY_SELECT_H
#define GIT_VERIFY_SELECT_H 1

#include "git_verify_internal.h"

int verify_path_match(const char *pattern, const char *path);
int verify_path_list_matches(const char *patterns, const char *path);
void verify_incremental_apply(const char *project_root, verify_config_t *cfg,
                              verify_thread_ctx_t *contexts, int *step_state, int *remaining);

#endif /* GIT_VERIFY_SELECT_H */
