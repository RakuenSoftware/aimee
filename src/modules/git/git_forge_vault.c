/* git_forge_vault.c — autonomous read of a webuser's git credentials from the
 * sealed vault (server wrap). See git_forge_vault.h. */
#include "git_forge_vault.h"
#include "vault_service.h"

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
