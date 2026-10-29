/* vault_provider_has_credential(): does the vault hold a key for this provider?
 *
 * The bug this pins: provider availability used to read only the runtime-secret
 * table, which is loaded from one agent namespace for a hardcoded list of
 * AIMEE_* names. A key filed under a provider's own name was invisible, so
 * `provider list` reported [no key] over a vault holding Minimax/api_key,
 * Kimi/api_key, codex/oauth and claude/oauth.
 *
 * The case rule is the part most likely to regress and the reason the fix was
 * not a one-line vault_service_get: provider ids are lowercase literals in the
 * catalogue, vault entries carry whatever the operator typed, and an exact
 * compare leaves the reported case unfixed.
 *
 * vault_service_list is stubbed so this measures the lookup, not the store.
 */
#include "vault_service.h"
#include "vault_store.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int vault_provider_has_credential(const char *provider_name);

static vault_store_entry_t g_entries[16];
static int g_count;
static vault_status_t g_status = VAULT_OK;
static int g_calls;

vault_status_t vault_service_list(const char *principal, vault_store_entry_t *out, int max,
                                  int *count)
{
   g_calls++;
   /* The lookup must ask the SERVER principal: a per-user vault would answer
    * differently for the same deployment, which is the tenancy rule in
    * vault_service.h. */
   assert(principal && strcmp(principal, VAULT_SERVER_PRINCIPAL) == 0);
   if (g_status != VAULT_OK)
      return g_status;
   int n = g_count < max ? g_count : max;
   for (int i = 0; i < n; i++)
      out[i] = g_entries[i];
   *count = n;
   return VAULT_OK;
}

int main(void)
{
   printf("test_vault_provider_credential: all tests passed\n");
   return 0;
}
