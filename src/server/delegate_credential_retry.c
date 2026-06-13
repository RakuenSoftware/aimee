/* delegate_credential_retry.c: same-request retry within a credential pool. */

#include "delegate_credential_retry.h"

#include "agent_config.h" /* agent_set_request_codex_creds, *_token_present */
#include "agent_exec.h"
#include "config.h" /* config_output_dir */
#include "delegate_credentials.h"
#include "vault_service.h" /* vault_service_inject_api_key, VAULT_*_CRED */

#include <openssl/crypto.h> /* OPENSSL_cleanse */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* WP-C.3 codex-oauth migration: if `principal` has a vaulted Codex OAuth token for
 * `agent`, bind it as this turn's token so it OVERRIDES the on-disk/session token.
 * A fresh per-turn client push still wins (we defer when one is already bound).
 * The transient plaintext is cleansed; lifetime mirrors the existing per-turn
 * codex token (overwritten by the next turn's bind on this worker thread). */
static void codex_oauth_apply_vault_override(const char *principal, const agent_t *target_agent)
{
   if (!principal || !principal[0] || !target_agent)
      return;
   if (strcmp(target_agent->auth_type, "codex-oauth") != 0)
      return;
   if (agent_request_codex_token_present())
      return; /* a live client push is authoritative */
   char tok[MAX_API_KEY_LEN];
   if (vault_service_get(principal, target_agent->name, VAULT_CODEX_TOKEN_CRED, tok, sizeof(tok),
                         time(NULL)) != VAULT_OK)
      return;
   char acct[128] = "";
   (void)vault_service_get(principal, target_agent->name, VAULT_CODEX_ACCOUNT_CRED, acct,
                           sizeof(acct), time(NULL));
   agent_set_request_codex_creds(tok, acct[0] ? acct : NULL);
   OPENSSL_cleanse(tok, sizeof(tok));
   OPENSSL_cleanse(acct, sizeof(acct));
}

delegate_cred_resolve_status_t delegate_resolve_credentials(
    const char *vault_principal, agent_t *target_agent, char *leased_principal,
    size_t leased_principal_cap, char *leased_cred_name, size_t leased_cred_name_cap,
    char *credential_state_path, size_t credential_state_path_cap, int *cooldown_secs)
{
   if (cooldown_secs)
      *cooldown_secs = 0;
   if (!target_agent)
      return DELEGATE_CRED_RESOLVE_OK;

   /* Restore persisted per-credential cooldown/health (shared env pool AND
    * per-principal vault rows) before consulting either path. */
   const char *dir = config_output_dir();
   snprintf(credential_state_path, credential_state_path_cap, "%s/delegate-credential-state.tsv",
            dir ? dir : "/tmp");
   (void)delegate_credentials_load_file(credential_state_path, time(NULL));

   /* WP-C.1 vault-FIRST (D14/D15): a vaulted cred beats the env lease; LOCKED
    * fails the run; a miss falls through to the env pool. */
   if (vault_principal && vault_principal[0])
   {
      /* WP-C.3 per-principal 429 backpressure: a vaulted cred is a single key (no
       * round-robin / exclusive lease — a user's own key may serve their
       * concurrent delegates), but a prior 429 cools it for THIS principal only. */
      int cooling = delegate_credentials_cooldown_remaining(vault_principal, target_agent->name,
                                                            VAULT_API_KEY_CRED, time(NULL));
      if (cooling > 0)
      {
         if (cooldown_secs)
            *cooldown_secs = cooling;
         return DELEGATE_CRED_RESOLVE_COOLING;
      }
      vault_status_t vst = vault_service_inject_api_key(
          vault_principal, target_agent->name, target_agent->api_key, MAX_API_KEY_LEN, time(NULL));
      if (vst == VAULT_ERR_LOCKED)
         return DELEGATE_CRED_RESOLVE_LOCKED;
      if (vst == VAULT_OK)
      {
         /* Key the failure/cooldown row by (principal, agent, "api_key"). No
          * exclusive lease is taken, so release on exit is a harmless no-op. */
         snprintf(leased_principal, leased_principal_cap, "%s", vault_principal);
         snprintf(leased_cred_name, leased_cred_name_cap, "%s", VAULT_API_KEY_CRED);
         return DELEGATE_CRED_RESOLVE_OK;
      }
      /* miss -> codex override / env pool below */
   }

   /* WP-C.3 codex-oauth: a vaulted token overrides the on-disk/session token. */
   codex_oauth_apply_vault_override(vault_principal, target_agent);

   /* Lease one credential from the shared env pool (principal ""), if configured. */
   if (target_agent->credential_count > 0)
   {
      char leased_env[MAX_CRED_ENV_VAR_LEN] = "";
      if (delegate_credentials_acquire("", target_agent->name, target_agent->credentials,
                                       target_agent->credential_count, leased_cred_name,
                                       leased_cred_name_cap, leased_env, sizeof(leased_env)) != 0)
         return DELEGATE_CRED_RESOLVE_POOL_EMPTY;
      const char *value = getenv(leased_env);
      if (value && value[0])
         snprintf(target_agent->api_key, MAX_API_KEY_LEN, "%s", value);
   }
   return DELEGATE_CRED_RESOLVE_OK;
}

static int run_delegate_attempt(agent_config_t *cfg, const char *role, const char *system_prompt,
                                const char *run_prompt, int max_tokens, int force_tools,
                                int enforce_writes, agent_result_t *result)
{
   if (force_tools)
      return agent_run_with_tools_write_enforce(cfg, role, system_prompt, run_prompt, max_tokens,
                                                enforce_writes, result);
   return agent_run(cfg, role, system_prompt, run_prompt, max_tokens, result);
}

static int result_failed_for_pool(const agent_result_t *result, int rc)
{
   return rc != 0 || !result || !result->success;
}

static void result_add_prior_metrics(agent_result_t *result, const agent_result_t *prior)
{
   if (!result || !prior)
      return;
   result->turns += prior->turns;
   result->tool_calls += prior->tool_calls;
   result->prompt_tokens += prior->prompt_tokens;
   result->completion_tokens += prior->completion_tokens;
   result->latency_ms += prior->latency_ms;
}

int delegate_run_with_credential_retry(agent_config_t *cfg, agent_t *agent, const char *role,
                                       const char *system_prompt, const char *run_prompt,
                                       int max_tokens, int force_tools, int enforce_writes,
                                       char *leased_cred_name, size_t leased_cred_name_cap,
                                       const char *credential_state_path, agent_result_t *result)
{
   if (!cfg || !result)
      return -1;

   int rc = run_delegate_attempt(cfg, role, system_prompt, run_prompt, max_tokens, force_tools,
                                 enforce_writes, result);
   if (!agent || agent->credential_count <= 1 || !leased_cred_name || !leased_cred_name[0])
      return rc;

   for (int attempt = 1; attempt < agent->credential_count && result_failed_for_pool(result, rc);
        attempt++)
   {
      agent_result_t prior = *result;
      char next_env[MAX_CRED_ENV_VAR_LEN] = "";
      /* Env-pool rotation only (credential_count > 1); the shared "" principal. */
      int rotated = delegate_credentials_rotate_after_failure(
          "", agent->name, agent->credentials, agent->credential_count, agent->provider,
          leased_cred_name, leased_cred_name_cap, next_env, sizeof(next_env), result->error,
          time(NULL));
      if (rotated != 1)
         break;
      if (credential_state_path && credential_state_path[0])
         (void)delegate_credentials_save_file(credential_state_path);

      const char *value = getenv(next_env);
      agent->api_key[0] = '\0';
      if (value && value[0])
         snprintf(agent->api_key, sizeof(agent->api_key), "%s", value);

      memset(result, 0, sizeof(*result));
      rc = run_delegate_attempt(cfg, role, system_prompt, run_prompt, max_tokens, force_tools,
                                enforce_writes, result);
      result_add_prior_metrics(result, &prior);
      free(prior.response);
   }

   return rc;
}
