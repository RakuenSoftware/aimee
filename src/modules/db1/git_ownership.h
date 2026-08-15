/* db1/git_ownership.h: user-local branch ownership records for MCP git flows.
 *
 * This subsystem owns branch/session ownership rows used to keep concurrent
 * local sessions from stomping on each other's branches. Backend access stays
 * private to src/db1/. */
#ifndef DEC_DB1_GIT_OWNERSHIP_H
#define DEC_DB1_GIT_OWNERSHIP_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   int db1_git_ownership_upsert(const char *repo_path, const char *branch_name,
                                const char *session_id);
   int db1_git_ownership_delete(const char *repo_path, const char *branch_name);
   int db1_git_ownership_get_owner(const char *repo_path, const char *branch_name, char *owner_out,
                                   size_t owner_len);
   int db1_git_ownership_get_branch_for_session(const char *repo_path, const char *session_id,
                                                char *branch_out, size_t branch_len);
   int db1_git_ownership_find_session_by_prefix(const char *session_prefix, char *session_out,
                                                size_t session_len);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_GIT_OWNERSHIP_H */
