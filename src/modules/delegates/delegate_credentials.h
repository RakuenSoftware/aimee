/* delegate_credentials.h: per-provider credential lease pool.
 *
 * Two sibling delegates routed to the same provider used to share one
 * Bearer token; a 429 on one poisoned the sibling. With this module,
 * each delegate spawn calls _acquire on entry and _release on exit;
 * available credentials are picked round-robin and a 429 cools only
 * the leased credential, not the whole agent.
 *
 * Process-global lease state, mutex-protected. Empty agent.credentials
 * (the default) is treated as "no pool — fall through to api_key", so
 * existing single-token agents are unaffected.
 *
 * WP-C.3: every row is keyed by (principal, agent, cred). The shared
 * agents.json env-var pool uses an EMPTY principal ("") — a 429 there is
 * global and cools the credential for everyone, exactly as before. A
 * per-principal VAULT credential passes its `uid:`/`webuser:` principal, so
 * one principal's 429-cooldown and health are isolated from another's even
 * when they vault the same agent. A NULL principal is normalised to "". */
#ifndef DEC_DELEGATE_CREDENTIALS_H
#define DEC_DELEGATE_CREDENTIALS_H 1

#include <stddef.h>
#include <time.h>

#include "aimee.h" /* MAX_PATH_LEN — agent_types.h uses it without including */
#include "agent_types.h"
#include "failover.h"
#include "vault_principal.h" /* VAULT_PRINCIPAL_MAX — pool rows are keyed by principal */

