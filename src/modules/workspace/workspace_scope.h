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

/* Resolve the webusers base dir (parent of every per-user root) into out[cap].
 * For the registry/lock module, which keeps its state beside the user trees. */
int ws_scope_webusers_base(char *out, size_t cap);

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

/* TOCTOU-safe project open: validate `project` (single component), open the
 * principal root (O_NOFOLLOW base fd), then openat(base, project,
 * O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC | extra_flags). Because the base fd is pinned
 * to the canonical root and O_NOFOLLOW rejects a symlink AT the project leaf,
 * there is no resolve-vs-use window. Returns the project dir fd (caller closes)
 * or -1. This is the PREFERRED consumer API; the string-returning
 * ws_scope_project_path() is for display/logging or pairing with this fd. */
int ws_scope_open_project(const char *principal, const char *project, int extra_flags);

/* 1 iff `name` is a valid single path component for a principal or project
 * (allowlist [A-Za-z0-9][A-Za-z0-9._-]*, len<=64, not "."/".."), else 0.
 * Exposed for callers validating user input before resolution. Pure. */
int ws_scope_name_valid(const char *name);

/* Max bytes of a project ref: two 64-byte components + '/'. */
#define WS_REF_MAX 129
/* Component cap shared with ws_scope_name_valid (kept here for ref buffers). */
#define WS_REF_COMP_MAX 64

/* 1 iff buf[0..len) is a valid project ref: a flat single component, or
 * exactly <org>/<repo> where BOTH components independently pass
 * ws_scope_name_valid. This is the ONLY function that ever accepts a '/' in a
 * project reference; embedded NUL is rejected by byte-scan (the buffer is not
 * assumed NUL-terminated). Pure. */
int ws_scope_project_ref_valid(const char *buf, size_t len);

/* openat2(dirfd, name, RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS, O_DIRECTORY|
 * O_NOFOLLOW|O_CLOEXEC). Returns the fd or -1. The webuser project surface is
 * gated on ws_scope_openat2_available() and FAILS CLOSED where openat2 (Linux
 * >= 5.6) is unavailable — no string-path fallback. */
int ws_scope_openat2_dir(int dirfd, const char *name);

/* 1 iff openat2 with the resolve flags above works here (probed once). */
int ws_scope_openat2_available(void);

/* Split a NUL-terminated ref into org (empty for a flat ref) + repo,
 * validating via ws_scope_project_ref_valid. Returns the component count
 * (1 or 2) or -1 on an invalid ref / short buffer. Buffers should be
 * WS_REF_COMP_MAX+1 bytes. */
int ws_scope_ref_split(const char *ref, char *org, size_t org_cap, char *repo, size_t repo_cap);

#endif /* WORKSPACE_SCOPE_H */
