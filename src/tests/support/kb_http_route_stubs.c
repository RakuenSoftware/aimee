/* Link-only stubs for routes outside test_kb_http_routes.c's focused surface. */
#include "kb_http_budget.h"
#include "kb_http_insights.h"
#include "kb_http_models.h"
#include "kb_http_rate.h"
#include "kb/http/kb_http_servers.h"
#include "kb_http_telemetry.h"
#include "modules/db2/c/server_registry.h"
#include "modules/db2/c/evidence_lifecycle.h"
#include "modules/db2/c/entity_registry.h"
#include "modules/db2/c/management_identity_journal.h"
#include "modules/db2/c/write_tier_grant.h"
#include "kb_oidc_token_exchange.h"
#include "vault_service.h"
#include <stdio.h>
#include <string.h>

/* kb_tls_serve links the primary-authoritative per-request enrollment seam.
 * Route tests do not provision DB2 enrollment state, so model an active peer. */
static int g_enrollment_authority = 1;

void test_kb_enrollment_authority_set(int status)
{
   g_enrollment_authority = status;
}

int db2_enrollment_is_active_by_key(const char *cert_issuer, const char *cert_serial_norm)
{
   (void)cert_issuer;
   (void)cert_serial_norm;
   return g_enrollment_authority;
}

int kb_http_telemetry_token_route(const char *method, const char *path, const char *query_string,
                                  const char *body, const char *presented, char *out_buf,
                                  int out_cap)
{
   (void)method;
   (void)path;
   (void)query_string;
   (void)body;
   (void)presented;
   (void)out_buf;
   (void)out_cap;
   return -1;
}

int kb_http_models_route(const char *method, const char *path, const char *body, char *out_buf,
                         int out_cap)
{
   (void)method;
   (void)path;
   (void)body;
   (void)out_buf;
   (void)out_cap;
   return -1;
}

int kb_http_insights_route(const char *method, const char *path, const char *query_string,
                           char *out_buf, int out_cap)
{
   (void)method;
   (void)path;
   (void)query_string;
   (void)out_buf;
   (void)out_cap;
   return -1;
}

int kb_http_budget_route(const char *method, const char *path, const char *query_string,
                         const char *body, char *out_buf, int out_cap)
{
   (void)method;
   (void)path;
   (void)query_string;
   (void)body;
   (void)out_buf;
   (void)out_cap;
   return -1;
}

int kb_http_rate_route(const char *method, const char *path, const char *query_string,
                       const char *body, char *out_buf, int out_cap)
{
   (void)method;
   (void)path;
   (void)query_string;
   (void)body;
   (void)out_buf;
   (void)out_cap;
   return -1;
}

int kb_http_telemetry_route(const char *method, const char *path, const char *query_string,
                            const char *body, char *out_buf, int out_cap)
{
   (void)method;
   (void)path;
   (void)query_string;
   (void)body;
   (void)out_buf;
   (void)out_cap;
   return -1;
}

int kb_http_servers_route(const char *method, const char *path, const char *query_string,
                          char *out_buf, int out_cap)
{
   (void)method;
   (void)path;
   (void)query_string;
   (void)out_buf;
   (void)out_cap;
   return -1;
}

int kb_http_servers_route_ex(const char *method, const char *path, const char *query_string,
                             const char *body, size_t body_len, char *out_buf, int out_cap)
{
   (void)body;
   (void)body_len;
   return kb_http_servers_route(method, path, query_string, out_buf, out_cap);
}

int g_test_registry_heartbeat_allow;
char g_test_registry_server_id[128], g_test_registry_issuer[601], g_test_registry_serial[129],
    g_test_registry_fingerprint[65];

