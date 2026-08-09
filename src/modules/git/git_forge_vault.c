/* git_forge_vault.c — autonomous read of environment Git credentials. */
#include "git_forge_vault.h"
#include "util.h" /* shell_escape */
#include "vault_service.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Map a vault_service server-wrap read to the 1/0/-1 contract:
 *   VAULT_OK       -> 1 (token written)
 *   VAULT_NO_ENTRY -> 0 (none stored — fall back to ambient creds)
 *   anything else  -> -1 (fail closed; out already cleansed by vault_service). */
static int read_cred(const char *principal, const char *cred, char *out, size_t out_len)
{
   if (out && out_len)
      out[0] = '\0';
   (void)principal; /* actor attribution, never a credential namespace */
   vault_status_t st =
       vault_service_get_server_principal(GIT_FORGE_VAULT_AGENT, cred, out, out_len);
   if (st == VAULT_OK)
      return 1;
   if (st == VAULT_NO_ENTRY)
      return 0;
   return -1;
}

int git_forge_vault_token(const char *principal, char *out, size_t out_len)
{
   return read_cred(principal, GIT_FORGE_TOKEN_CRED, out, out_len);
}

int git_forge_vault_sshkey(const char *principal, char *out, size_t out_len)
{
   return read_cred(principal, GIT_FORGE_SSHKEY_CRED, out, out_len);
}

int git_forge_vault_server_token(char *out, size_t out_len)
{
   if (out && out_len)
      out[0] = '\0';
   vault_status_t st = vault_service_get_server_principal(GIT_FORGE_VAULT_AGENT,
                                                          GIT_FORGE_TOKEN_CRED, out, out_len);
   if (st == VAULT_OK)
      return 1;
   if (st == VAULT_NO_ENTRY)
      return 0;
   return -1;
}

int git_identity_get(char *name_out, size_t name_len, char *email_out, size_t email_len)
{
   if (name_out && name_len)
      name_out[0] = '\0';
   if (email_out && email_len)
      email_out[0] = '\0';
   if (!name_out || !name_len || !email_out || !email_len)
      return -1;

   vault_status_t ns = vault_service_get_server_principal(GIT_FORGE_VAULT_AGENT,
                                                          GIT_AUTHOR_NAME_CRED, name_out, name_len);
   vault_status_t es = vault_service_get_server_principal(
       GIT_FORGE_VAULT_AGENT, GIT_AUTHOR_EMAIL_CRED, email_out, email_len);
   if (ns == VAULT_OK && es == VAULT_OK && name_out[0] && email_out[0])
      return 1;
   /* Fail closed on a real error; report "not configured" for a missing or
    * half-written pair so the caller can say so plainly. */
   int err = (ns != VAULT_OK && ns != VAULT_NO_ENTRY) || (es != VAULT_OK && es != VAULT_NO_ENTRY);
   name_out[0] = '\0';
   email_out[0] = '\0';
   return err ? -1 : 0;
}

/* Read one git config value for |repo_dir| into |out|. Returns 1 when a
 * non-empty value was read, 0 otherwise.
 *
 * |scope| selects how much config git may consult: SCOPE_REPO nulls the ambient
 * files the way git_ops.c does, so only the checkout's own .git/config answers;
 * SCOPE_ANY uses git's normal resolution, which also reaches the operator's
 * ~/.gitconfig. Only user.name/user.email are ever read through here, and only
 * as data to hand back via `git -c` — so widening the scope cannot let ambient
 * config change how git BEHAVES, which is what the nulling exists to prevent. */
#define GIT_CFG_SCOPE_REPO 0
#define GIT_CFG_SCOPE_ANY  1

static int read_git_config(const char *repo_dir, const char *key, int scope, char *out,
                           size_t out_len)
{
   if (!out || !out_len)
      return 0;
   out[0] = '\0';

   char *dir = shell_escape(repo_dir && repo_dir[0] ? repo_dir : ".");
   if (!dir)
      return 0;
   char cmd[4608];
   snprintf(cmd, sizeof(cmd), "%sgit -C '%s' config --get %s 2>/dev/null",
            scope == GIT_CFG_SCOPE_REPO
                ? "GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_SYSTEM=/dev/null GIT_CONFIG_GLOBAL=/dev/null "
                : "unset GIT_CONFIG_NOSYSTEM GIT_CONFIG_SYSTEM GIT_CONFIG_GLOBAL; ",
            dir, key);
   free(dir);

   FILE *p = popen(cmd, "r");
   if (!p)
      return 0;
   char *got = fgets(out, (int)out_len, p);
   pclose(p);
   if (!got)
   {
      out[0] = '\0';
      return 0;
   }
   out[strcspn(out, "\r\n")] = '\0';
   return out[0] ? 1 : 0;
}

/* Reader context for the in-process (popen) path: a checkout directory. */
static int popen_config_reader(const char *key, char *out, size_t out_len, void *ud)
{
   const char *repo_dir = (const char *)ud;
   /* The checkout's own config wins — a per-repo identity is a deliberate
    * choice — then the operator's ordinary git identity, which is where most
    * people actually have one. */
   if (read_git_config(repo_dir, key, GIT_CFG_SCOPE_REPO, out, out_len))
      return 1;
   return read_git_config(repo_dir, key, GIT_CFG_SCOPE_ANY, out, out_len);
}

/* |principal|'s own sealed author identity (server-wrapped, so it reads with no
 * user unlock — the same autonomous path the forge token uses). 1 when BOTH
 * fields are present, 0 when absent or half-written, -1 on a fail-closed
 * error. */
int git_identity_resolve_with(const char *principal, git_config_reader_fn read_cfg, void *ud,
                              char *name_out, size_t name_len, char *email_out, size_t email_len)
{
   if (!name_out || !name_len || !email_out || !email_len)
      return -1;

   (void)principal; /* all PAM actors commit as the configured environment */

   int rc = git_identity_get(name_out, name_len, email_out, email_len);
   if (rc != 0)
      return rc; /* server-sealed identity, or a fail-closed vault error */
   if (!read_cfg)
      return 0;

   /* Vault is clean. Use the identity the operator already configured rather
    * than stopping to demand an install-time step. */
   int have_name = read_cfg("user.name", name_out, name_len, ud);
   int have_email = read_cfg("user.email", email_out, email_len, ud);
   if (have_name && have_email)
      return 1;

   /* Half an identity is not an identity — same rule as the vault path. */
   name_out[0] = '\0';
   email_out[0] = '\0';
   return 0;
}

int git_identity_resolve(const char *repo_dir, char *name_out, size_t name_len, char *email_out,
                         size_t email_len)
{
   return git_identity_resolve_with(NULL, popen_config_reader, (void *)repo_dir, name_out, name_len,
                                    email_out, email_len);
}
