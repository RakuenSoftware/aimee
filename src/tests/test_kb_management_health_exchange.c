#include "kb/kb_management_health_exchange.h"
#include "kb_mgmt_status_authority.h"

#include <assert.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
   int snapshots, loads, clears, opens, requests, closes, authority;
   int generation_b;
   int snapshot_generation, staple_generation;
   int inactive_a, change_b, bad_challenge, authority_status, corrupt_signature, wrong_health;
   int future_leaf;
   uint64_t wall, mono;
   unsigned char sk[32], pk[32];
   void *session;
} fixture_t;

static kb_management_health_result_t snapshot(void *opaque, const kb_principal_t *actor,
                                              int64_t team, const char *id,
                                              db2_server_snapshot_t *out)
{
   fixture_t *f = opaque;
   assert(actor && team == 7 && !strcmp(id, "srv-1"));
   f->snapshots++;
   memset(out, 0, sizeof(*out));
   snprintf(out->server_id, sizeof(out->server_id), "srv-1");
   snprintf(out->endpoint, sizeof(out->endpoint), "https://8.8.8.8");
   snprintf(out->status, sizeof(out->status), "active");
   snprintf(out->management_issuer, sizeof(out->management_issuer), "CN=server-ca");
   snprintf(out->management_serial_norm, sizeof(out->management_serial_norm), "1a");
   memset(out->management_fingerprint, 'b', 64);
   out->management_fingerprint[64] = 0;
   snprintf(out->enrollment_state, sizeof(out->enrollment_state), "active");
   out->revocation_generation = f->snapshot_generation ? f->snapshot_generation : 1;
   if (f->snapshots == 2 && f->generation_b)
      out->revocation_generation++;
   if (f->inactive_a && f->snapshots == 1)
      snprintf(out->status, sizeof(out->status), "disabled");
   if (f->change_b && f->snapshots == 2)
      snprintf(out->endpoint, sizeof(out->endpoint), "https://1.1.1.1");
   return KB_MANAGEMENT_HEALTH_OK;
}

static kb_management_health_result_t bundle_load(void *opaque, kb_management_cert_bundle_t *b,
                                                 kb_management_cert_active_t *a)
{
   fixture_t *f = opaque;
   f->loads++;
   memset(b, 0xa5, sizeof(*b));
   b->leaf_pem_len = b->key_pem_len = b->ca_pem_len = 1;
   memset(a, 0, sizeof(*a));
   a->not_before_epoch = f->future_leaf ? 1001 : 1;
   a->not_after_epoch = 2000;
   a->revocation_generation = 1;
   snprintf(a->issuer, sizeof(a->issuer), "CN=client-ca");
   snprintf(a->serial_norm, sizeof(a->serial_norm), "2b");
   memset(a->fingerprint, 0xaa, sizeof(a->fingerprint));
   return KB_MANAGEMENT_HEALTH_OK;
}

static void bundle_clear(void *opaque, kb_management_cert_bundle_t *b)
{
   fixture_t *f = opaque;
   f->clears++;
   memset(b, 0, sizeof(*b));
}

static kb_management_health_result_t server_open(void *opaque,
                                                 const db2_server_snapshot_t *snapshot,
                                                 const kb_management_cert_bundle_t *bundle,
                                                 uint64_t deadline, void **out)
{
   fixture_t *f = opaque;
   assert(snapshot && bundle->leaf_pem_len && deadline == 9000);
   f->opens++;
   *out = f->session = (void *)0x1234;
   return KB_MANAGEMENT_HEALTH_OK;
}

static kb_management_health_result_t server_request(void *opaque, void *session, const char *method,
                                                    const char *path, const char *body,
                                                    const char *headers, uint64_t deadline,
                                                    char *out, size_t cap, int *status)
{
   fixture_t *f = opaque;
   assert(session == f->session && deadline <= 9000 && body && !*body);
   f->requests++;
   *status = 200;
   if (!strcmp(path, "/v1/management/challenge"))
   {
      assert(!strcmp(method, "POST") && !headers);
      snprintf(out, cap,
               f->bad_challenge ? "{\"nonce\":\"bad\",\"expires_at\":\"1010\"}"
                                : "{\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\","
                                  "\"expires_at\":\"1010\"}");
   }
   else
   {
      assert(!strcmp(method, "GET") && headers && strstr(headers, "X-Aimee-Management-Status:"));
      snprintf(out, cap, "{\"status\":\"ok\",\"server_id\":\"%s\"}",
               f->wrong_health ? "other" : "srv-1");
   }
   return KB_MANAGEMENT_HEALTH_OK;
}

