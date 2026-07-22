#include "kb_mgmt_status_authority.h"

#include <assert.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

static void assert_zero(const void *p, size_t n);

static int lookup(const char *issuer, const char *serial, const char *fp, const char *target,
                  const char *purpose, int64_t *generation, char *target_fp, size_t cap, void *ctx)
{
   (void)ctx;
   if (strcmp(issuer, "/CN=ca") || strcmp(serial, "01") || strlen(fp) != 64 ||
       strcmp(target, "server-1") || strcmp(purpose, "management.health.v1"))
      return -1;
   *generation = 9;
   snprintf(target_fp, cap, "%064d", 2);
   return 0;
}

static int sign_status(kb_mgmt_status_t *s, void *ctx)
{
   return kb_mgmt_status_sign(s, ctx);
}

static int lookup_result;

static int typed_lookup(const char *issuer, const char *serial, const char *fp, const char *target,
                        const char *purpose, int64_t *generation, char *target_fp, size_t cap,
                        void *ctx)
{
   if (lookup_result)
      return lookup_result;
   return lookup(issuer, serial, fp, target, purpose, generation, target_fp, cap, ctx);
}

static int sign_result(kb_mgmt_status_t *s, void *ctx)
{
   (void)s;
   return *(int *)ctx;
}

static int checkpoint_lookup(const char *peer_issuer, const char *peer_serial,
                             const char *peer_fingerprint,
                             const kb_mgmt_checkpoint_request_t *request, int *revoked,
                             int64_t *generation, void *ctx)
{
   (void)ctx;
   assert(!strcmp(peer_issuer, "/CN=server"));
   assert(!strcmp(peer_serial, "02"));
   assert(strlen(peer_fingerprint) == 64);
   assert(!strcmp(request->caller_issuer, "/CN=kb"));
   assert(!strcmp(request->target_server_id, "server-1"));
   *revoked = 1;
   *generation = 12;
   return KB_MGMT_STATUS_CALLBACK_OK;
}

static int checkpoint_sign(kb_mgmt_checkpoint_t *checkpoint,
                           const kb_mgmt_checkpoint_request_t *request, void *ctx)
{
   assert(request->canonical_sha256[0]);
   return kb_mgmt_checkpoint_sign(checkpoint, ctx);
}

static void test_checkpoint(const unsigned char sk[32], const unsigned char pk[32])
{
   static const char request_json[] =
       "{\"version\":\"1\",\"purpose\":\"management.action.v1\","
       "\"nonce\":\"BwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwc\","
       "\"caller_issuer_b64\":\"L0NOPWti\",\"caller_serial\":\"01\","
       "\"caller_fingerprint\":"
       "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
       "\"target\":\"server-1\",\"staple_generation\":\"9\","
       "\"staple_sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
       "\"correlation_id\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\","
       "\"jti\":\"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\","
       "\"request_sha256\":\"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\"}";
   kb_mgmt_checkpoint_request_t request;
   assert(kb_mgmt_checkpoint_request_from_json(request_json, strlen(request_json), &request) ==
          KB_MGMT_STATUS_AUTHORITY_OK);
   assert(strlen(request.canonical_sha256) == 64 && request.staple_generation == 9);
   char noncanonical[sizeof(request_json) + 2];
   snprintf(noncanonical, sizeof(noncanonical), " %s", request_json);
   assert(kb_mgmt_checkpoint_request_from_json(noncanonical, strlen(noncanonical), &request) ==
          KB_MGMT_STATUS_AUTHORITY_INVALID);
   assert_zero(&request, sizeof(request));
   assert(kb_mgmt_checkpoint_request_from_json(request_json, strlen(request_json), &request) ==
          KB_MGMT_STATUS_AUTHORITY_OK);
   char server_fp[65];
   memset(server_fp, 'e', 64);
   server_fp[64] = 0;
   kb_mgmt_checkpoint_t out;
   assert(kb_mgmt_checkpoint_authority_issue(&request, "/CN=server", "02", server_fp, "key-1", 3000,
                                             checkpoint_lookup, NULL, checkpoint_sign, (void *)sk,
                                             &out) == KB_MGMT_STATUS_AUTHORITY_OK);
   assert(out.revoked == 1 && out.generation == 12 && out.expires_at == 3005);
   assert(!strcmp(out.request_sha256, request.canonical_sha256));
   assert(kb_mgmt_checkpoint_verify_signature(&out, pk) == 0);
}

