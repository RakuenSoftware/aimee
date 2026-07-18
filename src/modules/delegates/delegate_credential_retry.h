#ifndef DEC_DELEGATE_CREDENTIAL_RETRY_H
#define DEC_DELEGATE_CREDENTIAL_RETRY_H 1

#include <stddef.h>

#include "aimee.h"
#include "agent_types.h"

/* Result of resolving a delegate's credential before the run (WP-C.1 vault-first,
 * WP-C.3 per-principal cooldown). The caller maps a non-OK status to a delegate
 * error; on OK it proceeds with target_agent->api_key set (vault or env hit) and
 * the named pool row to release on exit. */
typedef enum
{
   DELEGATE_CRED_RESOLVE_OK = 0,    /* proceed (api_key set, or no pool/vault) */
   DELEGATE_CRED_RESOLVE_LOCKED,    /* vault entry exists but is locked */
   DELEGATE_CRED_RESOLVE_COOLING,   /* vaulted cred is rate-limited (see cooldown_secs) */
   DELEGATE_CRED_RESOLVE_POOL_EMPTY /* env pool configured but all leased/cooling */
} delegate_cred_resolve_status_t;

/* Resolve the credential for one delegate run. Restores persisted health, then:
 * vault-FIRST for `vault_principal` (a 429-cooling vaulted cred returns COOLING),
 * else leases from the agent's env pool. Writes target_agent->api_key on a hit,
 * names the (principal,agent,cred) row to release in leased_principal/_cred_name,
 * and fills credential_state_path. cooldown_secs is set for COOLING. */
delegate_cred_resolve_status_t delegate_resolve_credentials(
    const char *vault_principal, agent_t *target_agent, char *leased_principal,
    size_t leased_principal_cap, char *leased_cred_name, size_t leased_cred_name_cap,
    char *credential_state_path, size_t credential_state_path_cap, int *cooldown_secs);

int delegate_run_with_credential_retry(agent_config_t *cfg, agent_t *agent, const char *role,
                                       const char *system_prompt, const char *run_prompt,
                                       int max_tokens, int force_tools, int enforce_writes,
                                       char *leased_cred_name, size_t leased_cred_name_cap,
                                       const char *credential_state_path, agent_result_t *result);

#endif /* DEC_DELEGATE_CREDENTIAL_RETRY_H */