static void server_close(void *opaque, void *session)
{
   fixture_t *f = opaque;
   assert(session == f->session);
   f->closes++;
}

static kb_management_health_result_t authority(void *opaque,
                                               const kb_management_cert_bundle_t *bundle,
                                               const char *raw, size_t len, uint64_t deadline,
                                               char *out, size_t cap, int *status)
{
   fixture_t *f = opaque;
   assert(bundle->leaf_pem_len && deadline <= 9000);
   f->authority++;
   if (f->authority_status)
   {
      *status = f->authority_status;
      out[0] = 0;
      return KB_MANAGEMENT_HEALTH_OK;
   }
   kb_mgmt_status_request_t request;
   assert(kb_mgmt_status_request_from_json(raw, len, &request) == KB_MGMT_STATUS_AUTHORITY_OK);
   kb_mgmt_status_t staple = {.version = 1,
                              .issued_at = 1000,
                              .expires_at = 1010,
                              .revocation_generation =
                                  f->staple_generation ? f->staple_generation : 1};
   memcpy(staple.nonce, request.nonce, sizeof(staple.nonce));
   snprintf(staple.key_id, sizeof(staple.key_id), "status-1");
   snprintf(staple.caller_issuer, sizeof(staple.caller_issuer), "CN=client-ca");
   snprintf(staple.caller_serial_norm, sizeof(staple.caller_serial_norm), "2b");
   memset(staple.caller_fingerprint, 'a', 64);
   staple.caller_fingerprint[64] = 0;
   snprintf(staple.target_server_id, sizeof(staple.target_server_id), "srv-1");
   memset(staple.target_mgmt_fingerprint, 'b', 64);
   staple.target_mgmt_fingerprint[64] = 0;
   snprintf(staple.purpose, sizeof(staple.purpose), "management.health.v1");
   assert(kb_mgmt_status_sign(&staple, f->sk) == 0);
   if (f->corrupt_signature)
      staple.signature[0] ^= 1;
   assert(kb_mgmt_status_to_json(&staple, out, cap) == 0);
   *status = 200;
   return KB_MANAGEMENT_HEALTH_OK;
}

static uint64_t wall(void *opaque)
{
   return ((fixture_t *)opaque)->wall;
}
static uint64_t mono(void *opaque)
{
   return ((fixture_t *)opaque)->mono;
}

static kb_management_health_result_t run(fixture_t *f, const unsigned char pk[32])
{
   kb_principal_t actor = {.authenticated = 1};
   kb_management_health_dependencies_t d = {.snapshot_ctx = f,
                                            .snapshot = snapshot,
                                            .bundle_ctx = f,
                                            .bundle_load = bundle_load,
                                            .bundle_clear = bundle_clear,
                                            .server_ctx = f,
                                            .server_open = server_open,
                                            .server_request = server_request,
                                            .server_close = server_close,
                                            .authority_ctx = f,
                                            .authority_issue = authority,
                                            .clock_ctx = f,
                                            .wall_seconds = wall,
                                            .monotonic_millis = mono,
                                            .status_key_id = "status-1",
                                            .status_public_key = pk};
   kb_management_health_request_t r = {
       .actor = &actor, .team_id = 7, .server_id = "srv-1", .deadline_millis = 9000};
   return kb_management_health_exchange(&r, &d);
}

/* Link-only stubs for production adapters; this test exercises injected seams. */
int db2_tenant_scope_begin(const kb_principal_t *p, int64_t team)
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
int db2_server_registry_snapshot(int64_t team, const char *id, db2_server_snapshot_t *out)
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

