/* git_cred_inject_stub.c: no-op stub of the ONE git-credential policy, for tests
 * that link mcp_git_query.o / workspace_turn.o (which now resolve creds through
 * git_cred_inject_build_env_for_repo) but exercise only routing / turn logic, not
 * real credential injection. Returning NULL means "no credential" → the caller
 * falls through to the ambient/shared-provider exec path, which is exactly what
 * these tests want. Binaries that need the real behaviour
 * (unit-test-git-cred-inject / -git-ops / -git-project) link the real object and
 * must NOT also link this TU. */
#include "modules/git/git_cred_inject.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* The commit identity comes from the vault via git_forge_vault.o, which these
 * binaries do not link (it would pull the whole vault in for tests that exercise
 * routing, not credentials). Report a configured identity so the commit path
 * proceeds to the behaviour under test: a stub returning "not configured" would
 * make every commit test assert the refusal instead. Binaries that test the
 * refusal link the real object. */
int git_identity_get(char *name_out, size_t name_len, char *email_out, size_t email_len);

int git_identity_get(char *name_out, size_t name_len, char *email_out, size_t email_len)
{
   if (!name_out || !name_len || !email_out || !email_len)
      return -1;
   snprintf(name_out, name_len, "%s", "Test Operator");
   snprintf(email_out, email_len, "%s", "operator@example.test");
   return 1;
}

int git_identity_resolve(const char *repo_dir, char *name_out, size_t name_len, char *email_out,
                         size_t email_len);

int git_identity_resolve(const char *repo_dir, char *name_out, size_t name_len, char *email_out,
                         size_t email_len)
{
   (void)repo_dir; /* the stub always has a sealed identity, so no fallback runs */
   return git_identity_get(name_out, name_len, email_out, email_len);
}

int git_identity_resolve_with(const char *principal,
                              int (*read_cfg)(const char *, char *, size_t, void *), void *ud,
                              char *name_out, size_t name_len, char *email_out, size_t email_len);

int git_identity_resolve_with(const char *principal,
                              int (*read_cfg)(const char *, char *, size_t, void *), void *ud,
                              char *name_out, size_t name_len, char *email_out, size_t email_len)
{
   (void)principal;
   if (getenv("AIMEE_TEST_GIT_IDENTITY_FROM_CONFIG"))
   {
      if (!read_cfg || !read_cfg("user.name", name_out, name_len, ud) ||
          !read_cfg("user.email", email_out, email_len, ud))
      {
         if (name_out && name_len)
            name_out[0] = '\0';
         if (email_out && email_len)
            email_out[0] = '\0';
         return 0;
      }
      return 1;
   }
   (void)read_cfg;
   (void)ud; /* normally the stub always has a sealed identity */
   return git_identity_get(name_out, name_len, email_out, email_len);
}
