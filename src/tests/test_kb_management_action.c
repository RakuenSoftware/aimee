#include "kb/kb_management_action.h"
#include "json_wire.h"
#include "kb_mgmt_status_authority.h"

#include <assert.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
   int stage, intents, snapshots, opens, requests, authority, tokens, outcomes, closes, clears;
   int ambiguous_intent, ambiguous_outcome, replay, invalid_active, newer_staple;
   kb_mgmt_token_authority_ipc_result_t token_result;
   kb_management_action_transport_t action_transport;
   const char *response;
   int response_status;
   uint64_t wall, mono;
   unsigned char sk[32], pk[32];
   db2_management_action_outcome_operation_t outcome;
} fixture_t;

static db2_management_action_result_t
operation_init(int64_t team, const char *server, db2_management_action_capability_t cap,
               const uint8_t digest[32], const char *issuer, const char *kid, int ttl,
               const char *installation, db2_management_action_operation_t *out)
{
   assert(team == 7 && !strcmp(server, "srv-1") && cap == DB2_MANAGEMENT_ACTION_CAP_REMOTE_WRITES);
   memset(out, 0, sizeof(*out));
   memset(out->correlation_id, '1', 64);
   memset(out->jti, '2', 64);
   out->team_id = team;
   out->capability = cap;
   snprintf(out->target_server_id, sizeof(out->target_server_id), "%s", server);
   static const char h[] = "0123456789abcdef";
   for (size_t i = 0; i < 32; i++)
   {
      out->request_sha256[i * 2] = h[digest[i] >> 4];
      out->request_sha256[i * 2 + 1] = h[digest[i] & 15];
   }
   snprintf(out->token_issuer, sizeof(out->token_issuer), "%s", issuer);
   snprintf(out->kid, sizeof(out->kid), "%s", kid);
   out->ttl_seconds = ttl;
   snprintf(out->installation_id, sizeof(out->installation_id), "%s", installation);
   return DB2_MANAGEMENT_ACTION_OK;
}

static fixture_t *g;

static db2_management_action_result_t intent(const kb_principal_t *actor,
                                             const db2_management_action_operation_t *op,
                                             db2_management_action_intent_t *out)
{
   assert(actor && actor->authenticated && g->stage == 0);
   g->intents++;
   if (g->ambiguous_intent && g->intents == 1)
      return DB2_MANAGEMENT_ACTION_COMMIT_AMBIGUOUS;
   g->stage = 1;
   memset(out, 0, sizeof(*out));
   out->replayed = g->replay;
   memcpy(out->correlation_id, op->correlation_id, 65);
   memcpy(out->jti, op->jti, 65);
   out->team_id = op->team_id;
   out->capability = op->capability;
   snprintf(out->target_server_id, sizeof(out->target_server_id), "%s", op->target_server_id);
   snprintf(out->request_sha256, sizeof(out->request_sha256), "%s", op->request_sha256);
   snprintf(out->token_issuer, sizeof(out->token_issuer), "%s", op->token_issuer);
   snprintf(out->audience, sizeof(out->audience), "%s", op->target_server_id);
   snprintf(out->kid, sizeof(out->kid), "%s", op->kid);
   out->issued_at = 1000;
   out->expires_at = 1090;
   snprintf(out->target_mgmt_issuer, sizeof(out->target_mgmt_issuer), "CN=server-ca");
   snprintf(out->target_mgmt_serial_norm, sizeof(out->target_mgmt_serial_norm), "1a");
   memset(out->target_mgmt_fingerprint, 'b', 64);
   out->revocation_generation = 3;
   snprintf(out->installation_id, sizeof(out->installation_id), "%s", op->installation_id);
   out->installation_generation = 4;
   out->installation_enrollment_id = 5;
   snprintf(out->local_cert_issuer, sizeof(out->local_cert_issuer), "CN=client-ca");
   snprintf(out->local_cert_serial_norm, sizeof(out->local_cert_serial_norm), "2b");
   memset(out->local_cert_fingerprint, 'a', 64);
   return DB2_MANAGEMENT_ACTION_OK;
}