int main(void)
{
   fixture_t f = {.generation_b = 1, .wall = 1000, .mono = 100};
   memset(f.sk, 7, sizeof(f.sk));
   EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, f.sk, sizeof(f.sk));
   size_t n = sizeof(f.pk);
   assert(key && EVP_PKEY_get_raw_public_key(key, f.pk, &n) == 1 && n == sizeof(f.pk));
   EVP_PKEY_free(key);
   assert(run(&f, f.pk) == KB_MANAGEMENT_HEALTH_OK);
   assert(f.snapshots == 2 && f.loads == 1 && f.clears == 1 && f.opens == 1 && f.requests == 2 &&
          f.authority == 1 && f.closes == 1);

#define RESET_COUNTS()                                                                             \
   do                                                                                              \
   {                                                                                               \
      f.snapshots = f.loads = f.clears = f.opens = f.requests = f.closes = f.authority = 0;        \
   } while (0)
   RESET_COUNTS();
   f.change_b = 1;
   assert(run(&f, f.pk) == KB_MANAGEMENT_HEALTH_CONFLICT && f.closes == 1 && f.clears == 1);
   f.change_b = 0;
   RESET_COUNTS();
   f.bad_challenge = 1;
   assert(run(&f, f.pk) == KB_MANAGEMENT_HEALTH_INTEGRITY && f.authority == 0 && f.closes == 1 &&
          f.clears == 1);
   f.bad_challenge = 0;
   RESET_COUNTS();
   f.authority_status = 403;
   assert(run(&f, f.pk) == KB_MANAGEMENT_HEALTH_DENIED && f.closes == 1 && f.clears == 1);
   f.authority_status = 0;
   RESET_COUNTS();
   f.corrupt_signature = 1;
   assert(run(&f, f.pk) == KB_MANAGEMENT_HEALTH_INTEGRITY && f.closes == 1 && f.clears == 1);
   f.corrupt_signature = 0;
   RESET_COUNTS();
   f.wrong_health = 1;
   assert(run(&f, f.pk) == KB_MANAGEMENT_HEALTH_INTEGRITY && f.closes == 1 && f.clears == 1);
   f.wrong_health = 0;
   RESET_COUNTS();
   f.inactive_a = 1;
   assert(run(&f, f.pk) == KB_MANAGEMENT_HEALTH_DENIED && f.opens == 0 && f.loads == 0);
   f.inactive_a = 0;
   RESET_COUNTS();
   f.future_leaf = 1;
   assert(run(&f, f.pk) == KB_MANAGEMENT_HEALTH_DENIED && f.opens == 0 && f.clears == 1);
   f.future_leaf = 0;
   RESET_COUNTS();
   f.snapshot_generation = 2;
   f.staple_generation = 1;
   assert(run(&f, f.pk) == KB_MANAGEMENT_HEALTH_INTEGRITY && f.snapshots == 1 && f.closes == 1 &&
          f.clears == 1);

   unsigned char nonce[32];
   uint64_t expires;
   const char *good =
       "{\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\",\"expires_at\":\"1\"}";
   assert(kb_management_health_challenge_decode(good, strlen(good), nonce, &expires) == 0 &&
          expires == 1);
   const char *bad = "{\"nonce\":\"x\",\"expires_at\":\"01\"}";
   assert(kb_management_health_challenge_decode(bad, strlen(bad), nonce, &expires) == -1);
   const char *read_good =
       "{\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\","
       "\"purpose\":\"management.read.v1\",\"expires_at\":1010}";
   assert(kb_management_read_challenge_decode(read_good, strlen(read_good), nonce, &expires) == 0 &&
          expires == 1010);
   const char *read_bad_purpose =
       "{\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\","
       "\"purpose\":\"management.health.v1\",\"expires_at\":1010}";
   assert(kb_management_read_challenge_decode(read_bad_purpose, strlen(read_bad_purpose), nonce,
                                              &expires) == -1);
   const char *read_bad_shape =
       "{\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\","
       "\"purpose\":\"management.read.v1\",\"expires_at\":1010,\"extra\":true}";
   assert(kb_management_read_challenge_decode(read_bad_shape, strlen(read_bad_shape), nonce,
                                              &expires) == -1);
   const char *health = "{\"status\":\"ok\",\"server_id\":\"srv-1\"}";
   assert(kb_management_health_response_decode(health, strlen(health), "srv-1") == 0);
   puts("kb_management_health_exchange: all tests passed");
   return 0;
}
