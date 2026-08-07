/* git_pr_api_stub.c: no-op stub of the in-process GitHub REST open-PR for tests
 * that link git_ops.o (whose "pr" op calls git_pr_create_via_api) but exercise
 * only op routing, not real PR creation — so they need not pull in the HTTP
 * client / TLS stack. The real object (server/git_pr_api.o) is linked by binaries
 * that need real behaviour and must NOT also link this TU. */
#include "modules/git/git_pr_api.h"

#include <stddef.h>
#include <stdio.h>

/* The no-checkout variant mcp_git_pr.c's create action calls after resolving the
 * slug through the workspace runner. Refuses like the wrapper below: these
 * binaries assert on routing and validation, never on a created PR. */
int git_pr_create_via_api_slug(const char *principal, const char *slug, const char *head,
                               const char *base, const char *title, const char *body, int draft,
                               char *out, size_t out_cap, char *err, size_t errlen)
{
   (void)principal;
   (void)slug;
   (void)head;
   (void)base;
   (void)title;
   (void)body;
   (void)draft;
   if (out && out_cap)
      out[0] = '\0';
   if (err && errlen)
      snprintf(err, errlen, "pr api unavailable (stub)");
   return -1;
}

int git_pr_create_via_api(const char *principal, const char *repo_dir, const char *title,
                          const char *body, char *out, size_t out_cap, char *err, size_t errlen)
{
   (void)principal;
   (void)repo_dir;
   (void)title;
   (void)body;
   if (out && out_cap)
      out[0] = '\0';
   if (err && errlen)
      snprintf(err, errlen, "pr api unavailable (stub)");
   return -1;
}