static db2_management_action_result_t outcome(const kb_principal_t *actor,
                                              const db2_management_action_outcome_operation_t *op,
                                              db2_management_action_outcome_t *out)
{
   assert(actor && op && out && g->stage >= 1);
   g->outcomes++;
   g->outcome = *op;
   if (g->ambiguous_outcome && g->outcomes == 1)
      return DB2_MANAGEMENT_ACTION_COMMIT_AMBIGUOUS;
   memset(out, 0, sizeof(*out));
   return DB2_MANAGEMENT_ACTION_OK;
}

static kb_management_health_result_t snapshot(void *ctx, const kb_principal_t *actor, int64_t team,
                                              const char *server, db2_server_snapshot_t *out)
{
   fixture_t *f = ctx;
   assert(actor && team == 7 && !strcmp(server, "srv-1") && f->stage >= 1 && f->stage < 5);
   f->snapshots++;
   f->stage = f->snapshots == 1 ? 2 : 5;
   memset(out, 0, sizeof(*out));
   snprintf(out->server_id, sizeof(out->server_id), "srv-1");
   snprintf(out->endpoint, sizeof(out->endpoint), "https://8.8.8.8");
   snprintf(out->status, sizeof(out->status), "active");
   snprintf(out->enrollment_state, sizeof(out->enrollment_state), "active");
   snprintf(out->management_issuer, sizeof(out->management_issuer), "CN=server-ca");
   snprintf(out->management_serial_norm, sizeof(out->management_serial_norm), "1a");
   memset(out->management_fingerprint, 'b', 64);
   out->revocation_generation = 3;
   return KB_MANAGEMENT_HEALTH_OK;
}

