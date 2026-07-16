/* git_cred_inject.c — vault-sourced git credential env. See git_cred_inject.h. */
#define _GNU_SOURCE 1
#include "git_cred_inject.h"
#include "forge_credentials.h" /* forge_cred_askpass_shim / forge_cred_server_identity */
#include "git_forge_vault.h"   /* git_forge_vault_token */
#include "git_host_cred.h"     /* git_host_cred_list — "is git configured at all?" */
#include "git_host_resolve.h"  /* git_host_resolve_token (per-host vault seam) */
#include "git_ssh_agent.h"     /* git_ssh_agent_ensure */
#include "util.h"              /* GIT_AGENT_SSH_COMMAND */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h> /* memfd_create — anonymous in-memory token fd (fd mode) */
#include <unistd.h>   /* write, close */

#define GIT_CRED_TOKEN_MAX 4096

/* True if `entry` is "KEY=...". */
static int is_key(const char *entry, const char *key)
{
   size_t kl = strlen(key);
   return strncmp(entry, key, kl) == 0 && entry[kl] == '=';
}

/* Build the git child env: a copy of `parent` with the credential-carrying keys
 * we manage dropped, plus GH_TOKEN/GIT_ASKPASS/GIT_TERMINAL_PROMPT (when a token
 * is given) and SSH_AUTH_SOCK + GIT_SSH_COMMAND (when a sock is given, so an SSH
 * remote uses the agent key with a non-interactive TOFU host-key policy).
 * Returns NULL on OOM. */
/* token_fd_target >= 0 selects FD MODE: advertise AIMEE_GIT_TOKEN_FD=<target>
 * (the askpass reads the token from that inherited fd) and do NOT put the secret
 * in the env. token_fd_target < 0 is LEGACY ENV MODE: GH_TOKEN=<token>. In both
 * modes `token` non-empty means "a token exists" (so the askpass is wired). */
static char **build_env(char *const *parent, const char *token, const char *shim, const char *sock,
                        int token_fd_target)
{
   size_t pc = 0;
   if (parent)
      while (parent[pc])
         pc++;
   /* +token-key,ASKPASS,PROMPT,SSH_AUTH_SOCK,GIT_SSH_COMMAND,NULL */
   char **out = calloc(pc + 6, sizeof(char *));
   if (!out)
      return NULL;
   size_t o = 0;
   for (size_t i = 0; i < pc; i++)
   {
      if (is_key(parent[i], "GH_TOKEN") || is_key(parent[i], "GITHUB_TOKEN") ||
          is_key(parent[i], "GIT_ASKPASS") || is_key(parent[i], "GIT_TERMINAL_PROMPT") ||
          is_key(parent[i], "SSH_AUTH_SOCK") || is_key(parent[i], "GIT_SSH_COMMAND") ||
          is_key(parent[i], "AIMEE_GIT_TOKEN_FD"))
         continue;
      out[o] = strdup(parent[i]);
      if (!out[o++])
         goto oom;
   }
   if (token && token[0])
   {
      char buf[GIT_CRED_TOKEN_MAX + 16];
      if (token_fd_target >= 0)
         snprintf(buf, sizeof(buf), "AIMEE_GIT_TOKEN_FD=%d", token_fd_target);
      else
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
      /* Force the non-interactive TOFU SSH command so an SSH remote authenticates
       * with the agent key and doesn't stall on a first-time host key. Pin
       * UserKnownHostsFile to a file beside the agent socket, in this principal's
       * own tmpfs runtime dir — so accept-new's first-use trust is scoped to this
       * principal and never bleeds through the OS account's shared
       * ~/.ssh/known_hosts to other tenants. */
      char kh[2300];
      snprintf(kh, sizeof(kh), "%s", sock);
      char *slash = strrchr(kh, '/');
      char cmd[3200];
      if (slash)
      {
         snprintf(slash + 1, sizeof(kh) - (size_t)(slash + 1 - kh), "known_hosts");
         snprintf(cmd, sizeof(cmd), "GIT_SSH_COMMAND=%s -o UserKnownHostsFile=%s",
                  GIT_AGENT_SSH_COMMAND, kh);
      }
      else
      {
         /* No directory component (shouldn't happen for a runtime-dir socket) —
          * fall back to the bare command rather than a malformed -o path. */
         snprintf(cmd, sizeof(cmd), "GIT_SSH_COMMAND=%s", GIT_AGENT_SSH_COMMAND);
      }
      if (!(out[o++] = strdup(cmd)))
         goto oom;
   }
   out[o] = NULL;
   return out;
oom:
   git_cred_inject_free_env(out);
   return NULL;
}

/* Zero `n` bytes of `p` so the wipe can't be optimised away. */
static void wipe(void *p, size_t n)
{
   volatile unsigned char *v = (volatile unsigned char *)p;
   while (n--)
      *v++ = 0;
}

/* Resolve the effective HTTPS token per the documented precedence into out.
 * Returns 1 (token written) or 0 (none — caller may still have ssh/ambient).
 * `out` is always either a full token or empty; never a partial value. */
