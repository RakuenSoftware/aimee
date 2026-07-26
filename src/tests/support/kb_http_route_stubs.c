/* Link-only stubs for routes outside test_kb_http_routes.c's focused surface. */
#include "kb_http_budget.h"
#include "kb_http_insights.h"
#include "kb_http_models.h"
#include "kb_http_rate.h"
#include "kb/http/kb_http_servers.h"
#include "kb_http_telemetry.h"
#include "db2/server_registry.h"
#include "db2/management_identity_journal.h"
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
vault_status_t vault_service_get_server_principal(const char *agent, const char *cred, char *out,
                                                  size_t cap)
{
   (void)agent;
   (void)cred;
   if (out && cap)
      out[0] = '\0';
   return VAULT_NO_ENTRY;
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
