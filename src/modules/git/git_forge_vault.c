/* git_forge_vault.c — autonomous read of a webuser's git credentials from the
 * sealed vault (server wrap). See git_forge_vault.h. */
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
   vault_status_t st =
       vault_service_get_server_wrap(principal, GIT_FORGE_VAULT_AGENT, cred, out, out_len);
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
 * non-empty value was read, 0 otherwise. The ambient config nulling matches
 * git_ops.c: only the checkout's own config is consulted, so this cannot pick
 * up a system/global setting that changes how git behaves. */
static int read_git_config(const char *repo_dir, const char *key, char *out, size_t out_len)
{
   if (!out || !out_len)
      return 0;
   out[0] = '\0';

   char *dir = shell_escape(repo_dir && repo_dir[0] ? repo_dir : ".");
   if (!dir)
      return 0;
   char cmd[4608];
   snprintf(cmd, sizeof(cmd),
            "GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_SYSTEM=/dev/null GIT_CONFIG_GLOBAL=/dev/null "
            "git -C '%s' config --get %s 2>/dev/null",
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

int git_identity_resolve(const char *repo_dir, char *name_out, size_t name_len, char *email_out,
                         size_t email_len)
{
   int rc = git_identity_get(name_out, name_len, email_out, email_len);
   if (rc != 0)
      return rc; /* sealed identity, or a fail-closed vault error */

   /* Vault is clean. Use the identity the operator already configured for this
    * checkout rather than stopping to demand an install-time step. */
   int have_name = read_git_config(repo_dir, "user.name", name_out, name_len);
   int have_email = read_git_config(repo_dir, "user.email", email_out, email_len);
   if (have_name && have_email)
      return 1;

   /* Half an identity is not an identity — same rule as the vault path. */
   name_out[0] = '\0';
   email_out[0] = '\0';
   return 0;
}