static int resolve_token(const char *principal, const char *remote_url, const char *repo_dir,
                         const char *preferred_token, char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   if (!out || cap == 0)
      return 0;

   /* 1. A live caller-supplied token (inline clone token / workspace broker). */
   if (preferred_token && preferred_token[0])
   {
      if ((size_t)snprintf(out, cap, "%s", preferred_token) < cap)
         return 1;
      wipe(out, cap); /* would truncate a secret → wipe the partial, fail closed */
      return 0;
   }

   /* 2. Per-host vault token, keyed by the repo's remote host (resolved from the
    * explicit remote URL, else the checkout's `origin`), via the shared seam. */
   if (git_host_resolve_token(remote_url, repo_dir, out, cap) == 1 && out[0])
      return 1;
   out[0] = '\0';

   /* 3. The principal's own vaulted personal forge token. */
   if (principal && principal[0] && git_forge_vault_token(principal, out, cap) == 1 && out[0])
      return 1;
   out[0] = '\0';

   /* 4. The server's own git identity (App installation token / AIMEE_FORGE_TOKEN). */
   if (forge_cred_server_identity(out, cap, NULL, 0) == 1 && out[0])
      return 1;
   out[0] = '\0';
   return 0;
}

char **git_cred_inject_build_env_for_repo(const char *principal, const char *remote_url,
                                          const char *repo_dir, const char *preferred_token,
                                          char *const *parent_environ, int *out_token_fd)
{
   if (out_token_fd)
      *out_token_fd = -1;
   const int fd_mode = (out_token_fd != NULL);

   char token[GIT_CRED_TOKEN_MAX] = {0};
   int have_token =
       resolve_token(principal, remote_url, repo_dir, preferred_token, token, sizeof(token));

   /* FD MODE: stage the token in an anonymous CLOEXEC memfd (never a named path,
    * never in the env). The askpass reads it via /proc/self/fd/<target>; the
    * caller inherits this fd into git and closes it after. memfd failure fails
    * closed (drop the token) rather than silently leaking it to the env. */
   int token_fd = -1;
   if (have_token && fd_mode)
   {
      int mfd = memfd_create("c", MFD_CLOEXEC); /* opaque name: don't advertise a cred in fdinfo */
      size_t tn = strlen(token);
      if (mfd >= 0 && write(mfd, token, tn) == (ssize_t)tn && write(mfd, "\n", 1) == 1)
         token_fd = mfd;
      else
      {
         if (mfd >= 0)
            close(mfd);
         have_token = 0; /* fail closed */
      }
   }

   /* Start (or reuse) the user's in-memory ssh-agent if they have a vaulted SSH
    * key (WP-C2); the key never touches disk. 1 = sock ready. */
   char sock[2048];
   int have_sock = (git_ssh_agent_ensure(principal, sock, sizeof(sock)) == 1);

   char **envp = NULL;
   if (have_token || have_sock)
      envp = build_env(parent_environ, have_token ? token : NULL, forge_cred_askpass_shim(),
                       have_sock ? sock : NULL,
                       (fd_mode && have_token) ? GIT_CRED_TOKEN_TARGET_FD : -1);

   if (!envp && token_fd >= 0) /* build failed → don't leak the memfd */
   {
      close(token_fd);
      token_fd = -1;
   }

   /* Wipe our stack copy of the token unconditionally (the buffer is zero-inited,
    * so this is always safe). In fd mode the only remaining plaintext is inside
    * the memfd (kernel memory, closed by the caller after exec); in env mode it
    * is the GH_TOKEN entry git_cred_inject_free_env zeroes. */
   wipe(token, sizeof(token));
   if (out_token_fd)
      *out_token_fd = token_fd;
   return envp;
}

char **git_cred_inject_build_env(const char *principal, char *const *parent_environ)
{
   return git_cred_inject_build_env_for_repo(principal, NULL, NULL, NULL, parent_environ, NULL);
}

int git_cred_inject_resolve_token(const char *principal, const char *remote_url,
                                  const char *repo_dir, const char *preferred_token, char *out,
                                  size_t cap)
{
   return resolve_token(principal, remote_url, repo_dir, preferred_token, out, cap);
}

int git_cred_forge_configured(void)
{
   /* Any per-host vault entry means git is configured, whatever repo is in play.
    * Host NAMES only — git_host_cred_list never returns tokens, so this asks the
    * question without materialising a secret. */
   char hosts[1][GIT_HOST_MAX];
   if (git_host_cred_list(hosts, 1) > 0)
      return 1;

   /* Else the server's own identity (a forge App installation / AIMEE_FORGE_TOKEN). */
   char token[GIT_CRED_TOKEN_MAX] = {0};
   int have = (forge_cred_server_identity(token, sizeof(token), NULL, 0) == 1 && token[0]);
   volatile char *p = (volatile char *)token; /* never let the token outlive the check */
   for (size_t i = 0; i < sizeof(token); i++)
      p[i] = 0;
   return have;
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