static void assert_zero(const void *p, size_t n)
{
   const unsigned char *bytes = p;
   for (size_t i = 0; i < n; ++i)
      assert(bytes[i] == 0);
}

static void test_codec(void)
{
   static const char valid[] =
       "{\"nonce\":\"BwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwc\","
       "\"target\":\"server-1\","
       "\"target_mgmt_fp\":\"0000000000000000000000000000000000000000000000000000000000000002\","
       "\"purpose\":\"management.health.v1\"}";
   kb_mgmt_status_request_t r;
   memset(&r, 0xa5, sizeof(r));
   assert(kb_mgmt_status_request_from_json(valid, sizeof(valid) - 1, &r) ==
          KB_MGMT_STATUS_AUTHORITY_OK);
   for (size_t i = 0; i < sizeof(r.nonce); ++i)
      assert(r.nonce[i] == 7);
   assert(!strcmp(r.target_server_id, "server-1"));

   static const char *invalid[] = {
       "{}",
       "{\"nonce\":\"BwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwc=\",\"target\":\"server-1\","
       "\"target_mgmt_fp\":\"0000000000000000000000000000000000000000000000000000000000000002\","
       "\"purpose\":\"management.health.v1\"}",
       "{\"nonce\":\"BwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwd\",\"target\":\"server-1\","
       "\"target_mgmt_fp\":\"0000000000000000000000000000000000000000000000000000000000000002\","
       "\"purpose\":\"management.health.v1\"}",
       "{\"nonce\":\"BwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwc\",\"nonce\":"
       "\"BwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwc\",\"target\":\"server-1\",\"target_mgmt_fp\":"
       "\"0000000000000000000000000000000000000000000000000000000000000002\",\"purpose\":"
       "\"management.health.v1\"}",
       "{\"nonce\":\"BwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwc\",\"target\":\"bad "
       "target\",\"target_mgmt_fp\":"
       "\"0000000000000000000000000000000000000000000000000000000000000002\",\"purpose\":"
       "\"management.health.v1\"}",
       "{\"nonce\":\"BwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwc\",\"target\":\"server-1\","
       "\"target_mgmt_fp\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\","
       "\"purpose\":\"management.health.v1\"}",
       "{\"nonce\":\"BwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwc\",\"target\":\"server-1\","
       "\"target_mgmt_fp\":\"0000000000000000000000000000000000000000000000000000000000000002\","
       "\"purpose\":\"management.health.v2\"}",
       "{\"nonce\":\"BwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwc\",\"target\":\"server-1\","
       "\"target_mgmt_fp\":\"0000000000000000000000000000000000000000000000000000000000000002\","
       "\"purpose\":\"management.health.v1\",\"extra\":\"x\"}",
       "{\"nonce\":7,\"target\":\"server-1\",\"target_mgmt_fp\":"
       "\"0000000000000000000000000000000000000000000000000000000000000002\",\"purpose\":"
       "\"management.health.v1\"}",
       " {\"nonce\":\"BwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwc\",\"target\":\"server-1\","
       "\"target_mgmt_fp\":\"0000000000000000000000000000000000000000000000000000000000000002\","
       "\"purpose\":\"management.health.v1\"}",
       "{\"nonce\":\"BwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwc\",\"target\":\"server-"
       "1\\u0000evil\",\"target_mgmt_fp\":"
       "\"0000000000000000000000000000000000000000000000000000000000000002\",\"purpose\":"
       "\"management.health.v1\"}",
   };
   for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
   {
      memset(&r, 0xa5, sizeof(r));
      assert(kb_mgmt_status_request_from_json(invalid[i], strlen(invalid[i]), &r) ==
             KB_MGMT_STATUS_AUTHORITY_INVALID);
      assert_zero(&r, sizeof(r));
   }
   char embedded[sizeof(valid)];
   memcpy(embedded, valid, sizeof(valid));
   embedded[10] = '\0';
   memset(&r, 0xa5, sizeof(r));
   assert(kb_mgmt_status_request_from_json(embedded, sizeof(embedded) - 1, &r) ==
          KB_MGMT_STATUS_AUTHORITY_INVALID);
   assert_zero(&r, sizeof(r));
}

