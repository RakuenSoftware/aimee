/* delegate_credential_retry.c: same-request retry within a credential pool. */

#include <aimee/delegates/delegate_credential_retry.h>

#include "agent_config.h" /* agent_set_request_codex_creds, *_token_present */
#include "agent_exec.h"
#include "config.h" /* config_output_dir */
#include <aimee/delegates/delegate_credentials.h>
#include "runtime_secret.h"
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
   if (!target_agent)
      return;
   if (strcmp(target_agent->auth_type, "codex-oauth") != 0)
      return;
   if (agent_request_codex_token_present())
      return; /* a live client push is authoritative */

   char tok[MAX_API_KEY_LEN] = "";
   char acct[128] = "";
   vault_status_t st = VAULT_NO_ENTRY;

   (void)principal;
   st = vault_service_get_server_principal(target_agent->name, VAULT_CODEX_TOKEN_CRED, tok,
                                           sizeof(tok));
   if (st == VAULT_OK)
      (void)vault_service_get_server_principal(target_agent->name, VAULT_CODEX_ACCOUNT_CRED, acct,
                                               sizeof(acct));

   if (st != VAULT_OK || !tok[0])
      return;
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

   /* Restore persisted per-credential cooldown/health before consulting either path. */
   const char *dir = config_output_dir();
   snprintf(credential_state_path, credential_state_path_cap, "%s/delegate-credential-state.tsv",
            dir ? dir : "/tmp");
   (void)delegate_credentials_load_file(credential_state_path, time(NULL));

   /* The environment credential beats the shared named pool. PAM identity is
    * never a credential or cooldown namespace. */
   {
      int cooling = delegate_credentials_cooldown_remaining(
          VAULT_SERVER_PRINCIPAL, target_agent->name, VAULT_API_KEY_CRED, time(NULL));
      if (cooling > 0)
      {
         if (cooldown_secs)
            *cooldown_secs = cooling;
         return DELEGATE_CRED_RESOLVE_COOLING;
      }
      vault_status_t vst =
          vault_service_inject_api_key(VAULT_SERVER_PRINCIPAL, target_agent->name,
                                       target_agent->api_key, MAX_API_KEY_LEN, time(NULL));
      if (vst == VAULT_ERR_LOCKED)
         return DELEGATE_CRED_RESOLVE_LOCKED;
      if (vst == VAULT_OK)
      {
         /* Key failure/cooldown by the one environment principal. */
         snprintf(leased_principal, leased_principal_cap, "%s", VAULT_SERVER_PRINCIPAL);
         snprintf(leased_cred_name, leased_cred_name_cap, "%s", VAULT_API_KEY_CRED);
         return DELEGATE_CRED_RESOLVE_OK;
      }
      /* miss -> codex override / shared named pool below */
   }

   /* WP-C.3 codex-oauth: a vaulted token overrides the on-disk/session token. */
   codex_oauth_apply_vault_override(vault_principal, target_agent);

   /* Lease one credential from the shared Vault-backed pool (principal ""), if
    * configured. api_key_env is a legacy slot name, never a runtime env read. */
   if (target_agent->credential_count > 0)
   {
      char leased_env[MAX_CRED_ENV_VAR_LEN] = "";
      if (delegate_credentials_acquire("", target_agent->name, target_agent->credentials,
                                       target_agent->credential_count, leased_cred_name,
                                       leased_cred_name_cap, leased_env, sizeof(leased_env)) != 0)
         return DELEGATE_CRED_RESOLVE_POOL_EMPTY;
      char value[MAX_API_KEY_LEN] = "";
      if (runtime_secret_get(leased_env, value, sizeof(value)) && value[0])
         snprintf(target_agent->api_key, MAX_API_KEY_LEN, "%s", value);
      OPENSSL_cleanse(value, sizeof(value));
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
   /* The delegate decided tools-OFF (e.g. CLI --no-tools -> force_tools=0). Force
    * it for the turn: agent_run_ex would otherwise re-derive use_tools from the
    * agent's config (tools_enabled + exec-role) and silently re-enable tools,
    * which makes an agentic model (e.g. codex) loop on tool calls and return no
    * text instead of the requested single-shot answer. */
   agent_run_force_no_tools(1);
   int rc = agent_run(cfg, role, system_prompt, run_prompt, max_tokens, result);
   agent_run_force_no_tools(0);
   return rc;
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

      char value[MAX_API_KEY_LEN] = "";
      agent->api_key[0] = '\0';
      if (runtime_secret_get(next_env, value, sizeof(value)) && value[0])
         snprintf(agent->api_key, sizeof(agent->api_key), "%s", value);
      OPENSSL_cleanse(value, sizeof(value));

      memset(result, 0, sizeof(*result));
      rc = run_delegate_attempt(cfg, role, system_prompt, run_prompt, max_tokens, force_tools,
                                enforce_writes, result);
      result_add_prior_metrics(result, &prior);
      free(prior.response);
   }

   return rc;
}