int db2_server_registry_heartbeat(const char *server_id, const char *issuer, const char *serial,
                                  const char *fingerprint, const char *health, const char *version)
{
   snprintf(g_test_registry_server_id, sizeof(g_test_registry_server_id), "%s", server_id);
   snprintf(g_test_registry_issuer, sizeof(g_test_registry_issuer), "%s", issuer);
   snprintf(g_test_registry_serial, sizeof(g_test_registry_serial), "%s", serial);
   snprintf(g_test_registry_fingerprint, sizeof(g_test_registry_fingerprint), "%s", fingerprint);
   (void)health;
   (void)version;
   return g_test_registry_heartbeat_allow ? 0 : -1;
}

int g_test_registry_client_match = 1;

int db2_server_registry_client_match(const char *server_id, int64_t team, const char *issuer,
                                     const char *serial, const char *fingerprint)
{
   snprintf(g_test_registry_server_id, sizeof(g_test_registry_server_id), "%s", server_id);
   snprintf(g_test_registry_issuer, sizeof(g_test_registry_issuer), "%s", issuer);
   snprintf(g_test_registry_serial, sizeof(g_test_registry_serial), "%s", serial);
   snprintf(g_test_registry_fingerprint, sizeof(g_test_registry_fingerprint), "%s", fingerprint);
   (void)team;
   return g_test_registry_client_match;
}

/* The OIDC login callback's two outward dependencies. This test's focus is
 * routing, and it never drives a login to completion, so a stub that REFUSES is
 * both sufficient and the safer default: if the callback is ever reached from
 * here by accident, it fails closed rather than proceeding with a fabricated
 * secret. The callback's own behaviour is tested in
 * test_kb_http_identity_login.c, which stubs these to succeed.
 *
 * Linking the real ones instead would pull the vault and the TLS client into a
 * routing test — and kb_oidc_token_exchange_post lives in its own translation
 * unit specifically so that is avoidable. */
typedef struct
{
   char cred[96];
   char *registry;
} test_enroll_vault_record_t;

static test_enroll_vault_record_t g_test_enroll_vault[8];

vault_status_t vault_service_get_server_principal(const char *agent, const char *cred, char *out,
                                                  size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   if (agent && cred && strcmp(agent, "kb-enrollment") == 0)
      for (size_t i = 0; i < sizeof(g_test_enroll_vault) / sizeof(g_test_enroll_vault[0]); i++)
         if (g_test_enroll_vault[i].registry && strcmp(g_test_enroll_vault[i].cred, cred) == 0)
         {
            if (!out || strlen(g_test_enroll_vault[i].registry) >= cap)
               return VAULT_ERR_BADARG;
            snprintf(out, cap, "%s", g_test_enroll_vault[i].registry);
            return VAULT_OK;
         }
   return VAULT_NO_ENTRY;
}

vault_status_t vault_service_set_server(const char *agent, const char *cred, const char *secret)
{
   if (!agent || !cred || !secret || strcmp(agent, "kb-enrollment") != 0)
      return VAULT_ERR_BADARG;
   test_enroll_vault_record_t *slot = NULL;
   for (size_t i = 0; i < sizeof(g_test_enroll_vault) / sizeof(g_test_enroll_vault[0]); i++)
      if (g_test_enroll_vault[i].registry && strcmp(g_test_enroll_vault[i].cred, cred) == 0)
         slot = &g_test_enroll_vault[i];
      else if (!slot && !g_test_enroll_vault[i].registry)
         slot = &g_test_enroll_vault[i];
   if (!slot)
      return VAULT_ERR_IO;
   char *copy = strdup(secret);
   if (!copy)
      return VAULT_ERR_IO;
   free(slot->registry);
   slot->registry = copy;
   snprintf(slot->cred, sizeof(slot->cred), "%s", cred);
   return VAULT_OK;
}

kb_oidc_token_exchange_result_t
kb_oidc_token_exchange_post(const kb_oidc_login_config_t *cfg,
                            const kb_oidc_login_pending_t *pending, const char *code,
                            const char *client_secret, char *unverified_id_token_out, size_t cap)
{
   (void)cfg;
   (void)pending;
   (void)code;
   (void)client_secret;
   if (unverified_id_token_out && cap)
      unverified_id_token_out[0] = '\0';
   return KB_OIDC_TOKEN_EXCHANGE_UNAVAILABLE;
}

