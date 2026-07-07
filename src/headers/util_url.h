/* util_url.h: canonical URL handling for project/workspace identity.
 *
 * Implements the URL normalization pipeline from the DB1/DB2 storage split
 * (docs/STORAGE_TIERS.md). The goal
 * is that two clones of the same repo on different machines — via different
 * transports, with different casing, and with or without a trailing .git —
 * resolve to the same canonical string.
 */
#ifndef DEC_UTIL_URL_H
#define DEC_UTIL_URL_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Normalize a git remote URL to a canonical form.
    *
    * Pipeline:
    *   1. Transport rewrite:
    *        git@host:org/repo          → https://host/org/repo
    *        ssh://user@host/org/repo   → https://host/org/repo
    *        http(s)://host/org/repo    → https://host/org/repo
    *        git://host/org/repo        → https://host/org/repo
    *   2. Authority: user info dropped; port dropped; host lowercased.
    *   3. Path: strip trailing .git, strip trailing '/', collapse '//' runs.
    *   4. Case: scheme and host always lowercased. Path lowercased only for
    *      hosts whose routing is case-insensitive (util_url_host_is_case_insensitive).
    *
    * Returns a newly allocated NUL-terminated string on success; caller frees
    * with free(). Returns NULL for empty input, unrecognized transports, or
    * malformed URLs (missing host or path).
    */
   char *util_url_normalize(const char *url);

   /* Return the workspace URL that is the immediate parent of `project_url`.
    *
    * For `https://host/a/b/c` returns a newly allocated `https://host/a/b`.
    * Callers that want to walk subgroup nesting (e.g. GitLab) call this
    * repeatedly until it returns NULL.
    *
    * Returns NULL when:
    *   - `project_url` is NULL.
    *   - `project_url` is a `local:<uuid>` scope (no workspace hierarchy).
    *   - `project_url` is already at its host root (no further parent).
    *   - `project_url` does not start with `https://`.
    *
    * Caller frees with free().
    */
   char *util_url_workspace_parent(const char *project_url);

   /* Report whether a host uses case-insensitive routing for path segments.
    *
    * Known-insensitive: github.com, gitlab.com, bitbucket.org, and their
    * well-known subdomains (e.g. gist.github.com, api.github.com). All other
    * hosts are treated as case-sensitive.
    *
    * `host` must already be lowercased. Returns 1 for case-insensitive, 0
    * otherwise. Returns 0 on NULL input.
    */
   int util_url_host_is_case_insensitive(const char *host);

#ifdef __cplusplus
}
#endif

#endif /* DEC_UTIL_URL_H */
