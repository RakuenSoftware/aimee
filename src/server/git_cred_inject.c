/* git_cred_inject.c — vault-sourced git credential env. See git_cred_inject.h. */
#include "git_cred_inject.h"
#include "forge_credentials.h" /* forge_cred_build_env_from_token, askpass_shim, free_env */
#include "git_forge_vault.h"   /* git_forge_vault_token */

#include <string.h>

/* Token plaintext buffer — sized to the forge broker's token cap. */
#define GIT_CRED_TOKEN_MAX 4096

char **git_cred_inject_build_env(const char *principal, char *const *parent_environ)
{
   char token[GIT_CRED_TOKEN_MAX];
   /* Autonomous server-wrap read: works for background / code-server git ops
    * after the user's session KEK has expired. 1 = token, 0 = none, -1 = error. */
   if (git_forge_vault_token(principal, token, sizeof(token)) != 1)
      return NULL; /* no vaulted token (or fail-closed error) -> ambient creds */

   char **envp = forge_cred_build_env_from_token(token, parent_environ, forge_cred_askpass_shim());
   /* Wipe our stack copy; the only remaining plaintext is the GH_TOKEN entry in
    * envp, which git_cred_inject_free_env zeroes. */
   volatile char *p = (volatile char *)token;
   for (size_t i = 0; i < sizeof(token); i++)
      p[i] = 0;
   return envp;
}

void git_cred_inject_free_env(char **envp)
{
   forge_cred_free_env(envp); /* zeroes the GH_TOKEN entry, then frees */
}
