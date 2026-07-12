#ifndef GIT_ORG_REPOS_H
#define GIT_ORG_REPOS_H 1

#include <stddef.h>

/* git_org_repos — provider-agnostic enumeration of the repositories under an
 * owner/org on a git host, for the "Workspaces & projects" wizard page. A
 * workspace is the collection of repos under an owner (e.g. github.com/…); this
 * lists them so the operator can bulk-clone a selection. Credentials come from the
 * per-host token store (git_host_cred); a public org lists unauthenticated.
 *
 * Supported providers: GitHub, GitLab, Gitea/Forgejo, Bitbucket. The provider is
 * detected from the host; self-hosted hosts fall back to the Gitea API shape (the
 * most common self-hosted default), except hosts whose name carries a provider
 * hint ("gitlab"/"bitbucket"/"github"). */

struct cJSON;

typedef enum
{
   GIT_ORG_UNKNOWN = 0,
   GIT_ORG_GITHUB,
   GIT_ORG_GITLAB,
   GIT_ORG_GITEA,
   GIT_ORG_BITBUCKET,
} git_org_provider_t;

/* Detect the provider from a bare host ("github.com", "gitlab.example.com", …).
 * Returns GIT_ORG_UNKNOWN only for a NULL/empty host. */
git_org_provider_t git_org_detect(const char *host);

/* Canonical provider name: "github" | "gitlab" | "gitea" | "bitbucket" | "unknown". */
const char *git_org_provider_name(git_org_provider_t p);

/* Parse ONE page of a provider's repo-list JSON `json` into `out` (a cJSON array),
 * appending {name, clone_url, ssh_url, private} objects. Returns the count appended
 * (>=0), or -1 when the body is not the expected shape. Pure — no network. Exposed
 * for unit tests. */
int git_org_parse(git_org_provider_t p, const char *json, struct cJSON *out);

/* Enumerate every repo under `owner` on `host` (paginated). On success returns 0,
 * sets *out to a newly-allocated cJSON array (caller cJSON_Delete's it) of
 * {name, clone_url, ssh_url, private} objects, and copies the provider name into
 * provider[provider_cap]. On failure returns an HTTP status to relay to the client
 * (400 bad args, 401 unauthorized upstream, 404 owner not found, 502 upstream/parse
 * failure) and fills err[errlen]. Uses the per-host token from git_host_cred when
 * present; unauthenticated otherwise. */
int git_org_repos_list(const char *host, const char *owner, struct cJSON **out,
                       char *provider, size_t provider_cap, char *err, size_t errlen);

#endif /* GIT_ORG_REPOS_H */
