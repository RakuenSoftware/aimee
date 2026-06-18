/* git_cred_inject.c — vault-sourced git credential env. See git_cred_inject.h. */
#include "git_cred_inject.h"
#include "forge_credentials.h" /* forge_cred_askpass_shim */
#include "git_forge_vault.h"   /* git_forge_vault_token */
#include "git_ssh_agent.h"     /* git_ssh_agent_ensure */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GIT_CRED_TOKEN_MAX 4096

/* True if `entry` is "KEY=...". */
static int is_key(const char *entry, const char *key)
{
   size_t kl = strlen(key);
   return strncmp(entry, key, kl) == 0 && entry[kl] == '=';
}

/* Build the git child env: a copy of `parent` with the credential-carrying keys
 * we manage dropped, plus GH_TOKEN/GIT_ASKPASS/GIT_TERMINAL_PROMPT (when a token
 * is given) and SSH_AUTH_SOCK (when a sock is given). Returns NULL on OOM. */
static char **build_env(char *const *parent, const char *token, const char *shim, const char *sock)
{
   size_t pc = 0;
   if (parent)
      while (parent[pc])
         pc++;
   char **out = calloc(pc + 5, sizeof(char *)); /* +GH_TOKEN,ASKPASS,PROMPT,SSH_AUTH_SOCK,NULL */
   if (!out)
      return NULL;
   size_t o = 0;
   for (size_t i = 0; i < pc; i++)
   {
      if (is_key(parent[i], "GH_TOKEN") || is_key(parent[i], "GITHUB_TOKEN") ||
          is_key(parent[i], "GIT_ASKPASS") || is_key(parent[i], "GIT_TERMINAL_PROMPT") ||
          is_key(parent[i], "SSH_AUTH_SOCK"))
         continue;
      out[o] = strdup(parent[i]);
      if (!out[o++])
         goto oom;
   }
   if (token && token[0])
   {
      char buf[GIT_CRED_TOKEN_MAX + 16];
      snprintf(buf, sizeof(buf), "GH_TOKEN=%s", token);
      if (!(out[o++] = strdup(buf)))
         goto oom;
      if (shim && shim[0])
      {
         char ab[2048];
         snprintf(ab, sizeof(ab), "GIT_ASKPASS=%s", shim);
         if (!(out[o++] = strdup(ab)))
            goto oom;
      }
      if (!(out[o++] = strdup("GIT_TERMINAL_PROMPT=0")))
         goto oom;
   }
   if (sock && sock[0])
   {
      char sb[2300];
      snprintf(sb, sizeof(sb), "SSH_AUTH_SOCK=%s", sock);
      if (!(out[o++] = strdup(sb)))
         goto oom;
   }
   out[o] = NULL;
   return out;
oom:
   git_cred_inject_free_env(out);
   return NULL;
}

char **git_cred_inject_build_env(const char *principal, char *const *parent_environ)
{
   char token[GIT_CRED_TOKEN_MAX];
   /* Autonomous server-wrap read of the webuser's OWN vaulted forge token (an
    * optional personal override). 1 = token, 0 = none. */
   int have_token = (git_forge_vault_token(principal, token, sizeof(token)) == 1);

   /* Default for a personal-agent server: when the user has no personal token,
    * use aimee-server's OWN configured git identity (a GitHub App installation
    * token or AIMEE_FORGE_TOKEN). The clone/editor then authenticates with the
    * server's credential, so a webchat user never has to store a PAT. */
   if (!have_token)
      have_token = (forge_cred_server_identity(token, sizeof(token), NULL, 0) == 1);

   /* Start (or reuse) the user's in-memory ssh-agent if they have a vaulted SSH
    * key (WP-C2); the key never touches disk. 1 = sock ready. */
   char sock[2048];
   int have_sock = (git_ssh_agent_ensure(principal, sock, sizeof(sock)) == 1);

   char **envp = NULL;
   if (have_token || have_sock)
      envp = build_env(parent_environ, have_token ? token : NULL, forge_cred_askpass_shim(),
                       have_sock ? sock : NULL);

   /* Wipe our stack copy of the token; the remaining plaintext is only the
    * GH_TOKEN entry in envp, which git_cred_inject_free_env zeroes. */
   if (have_token)
   {
      volatile char *p = (volatile char *)token;
      for (size_t i = 0; i < sizeof(token); i++)
         p[i] = 0;
   }
   return envp;
}

void git_cred_inject_free_env(char **envp)
{
   if (!envp)
      return;
   for (int i = 0; envp[i]; i++)
   {
      /* zero a GH_TOKEN entry's secret before freeing so it doesn't linger */
      if (is_key(envp[i], "GH_TOKEN"))
      {
         volatile char *p = (volatile char *)envp[i];
         for (size_t j = 0; p[j]; j++)
            p[j] = 0;
      }
      free(envp[i]);
   }
   free(envp);
}
