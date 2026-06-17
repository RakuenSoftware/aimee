#ifndef WORKSPACE_SCOPE_H
#define WORKSPACE_SCOPE_H 1

#include <stddef.h>

/* workspace_scope — per-webuser workspace isolation (webchat-git §0 / WP-A).
 *
 * Every webchat user's projects live under
 *   ${AIMEE_WORKSPACES_DIR or <aimee_home>/workspaces}/webusers/<name>/
 * where <name> is the username from a `webuser:<name>` vault principal (the
 * attested identity resolved by vault_principal_resolve / conn->vault_principal).
 *
 * This module is the ONE place that maps a (principal, project) to an absolute
 * path, and it is hardened so one webuser can never reach another's tree:
 *  - the principal name is allowlist-validated (no '/', '..', control chars);
 *  - a project is a single path component (no nesting, no traversal);
 *  - the resolved path is canonicalized (realpath) and re-checked to lie within
 *    the principal root, so a symlink that escapes the root is rejected.
 *
 * Consumers that then open files/dirs under the returned root MUST still open
 * relative to a base-dir fd with O_NOFOLLOW / openat2(RESOLVE_BENEATH) to close
 * the resolve-vs-use TOCTOU window (see ws_scope_open_user_root). */

/* Validate a `webuser:<name>` principal and resolve its root dir into out[cap]
 * (".../webusers/<name>"). If create != 0, mkdir the chain 0700. Returns 0 on
 * success; -1 on a non-webuser / malformed principal or filesystem error. */
int ws_scope_user_root(const char *principal, int create, char *out, size_t cap);

/* Resolve a single-component `project` name under `principal`'s root into
 * out[cap], canonically and within the root. `must_exist`: 1 => the project dir
 * must already exist (realpath'd; symlink-escape rejected); 0 => return
 * <root>/<project> for a not-yet-created clone target (root canonicalized;
 * rejects if the target already exists as a symlink). Returns 0; -1 on a bad
 * project name, an escape attempt, or error. */
int ws_scope_project_path(const char *principal, const char *project, int must_exist, char *out,
                          size_t cap);

/* 1 iff `abs_path` canonically lies within `principal`'s root (realpath of
 * both; a trailing-'/' boundary so /a/bc is not "within" /a/b). 0 otherwise or
 * on error. Use to guard a caller-supplied absolute root. */
int ws_scope_contains(const char *principal, const char *abs_path);

/* Open `principal`'s root dir with O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC and return
 * the fd (caller closes), or -1. Consumers should openat() project paths from
 * this fd to avoid TOCTOU. (create=1 ensures the root exists first.) */
int ws_scope_open_user_root(const char *principal);

/* 1 iff `name` is a valid single path component for a principal or project
 * (allowlist [A-Za-z0-9][A-Za-z0-9._-]*, len<=64, not "."/".."), else 0.
 * Exposed for callers validating user input before resolution. Pure. */
int ws_scope_name_valid(const char *name);

#endif /* WORKSPACE_SCOPE_H */