static kb_management_health_result_t bundle(void *ctx, kb_management_cert_bundle_t *b,
                                            kb_management_cert_active_t *a)
{
   fixture_t *f = ctx;
   assert(f->stage == 2);
   f->stage = 3;
   memset(b, 0, sizeof(*b));
   b->leaf_pem_len = b->key_pem_len = 1;
   memset(a, 0, sizeof(*a));
   snprintf(a->installation_id, sizeof(a->installation_id), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
   a->generation = 4;
   a->enrollment_id = 5;
   a->not_before_epoch = 900;
   a->not_after_epoch = 1100;
   a->revocation_generation = 3;
   snprintf(a->issuer, sizeof(a->issuer), "CN=client-ca");
   snprintf(a->serial_norm, sizeof(a->serial_norm), "2b");
   memset(a->fingerprint, 0xaa, 32);
   if (f->invalid_active)
      a->generation++;
   return KB_MANAGEMENT_HEALTH_OK;
}

static void clear(void *ctx, kb_management_cert_bundle_t *b)
{
   ((fixture_t *)ctx)->clears++;
   memset(b, 0, sizeof(*b));
}

static kb_management_health_result_t open_server(void *ctx, const db2_server_snapshot_t *s,
                                                 const kb_management_cert_bundle_t *b,
                                                 uint64_t deadline, void **out)
{
   fixture_t *f = ctx;
   assert(f->stage == 3 && s && b && deadline == 9000);
   f->stage = 4;
   f->opens++;
   *out = f;
   return KB_MANAGEMENT_HEALTH_OK;
}

static kb_management_action_transport_t request_server(void *ctx, void *session, const char *method,
                                                       const char *path, const char *body,
                                                       const char *headers, uint64_t deadline,
                                                       char *out, size_t cap, int *status)
{
   fixture_t *f = ctx;
   assert(session == f && !strcmp(method, "POST") && deadline == 9000);
   f->requests++;
   if (!strcmp(path, "/v1/management/action/challenge"))
   {
      assert(f->stage == 4 && !*body && !headers);
      snprintf(out, cap,
               "{\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\","
               "\"expires_at\":\"1010\"}");
      *status = 200;
      return KB_MANAGEMENT_ACTION_SENT_RESPONSE;
   }
   assert(!strcmp(path, "/v1/management/action") && f->stage == 6);
   assert(!strcmp(body, "{\"action\":\"agent.enable\",\"agent\":\"alpha\"}"));
   assert(headers && strstr(headers, "Authorization: Bearer a.b.c\r\n") &&
          strstr(headers, "X-Aimee-Management-Status:"));
   f->stage = 7;
   if (f->action_transport == KB_MANAGEMENT_ACTION_SENT_RESPONSE)
   {
      snprintf(out, cap, "%s", f->response);
      *status = f->response_status;
   }
   return f->action_transport;
}

static void close_server(void *ctx, void *session)
{
   fixture_t *f = ctx;
   assert(session == f);
   f->closes++;
}

static kb_management_health_result_t authority(void *ctx, const kb_management_cert_bundle_t *bundle,
                                               const char *raw, size_t len, uint64_t deadline,
                                               char *out, size_t cap, int *status)
{
   fixture_t *f = ctx;
   assert(f->stage == 4 && bundle->leaf_pem_len && deadline == 9000);
   f->authority++;
   assert(raw && len && strstr(raw, "\"purpose\":\"management.action.v1\""));
   kb_mgmt_status_t s = {.version = 1,
                         .issued_at = 1000,
                         .expires_at = 1010,
                         .revocation_generation = f->newer_staple ? 4 : 3};
   memset(s.nonce, 0, 32);
   snprintf(s.key_id, sizeof(s.key_id), "status-1");
   snprintf(s.caller_issuer, sizeof(s.caller_issuer), "CN=client-ca");
   snprintf(s.caller_serial_norm, sizeof(s.caller_serial_norm), "2b");
   memset(s.caller_fingerprint, 'a', 64);
   snprintf(s.target_server_id, sizeof(s.target_server_id), "srv-1");
   memset(s.target_mgmt_fingerprint, 'b', 64);
   snprintf(s.purpose, sizeof(s.purpose), "management.action.v1");
   assert(kb_mgmt_status_sign(&s, f->sk) == 0 && kb_mgmt_status_to_json(&s, out, cap) == 0);
   *status = 200;
   return KB_MANAGEMENT_HEALTH_OK;
}

static kb_mgmt_token_authority_ipc_result_t token_issue(void *ctx, const char *correlation,
                                                        const char *jti,
                                                        kb_mgmt_token_authority_output_t *out)
{
   fixture_t *f = ctx;
   assert(f->stage == 5 && strlen(correlation) == 64 && strlen(jti) == 64);
   f->tokens++;
   if (f->token_result != KB_MGMT_TOKEN_AUTHORITY_IPC_OK)
      return f->token_result;
   snprintf(out->jwt, sizeof(out->jwt), "a.b.c");
   out->jwt_len = 5;
   f->stage = 6;
   return KB_MGMT_TOKEN_AUTHORITY_IPC_OK;
}

static uint64_t wall(void *ctx)
{
   return ((fixture_t *)ctx)->wall;
}
static uint64_t mono(void *ctx)
{
   return ((fixture_t *)ctx)->mono;
}

static kb_management_action_result_t run(fixture_t *f)
{
   g = f;
   kb_principal_t actor = {.authenticated = 1};
   kb_management_action_request_t r = {.actor = &actor,
                                       .team_id = 7,
                                       .server_id = "srv-1",
                                       .body = "{\"agent\":\"alpha\",\"action\":\"agent.enable\"}",
                                       .body_len = 48,
                                       .deadline_millis = 9000};
   r.body_len = strlen(r.body);
   kb_management_action_dependencies_t d = {.operation_init = operation_init,
                                            .intent_start = intent,
                                            .outcome_append = outcome,
                                            .snapshot_ctx = f,
                                            .snapshot = snapshot,
                                            .bundle_ctx = f,
                                            .bundle_load = bundle,
                                            .bundle_clear = clear,
                                            .server_ctx = f,
                                            .server_open = open_server,
                                            .server_request = request_server,
                                            .server_close = close_server,
                                            .authority_ctx = f,
                                            .authority_issue = authority,
                                            .token_ctx = f,
                                            .token_issue = token_issue,
                                            .clock_ctx = f,
                                            .wall_seconds = wall,
                                            .monotonic_millis = mono,
                                            .status_key_id = "status-1",
                                            .status_public_key = f->pk,
                                            .token_issuer = "https://issuer.example",
                                            .kid = "kid-1",
                                            .installation_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                                            .ttl_seconds = 90};
   return kb_management_action_execute(&r, &d);
}

/* Link-only production adapter dependencies. */
int db2_tenant_scope_begin(const kb_principal_t *p, int64_t t)
{
   return -1;
}
int db2_tenant_scope_commit(void)
{
   return -1;
}
void db2_tenant_scope_rollback(void)
{
}
int db2_server_registry_snapshot(int64_t t, const char *s, db2_server_snapshot_t *o)
{
   return -1;
}
kb_management_cert_result_t kb_management_cert_load_active(kb_management_cert_lifecycle_t *l,
                                                           kb_management_cert_bundle_t *b,
                                                           kb_management_cert_active_t *a)
{
   return KB_MANAGEMENT_CERT_UNAVAILABLE;
}
void kb_management_cert_bundle_clear(kb_management_cert_bundle_t *b)
{
   memset(b, 0, sizeof(*b));
}

static void reset(fixture_t *f)
{
   unsigned char sk[32], pk[32];
   memcpy(sk, f->sk, 32);
   memcpy(pk, f->pk, 32);
   memset(f, 0, sizeof(*f));
   memcpy(f->sk, sk, 32);
   memcpy(f->pk, pk, 32);
   f->wall = 1000;
   f->mono = 100;
   f->action_transport = KB_MANAGEMENT_ACTION_SENT_RESPONSE;
   f->response = "{\"result\":\"succeeded\",\"effect\":\"applied\"}";
   f->response_status = 200;
}

int main(void)
{
   kb_management_action_body_t a, b;
   const char *one = "{\"action\":\"agent.enable\",\"agent\":\"alpha\"}";
   const char *two = "{\"agent\":\"alpha\",\"action\":\"agent.enable\"}";
   assert(!kb_management_action_body_parse(one, strlen(one), &a));
   assert(!kb_management_action_body_parse(two, strlen(two), &b));
   assert(!strcmp(a.canonical, b.canonical) && !memcmp(a.digest, b.digest, 32));
   /* The roster ops were renamed to `model.*`; this is an INGRESS contract, so
    * both spellings parse. They are distinct actions on the wire, not aliases —
    * each is echoed verbatim, so the canonical form and digest differ. */
   const char *renamed = "{\"action\":\"model.enable\",\"agent\":\"alpha\"}";
   assert(!kb_management_action_body_parse(renamed, strlen(renamed), &b));
   assert(!strcmp(b.canonical, "{\"action\":\"model.enable\",\"agent\":\"alpha\"}"));
   assert(strcmp(a.canonical, b.canonical) != 0);
   const char *renamed_off = "{\"action\":\"model.disable\",\"agent\":\"alpha\"}";
   assert(!kb_management_action_body_parse(renamed_off, strlen(renamed_off), &b));
   assert(kb_management_action_body_parse(
       "{\"action\":\"agent.enable\",\"agent\":\"a\",\"agent\":\"b\"}", 54, &a));
   assert(kb_management_action_body_parse("{\"action\":\"agent.run\",\"agent\":\"a\"}", 35, &a));
   const char *nul_action = "{\"action\":\"agent.enable\\u0000junk\",\"agent\":\"alpha\"}";
   const char *nul_agent = "{\"action\":\"agent.enable\",\"agent\":\"alpha\\u0000other\"}";
   assert(kb_management_action_body_parse(nul_action, strlen(nul_action), &a));
   assert(kb_management_action_body_parse(nul_agent, strlen(nul_agent), &a));
   const char *literal_escape = "{\"action\":\"agent.enable\",\"agent\":\"alpha\\\\u0000\"}";
   assert(!json_wire_has_nul_escape(literal_escape, strlen(literal_escape)));
   assert(json_wire_has_nul_escape(nul_action, strlen(nul_action)));
   assert(json_wire_has_nul_escape(nul_agent, strlen(nul_agent)));
   assert(kb_management_action_body_parse(literal_escape, strlen(literal_escape), &a));
   db2_management_action_outcome_operation_t decoded = {0};
   const char *reversed = "{\"effect\":\"applied\",\"result\":\"succeeded\"}";
   assert(kb_management_action_response_parse(reversed, strlen(reversed), 200, &decoded));

   fixture_t f = {0};
   memset(f.sk, 7, 32);
   EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, f.sk, 32);
   size_t n = 32;
   assert(key && EVP_PKEY_get_raw_public_key(key, f.pk, &n) == 1 && n == 32);
   EVP_PKEY_free(key);
   reset(&f);
   assert(run(&f) == KB_MANAGEMENT_ACTION_OK && f.intents == 1 && f.snapshots == 2 &&
          f.tokens == 1 && f.outcomes == 1 && f.outcome.result == DB2_MANAGEMENT_ACTION_SUCCEEDED &&
          f.outcome.has_status_code && f.outcome.has_response_sha256 && f.closes == 1 &&
          f.clears == 1);

   reset(&f);
   f.ambiguous_intent = 1;
   assert(run(&f) == KB_MANAGEMENT_ACTION_OK && f.intents == 2);
   reset(&f);
   f.replay = 1;
   assert(run(&f) == KB_MANAGEMENT_ACTION_CONFLICT && f.opens == 0 && f.tokens == 0);
   reset(&f);
   f.invalid_active = 1;
   assert(run(&f) == KB_MANAGEMENT_ACTION_UNAVAILABLE && f.opens == 0 && f.clears == 1);
   reset(&f);
   f.newer_staple = 1;
   assert(run(&f) == KB_MANAGEMENT_ACTION_UNAVAILABLE && f.tokens == 0 && f.requests == 1 &&
          f.outcomes == 1);
   for (int denied = KB_MGMT_TOKEN_AUTHORITY_IPC_INVALID;
        denied <= KB_MGMT_TOKEN_AUTHORITY_IPC_ALREADY_USED; denied++)
   {
      reset(&f);
      f.token_result = (kb_mgmt_token_authority_ipc_result_t)denied;
      assert(run(&f) == KB_MANAGEMENT_ACTION_UNAVAILABLE && f.requests == 1 && f.outcomes == 1 &&
             f.outcome.result_class == DB2_MANAGEMENT_ACTION_CLASS_LOCAL_FAILURE);
   }
   reset(&f);
   f.action_transport = KB_MANAGEMENT_ACTION_NOT_SENT;
   assert(run(&f) == KB_MANAGEMENT_ACTION_UNAVAILABLE &&
          f.outcome.result_class == DB2_MANAGEMENT_ACTION_CLASS_LOCAL_FAILURE);
   reset(&f);
   f.action_transport = KB_MANAGEMENT_ACTION_SENT_AMBIGUOUS;
   assert(run(&f) == KB_MANAGEMENT_ACTION_INDETERMINATE &&
          f.outcome.result_class == DB2_MANAGEMENT_ACTION_CLASS_TRANSPORT_AMBIGUOUS);
   reset(&f);
   f.response = "{\"result\":\"succeeded\",\"effect\":\"none\"}";
   assert(run(&f) == KB_MANAGEMENT_ACTION_INDETERMINATE &&
          f.outcome.result_class == DB2_MANAGEMENT_ACTION_CLASS_PROTOCOL_FAILURE);
   static const struct
   {
      const char *body;
      int status;
      kb_management_action_result_t result;
      db2_management_action_outcome_class_t result_class;
   } responses[] = {
       {"{\"result\":\"denied\",\"effect\":\"none\"}", 403, KB_MANAGEMENT_ACTION_DENIED,
        DB2_MANAGEMENT_ACTION_CLASS_REMOTE_DENIED},
       {"{\"result\":\"failed\",\"effect\":\"none\"}", 500, KB_MANAGEMENT_ACTION_UNAVAILABLE,
        DB2_MANAGEMENT_ACTION_CLASS_REMOTE_FAILURE},
       {"{\"result\":\"indeterminate\",\"effect\":\"unknown\"}", 502,
        KB_MANAGEMENT_ACTION_INDETERMINATE, DB2_MANAGEMENT_ACTION_CLASS_PROTOCOL_FAILURE},
   };
   for (size_t i = 0; i < sizeof(responses) / sizeof(responses[0]); i++)
   {
      reset(&f);
      f.response = responses[i].body;
      f.response_status = responses[i].status;
      assert(run(&f) == responses[i].result && f.outcome.result_class == responses[i].result_class);
   }
   reset(&f);
   f.ambiguous_outcome = 1;
   assert(run(&f) == KB_MANAGEMENT_ACTION_OK && f.outcomes == 2);
   puts("kb_management_action: all tests passed");
   return 0;
}
