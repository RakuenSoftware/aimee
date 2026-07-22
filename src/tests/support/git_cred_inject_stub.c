/* git_cred_inject_stub.c: no-op stub of the ONE git-credential policy, for tests
 * that link mcp_git_query.o / workspace_turn.o (which now resolve creds through
 * git_cred_inject_build_env_for_repo) but exercise only routing / turn logic, not
 * real credential injection. Returning NULL means "no credential" → the caller
 * falls through to the ambient/shared-provider exec path, which is exactly what
 * these tests want. Binaries that need the real behaviour
 * (unit-test-git-cred-inject / -git-ops / -git-project) link the real object and
 * must NOT also link this TU. */
#include "git_cred_inject.h"

#include <stddef.h>

char **git_cred_inject_build_env_for_repo(const char *principal, const char *remote_url,
                                          const char *repo_dir, const char *preferred_token,
                                          char *const *parent_environ, int *out_token_fd)
{
   (void)principal;
   (void)remote_url;
   (void)repo_dir;
   (void)preferred_token;
   (void)parent_environ;
   if (out_token_fd)
      *out_token_fd = -1;
   return NULL;
}

char **git_cred_inject_build_env(const char *principal, char *const *parent_environ)
{
   (void)principal;
   (void)parent_environ;
   return NULL;
}

void git_cred_inject_free_env(char **envp)
{
   (void)envp;
}

/* "aimee-server has no forge credential configured", consistent with the NULL env
 * above. For provider_cli_adapter's spawn that means the delegate credential strip
 * does NOT apply — no aimee git route, no restriction — which is the honest answer
 * for a test binary that has no vault, and keeps these tests exercising the spawn
 * rather than a credential policy they never configured. */
int git_cred_forge_configured(void)
{
   return 0;
}