/* The identity-intent seam. Refusing stubs, for the same reason as the two above:
 * this test's focus is routing, it never drives a login to completion, and a
 * refusing default means an accidental path through the login routes fails closed
 * rather than proceeding against a fabricated authority. The real behaviour is
 * covered by test_kb_http_identity_login.c (route ordering) and the P1 RLS gate
 * plus scripts/run-identity-mint-e2e.sh (the SQL and the mint). */
db2_management_action_result_t db2_identity_login_context(const kb_principal_t *principal,
                                                          int64_t team_id, char installation_id[33],
                                                          char kid[DB2_IDENTITY_KID_MAX + 1])
{
   (void)principal;
   (void)team_id;
   if (installation_id)
      installation_id[0] = '\0';
   if (kid)
      kid[0] = '\0';
   return DB2_MANAGEMENT_ACTION_UNAVAILABLE;
}

db2_management_action_result_t
db2_identity_intent_operation_init(int64_t team_id, const char *target_server_id,
                                   db2_identity_auth_mode_t auth_mode, const char *token_issuer,
                                   const char *kid, int ttl_seconds, const char *installation_id,
                                   db2_identity_intent_operation_t *out)
{
   (void)team_id;
   (void)target_server_id;
   (void)auth_mode;
   (void)token_issuer;
   (void)kid;
   (void)ttl_seconds;
   (void)installation_id;
   if (out)
      memset(out, 0, sizeof(*out));
   return DB2_MANAGEMENT_ACTION_UNAVAILABLE;
}

db2_management_action_result_t db2_identity_intent_start(const kb_principal_t *principal,
                                                         const db2_identity_intent_operation_t *op,
                                                         db2_identity_intent_t *out)
{
   (void)principal;
   (void)op;
   if (out)
      memset(out, 0, sizeof(*out));
   return DB2_MANAGEMENT_ACTION_UNAVAILABLE;
}

/* The write-tier grant seam. Refusing stubs, like the others above: this test's focus is
 * routing, it never administers a grant, and a refusing default means an accidental path
 * through those routes fails closed rather than proceeding against a fabricated table.
 * The real behaviour is covered by test_kb_http_grants.c (routing and validation) and the
 * P1 RLS gate (the SQL and its authorization). */
int db2_write_tier_grant_set_reporting(const char *server_id, int64_t team_id, const char *subject,
                                       kb_identity_tier_t tier, const char *granted_by,
                                       db2_write_tier_grant_report_t *out)
{
   (void)server_id;
   (void)team_id;
   (void)subject;
   (void)tier;
   (void)granted_by;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int db2_write_tier_grant_revoke(const char *server_id, int64_t team_id, const char *subject)
{
   (void)server_id;
   (void)team_id;
   (void)subject;
   return -1;
}

int db2_write_tier_grant_list_ex(const char *server_id, int64_t team_id, int include_revoked,
                                 const char *subject, db2_write_tier_grant_row_t *out, size_t cap,
                                 size_t *count)
{
   (void)server_id;
   (void)team_id;
   (void)include_revoked;
   (void)subject;
   (void)out;
   (void)cap;
   if (count)
      *count = 0;
   return -1;
}

/* The revoke route now derives `found` from an exact lookup rather than scanning a listing
 * (a review found the scan reported found:false for a subject sorting beyond the row cap).
 * Refusing stub, like the others here: this test never revokes a grant. */
int db2_write_tier_grant_lookup(const char *server_id, int64_t team_id, const char *subject,
                                kb_identity_tier_t *out)
{
   (void)server_id;
   (void)team_id;
   (void)subject;
   (void)out;
   return -1;
}

/* Typed-fact console storage seams.  The focused HTTP route suite does not open
 * DB2.  Keep reads empty and mutations fail-closed; lifecycle/entity integration
 * is exercised by its dedicated DB2 tests and the real full-stack browser E2E. */
int db2_fact_candidates(fact_candidate_t *out, int max)
{
   (void)out;
   (void)max;
   return 0;
}

int db2_entity_summaries(entity_summary_t *out, int max)
{
   (void)out;
   (void)max;
   return 0;
}

int db2_entity_merge_summaries(entity_merge_summary_t *out, int max)
{
   (void)out;
   (void)max;
   return 0;
}

static int g_fact_actor_enabled;

void test_kb_fact_actor_set(int enabled)
{
   g_fact_actor_enabled = enabled;
}

int db2_fact_actor_from_request(int require_operator, fact_actor_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!g_fact_actor_enabled || !out)
      return -1;
   snprintf(out->principal, sizeof(out->principal), "test:operator");
   snprintf(out->role, sizeof(out->role), "%s", require_operator ? "operator" : "user");
   snprintf(out->transport_identity, sizeof(out->transport_identity), "test-transport");
   out->rank = require_operator ? FACT_ACTOR_OPERATOR : FACT_ACTOR_USER;
   out->authenticated = 1;
   return 0;
}

