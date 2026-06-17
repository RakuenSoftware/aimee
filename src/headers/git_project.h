#ifndef GIT_PROJECT_H
#define GIT_PROJECT_H 1

#include <stddef.h>

/* git_project — clone a remote repo as a project under a webchat user's scoped
 * workspace (webchat-git WP-D). The destination is resolved + validated by
 * workspace_scope (no cross-tenant escape), the user's vaulted git credentials
 * are injected (WP-C) into the git child env only, and `git clone` runs without
 * a shell (argv + envp), so a hostile URL cannot inject a command or a flag.
 * Indexing the cloned project is the caller's concern (index_scan_project). */

/* Clone `url` as project `name` (derived from the URL basename, minus a trailing
 * ".git", when `name` is NULL/empty) under `principal` (a `webuser:<name>`).
 * On success writes the absolute project path to out_path[path_cap] and the
 * resolved project name to out_name[name_cap], and returns 0. On failure returns
 * -1 with a short, non-sensitive message in err[errlen]. Refuses: a non-webuser
 * principal, a name that escapes the scope, an already-existing project, or an
 * empty/flag-like URL. */
int git_project_clone(const char *principal, const char *url, const char *name, char *out_path,
                      size_t path_cap, char *out_name, size_t name_cap, char *err, size_t errlen);

/* List `principal`'s project names (the subdirectories of their scoped
 * workspace) into out[max][GIT_PROJECT_NAME_MAX], sorted is not guaranteed.
 * Returns the count (>=0), or -1 on a bad principal. A missing scope root is 0
 * projects, not an error. */
#define GIT_PROJECT_NAME_MAX 128
int git_project_list(const char *principal, char out[][GIT_PROJECT_NAME_MAX], int max);

#endif /* GIT_PROJECT_H */
