/* git_pr_api_stub.c: no-op stub of the in-process GitHub REST open-PR for tests
 * that link git_ops.o (whose "pr" op calls git_pr_create_via_api) or
 * mcp_git_pr.o (whose create action calls git_pr_create_via_api_ex) but exercise
 * only op routing and parameter validation, not real PR creation — so they need
 * not pull in the HTTP client / TLS stack. The real object (server/git_pr_api.o)
 * is linked by binaries that need real behaviour and must NOT also link this TU. */
#include "modules/git/git_pr_api.h"

#include <stddef.h>
#include <stdio.h>

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

/* The explicit head/base variant, which mcp_git_pr.c's create action calls so the
 * PR names the owned branch rather than whatever HEAD happens to be. Fails like
 * the wrapper above rather than synthesizing a URL: every caller in these
 * binaries returns earlier (missing title, missing action, the verify gate), so
 * this does not run today, and a refusal is easier to diagnose than a plausible
 * fake success that makes a test assert against a PR nobody created. */
int git_pr_create_via_api_ex(const char *principal, const char *repo_dir, const char *head,
                             const char *base, const char *title, const char *body, char *out,
                             size_t out_cap, char *err, size_t errlen)
{
   (void)head;
   (void)base;
   return git_pr_create_via_api(principal, repo_dir, title, body, out, out_cap, err, errlen);
}