#ifdef __cplusplus
extern "C"
{
#endif

   /* Try to lease a credential from the pool. Returns 0 on success and
    * writes the chosen credential's name + api_key_env env-var string
    * into the out buffers. Returns -1 if no credential is available
    * (all leased / all cooling), or if the agent has no credential pool
    * (credential_count == 0).
    *
    * Callers should fall back to the agent's single api_key when this
    * returns -1 with credential_count == 0; an error return with
    * credential_count > 0 means every credential is in use or cooling
    * and the caller should retry after a backoff. */
   int delegate_credentials_acquire(const char *principal, const char *agent_name,
                                    const agent_credential_t *creds, int credential_count,
                                    char *out_name, size_t out_name_cap, char *out_env_var,
                                    size_t out_env_var_cap);

   /* Release a previously-acquired lease so siblings can pick it up.
    * No-op for unknown (principal, agent_name, cred_name) rows. */
   void delegate_credentials_release(const char *principal, const char *agent_name,
                                     const char *cred_name);

   /* Mark a credential as cooling for `seconds` seconds. Used after a 429
    * so siblings skip the entry until the cooldown elapses. The
    * credential remains leased until the caller also calls _release. */
   void delegate_credentials_cooldown(const char *principal, const char *agent_name,
                                      const char *cred_name, int seconds);

   /* Read-only: seconds remaining on the (principal, agent, cred) cooldown, or 0
    * if the credential is unknown or not cooling. The vault use-path consults
    * this to apply per-principal 429 backpressure to a single vaulted credential
    * without taking an exclusive lease (a user's own key may be used by their
    * concurrent delegates). Creates no row. */
   int delegate_credentials_cooldown_remaining(const char *principal, const char *agent_name,
                                               const char *cred_name, time_t now_epoch);

   /* Default cooldown applied when a delegate run fails with a rate-
    * limit / 429 / overloaded error and a credential lease was held.
    * 60s matches the typical OpenAI / Anthropic Retry-After hint and is
    * short enough that the credential rejoins the pool before the next
    * delegate cycle in normal operator use. */
#define DELEGATE_CRED_COOLDOWN_SECONDS_DEFAULT 60

   /* Returns 1 when `error` looks like an upstream rate-limit / overload
    * the credential pool should treat as transient (the leased entry
    * gets cooled, siblings skip it until cooldown elapses). Returns 0
    * for empty / NULL / unrelated errors.
    *
    * Mirrors agent_runtime.c's internal classifier — duplicated here so
    * the lease pool does not pull in the agent runtime, and so a server
    * unit test can pin the exact set of strings that trigger cooldown. */
   int delegate_credentials_is_rate_limit(const char *error);

   typedef enum
   {
      DELEGATE_CRED_STATUS_OK = 0,
      DELEGATE_CRED_STATUS_RATE_LIMITED,
      DELEGATE_CRED_STATUS_EXHAUSTED,
      DELEGATE_CRED_STATUS_AUTH_FAILED
   } delegate_credential_status_t;

   typedef struct delegate_ratelimit_state
   {
      int req_limit;
      int req_remaining;
      time_t req_reset;
      int req_limit_1h;
      int req_remaining_1h;
      time_t req_reset_1h;
      long tok_limit;
      long tok_remaining;
      time_t tok_reset;
      long tok_limit_1h;
      long tok_remaining_1h;
      time_t tok_reset_1h;
      time_t captured_at;
   } delegate_ratelimit_state_t;

   typedef struct delegate_credential_snapshot
   {
      char agent_name[MAX_AGENT_NAME];
      char cred_name[MAX_CRED_NAME_LEN];
      delegate_credential_status_t status;
      int leased;
      time_t cooldown_until;
      char last_error[128];
      delegate_ratelimit_state_t rl;
   } delegate_credential_snapshot_t;

   const char *delegate_credential_status_name(delegate_credential_status_t status);

   /* Parse provider x-ratelimit-* response headers for the acting credential.
    * raw_headers is a CRLF/LF-separated header block. Reset values are relative
    * seconds and are stored as absolute epoch seconds using now_epoch. */
   int delegate_credentials_capture_headers(const char *principal, const char *agent_name,
                                            const char *cred_name, const char *raw_headers,
                                            time_t now_epoch);

   /* Mark a credential according to the failover classifier. Returns 1 when
    * the reason changed credential health, 0 for unrelated reasons. */
   int delegate_credentials_report_failure(const char *principal, const char *agent_name,
                                           const char *cred_name, failover_reason_t reason,
                                           const char *error, time_t now_epoch);

   failover_reason_t delegate_credentials_classify_failure(const char *provider, const char *error);

   /* Mark the current leased credential failed, release it, and try to lease
    * the next available member from the same pool. Returns 1 when a new
    * credential was leased, 0 when the error is not pool-rotatable or no
    * member is available, and -1 for invalid arguments. On rotatable failures
    * the old lease is always released, even when no replacement is available.
    * Env-pool rotation only — callers pass the shared "" principal. */
   int delegate_credentials_rotate_after_failure(const char *principal, const char *agent_name,
                                                 const agent_credential_t *creds,
                                                 int credential_count, const char *provider,
                                                 char *leased_cred_name,
                                                 size_t leased_cred_name_cap, char *out_env_var,
                                                 size_t out_env_var_cap, const char *error,
                                                 time_t now_epoch);

   /* Operator-facing health snapshot of the SHARED env pool (principal == "").
    * Per-principal vault rows are intentionally not surfaced here (they are
    * per-user, not operator-configured). */
   int delegate_credentials_snapshot(const char *agent_name, delegate_credential_snapshot_t *out,
                                     int max);

   int delegate_credentials_format_quota(const char *agent_name, char *buf, size_t buf_len);

   /* Persist/reload process-local credential health. Leases are intentionally
    * not persisted; on load every row starts unleased, while cooldown/status
    * and captured rate-limit windows survive process restart. */
   int delegate_credentials_save_file(const char *path);
   int delegate_credentials_load_file(const char *path, time_t now_epoch);

   /* Test seam: clear all per-process lease + cooldown state. Tests
    * call this between cases so prior leases do not leak. */
   void delegate_credentials_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DELEGATE_CREDENTIALS_H */