int main(void)
{
   test_codec();
   kb_mgmt_status_request_t r = {0};
   snprintf(r.target_server_id, sizeof(r.target_server_id), "server-1");
   snprintf(r.target_mgmt_fingerprint, sizeof(r.target_mgmt_fingerprint), "%064d", 2);
   snprintf(r.purpose, sizeof(r.purpose), "management.health.v1");
   memset(r.nonce, 7, sizeof(r.nonce));
   char caller_fp[65];
   memset(caller_fp, 'a', 64);
   caller_fp[64] = '\0';
   unsigned char sk[32], pk[32];
   EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
   EVP_PKEY *key = NULL;
   assert(kctx && EVP_PKEY_keygen_init(kctx) == 1 && EVP_PKEY_keygen(kctx, &key) == 1);
   size_t n = 32;
   assert(EVP_PKEY_get_raw_private_key(key, sk, &n) == 1);
   n = 32;
   assert(EVP_PKEY_get_raw_public_key(key, pk, &n) == 1);
   test_checkpoint(sk, pk);
   kb_mgmt_status_t out;
   assert(kb_mgmt_status_authority_issue(&r, "/CN=ca", "01", caller_fp, "key-1", 100, typed_lookup,
                                         NULL, sign_status, sk,
                                         &out) == KB_MGMT_STATUS_AUTHORITY_OK);
   assert(out.revocation_generation == 9 && out.expires_at == 110);
   assert(kb_mgmt_status_verify_signature(&out, pk) == 0);
   r.target_mgmt_fingerprint[0] = 'f';
   memset(&out, 0xa5, sizeof(out));
   assert(kb_mgmt_status_authority_issue(&r, "/CN=ca", "01", caller_fp, "key-1", 100, typed_lookup,
                                         NULL, sign_status, sk,
                                         &out) == KB_MGMT_STATUS_AUTHORITY_DENIED);
   assert_zero(&out, sizeof(out));
   r.target_mgmt_fingerprint[0] = '0';
   lookup_result = KB_MGMT_STATUS_CALLBACK_DENIED;
   memset(&out, 0xa5, sizeof(out));
   assert(kb_mgmt_status_authority_issue(&r, "/CN=ca", "01", caller_fp, "key-1", 100, typed_lookup,
                                         NULL, sign_status, sk,
                                         &out) == KB_MGMT_STATUS_AUTHORITY_DENIED);
   assert_zero(&out, sizeof(out));
   lookup_result = KB_MGMT_STATUS_CALLBACK_CONFLICT;
   assert(kb_mgmt_status_authority_issue(&r, "/CN=ca", "01", caller_fp, "key-1", 100, typed_lookup,
                                         NULL, sign_status, sk,
                                         &out) == KB_MGMT_STATUS_AUTHORITY_CONFLICT);
   lookup_result = 0;
   int typed_sign = KB_MGMT_STATUS_CALLBACK_INTEGRITY;
   memset(&out, 0xa5, sizeof(out));
   assert(kb_mgmt_status_authority_issue(&r, "/CN=ca", "01", caller_fp, "key-1", 100, typed_lookup,
                                         NULL, sign_result, &typed_sign,
                                         &out) == KB_MGMT_STATUS_AUTHORITY_INTEGRITY);
   assert_zero(&out, sizeof(out));
   EVP_PKEY_free(key);
   EVP_PKEY_CTX_free(kctx);
   puts("kb_mgmt_status_authority: ok");
   return 0;
}
