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

static void vault_clear(void)
{
   memset(g_entries, 0, sizeof(g_entries));
   g_count = 0;
   g_status = VAULT_OK;
   g_calls = 0;
}

static void vault_add(const char *agent, const char *cred)
{
   snprintf(g_entries[g_count].agent, sizeof(g_entries[g_count].agent), "%s", agent);
   snprintf(g_entries[g_count].cred, sizeof(g_entries[g_count].cred), "%s", cred);
   g_count++;
}

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

static void test_exact_name_matches(void)
{
   vault_clear();
   vault_add("minimax", "api_key");
   assert(vault_provider_has_credential("minimax") == 1);
}

/* The reported case: the operator typed "Minimax", the catalogue id is
 * "minimax", and before the fix that provider reported [no key]. */
static void test_operator_casing_matches_catalogue_id(void)
{
   vault_clear();
   vault_add("Minimax", "api_key");
   assert(vault_provider_has_credential("minimax") == 1);
}

static void test_any_credential_name_counts(void)
{
   vault_clear();
   vault_add("codex", "oauth_refresh_token");
   assert(vault_provider_has_credential("codex") == 1);
}

static void test_other_providers_do_not_match(void)
{
   vault_clear();
   vault_add("Minimax", "api_key");
   vault_add("Kimi", "api_key");
   assert(vault_provider_has_credential("anthropic") == 0);
}

/* A name that merely contains the provider is a different agent. Substring
 * matching here would report a key that authenticates nothing. */
static void test_partial_name_does_not_match(void)
{
   vault_clear();
   vault_add("minimax-staging", "api_key");
   assert(vault_provider_has_credential("minimax") == 0);
}

static void test_entry_without_a_credential_name_does_not_count(void)
{
   vault_clear();
   vault_add("minimax", "");
   assert(vault_provider_has_credential("minimax") == 0);
}

static void test_empty_vault(void)
{
   vault_clear();
   assert(vault_provider_has_credential("minimax") == 0);
}

/* Fail closed. A vault that cannot be read must report "not configured" rather
 * than a credential that is not there — the opposite would print [key set] for
 * a provider that will fail on first use. */
static void test_vault_error_reports_unconfigured(void)
{
   vault_clear();
   vault_add("minimax", "api_key");
   g_status = VAULT_ERR_IO;
   assert(vault_provider_has_credential("minimax") == 0);
}

static void test_null_and_empty_names_do_not_reach_the_vault(void)
{
   vault_clear();
   vault_add("minimax", "api_key");
   assert(vault_provider_has_credential(NULL) == 0);
   assert(vault_provider_has_credential("") == 0);
   assert(g_calls == 0);
}

int main(void)
{
   test_exact_name_matches();
   test_operator_casing_matches_catalogue_id();
   test_any_credential_name_counts();
   test_other_providers_do_not_match();
   test_partial_name_does_not_match();
   test_entry_without_a_credential_name_does_not_count();
   test_empty_vault();
   test_vault_error_reports_unconfigured();
   test_null_and_empty_names_do_not_reach_the_vault();
   printf("test_vault_provider_credential: all tests passed\n");
   return 0;
}
