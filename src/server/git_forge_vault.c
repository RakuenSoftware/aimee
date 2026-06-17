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