int db2_evidence_lifecycle_json(const fact_actor_t *actor, evidence_lifecycle_op_t op,
                                const char *const *args, int nargs, char *out, int out_cap)
{
   (void)actor;
   (void)op;
   (void)args;
   (void)nargs;
   if (out && out_cap > 0)
      snprintf(out, (size_t)out_cap, "{\"ok\":true}");
   return 0;
}

int db2_fact_mutation_review(const fact_actor_t *actor, int64_t assertion_id,
                             fact_review_action_t action, fact_mutation_result_t *out)
{
   (void)actor;
   (void)assertion_id;
   (void)action;
   (void)out;
   return -1;
}

int64_t db2_entity_merge_as(const fact_actor_t *actor, int64_t from_id, int64_t into_id,
                            char commit_id[FACT_COMMIT_ID_MAX])
{
   (void)actor;
   (void)from_id;
   (void)into_id;
   if (commit_id)
      commit_id[0] = '\0';
   return -1;
}

int db2_entity_unmerge_as(const fact_actor_t *actor, int64_t merge_id,
                          char commit_id[FACT_COMMIT_ID_MAX])
{
   (void)actor;
   (void)merge_id;
   if (commit_id)
      commit_id[0] = '\0';
   return -1;
}

int db2_fact_commit_preview(const char *commit_id, fact_commit_change_t *out, int max)
{
   (void)commit_id;
   (void)out;
   (void)max;
   return -1;
}

int db2_fact_commit_rollback(const fact_actor_t *actor, const char *commit_id,
                             char rollback_commit_id[FACT_COMMIT_ID_MAX])
{
   (void)actor;
   (void)commit_id;
   if (rollback_commit_id)
      rollback_commit_id[0] = '\0';
   return -1;
}

int db2_fact_ingest_run_preview(const char *ingest_run_id, fact_commit_change_t *out, int max)
{
   (void)ingest_run_id;
   (void)out;
   (void)max;
   return -1;
}

int db2_fact_ingest_run_rollback(const fact_actor_t *actor, const char *ingest_run_id,
                                 char rollback_commit_id[FACT_COMMIT_ID_MAX])
{
   (void)actor;
   (void)ingest_run_id;
   if (rollback_commit_id)
      rollback_commit_id[0] = '\0';
   return -1;
}

int db2_fact_erasure_preview(const char *source, const char *relation, const char *target,
                             fact_erasure_impact_t *out)
{
   (void)source;
   (void)relation;
   (void)target;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int db2_fact_erasure_execute(const fact_actor_t *actor, const char *source, const char *relation,
                             const char *target, fact_erasure_impact_t *out,
                             char commit_id[FACT_COMMIT_ID_MAX])
{
   (void)actor;
   (void)source;
   (void)relation;
   (void)target;
   if (out)
      memset(out, 0, sizeof(*out));
   if (commit_id)
      commit_id[0] = '\0';
   return -1;
}
