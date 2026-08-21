#include "server_http_mgmt_read_routes.h"

#include "kb_client_mtls.h"
#include "server_http_identity.h"
#include "server_http_internal.h"
#include "server_management_jti.h"
#include "server_mgmt_checkpoint_client.h"
#include "server_mgmt_jwks_cache.h"
#include "management_read.h"
#include "server_mgmt_status.h"
#include "server_runtime_identity.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int read_hex_key(const char *hex, unsigned char key[32])
{
   if (!hex || strlen(hex) != 64)
      return -1;
   for (int i = 0; i < 32; ++i)
   {
      unsigned a = (unsigned char)hex[i * 2], b = (unsigned char)hex[i * 2 + 1];
      a = a >= '0' && a <= '9' ? a - '0' : (a >= 'a' && a <= 'f' ? a - 'a' + 10 : 99);
      b = b >= '0' && b <= '9' ? b - '0' : (b >= 'a' && b <= 'f' ? b - 'a' + 10 : 99);
      if (a > 15 || b > 15)
         return -1;
      key[i] = (unsigned char)((a << 4) | b);
   }
   return 0;
}

static int read_b64url(const unsigned char *in, size_t n, char *out, size_t cap)
{
   unsigned char encoded[65];
   size_t padded = 4 * ((n + 2) / 3);
   size_t padding = n % 3 ? 3 - (n % 3) : 0;
   size_t need = padded - padding;
   if (!in || !out || padded + 1 > sizeof(encoded) || cap <= need)
      return -1;
   int got = EVP_EncodeBlock(encoded, in, (int)n);
   if (got <= 0)
      return -1;
   while (got > 0 && encoded[got - 1] == '=')
      --got;
   if ((size_t)got != need)
      return -1;
   for (int i = 0; i < got; ++i)
      out[i] = encoded[i] == '+' ? '-' : (encoded[i] == '/' ? '_' : (char)encoded[i]);
   out[got] = '\0';
   OPENSSL_cleanse(encoded, sizeof(encoded));
   return 0;
}

static server_mgmt_read_result_t read_status(void *ctx, const server_mgmt_read_request_t *rq,
                                             server_mgmt_read_status_proof_t *proof)
{
   (void)ctx;
   const char *key_id = getenv("AIMEE_MGMT_STATUS_KEY_ID");
   const char *key_hex = getenv("AIMEE_MGMT_STATUS_PUBLIC_KEY");
   unsigned char pub[32], digest[SHA256_DIGEST_LENGTH];
   kb_mgmt_status_t st;
   const char *purpose = rq ? server_mgmt_read_selector_purpose(rq->selector) : NULL;
   if (!proof || !purpose || !key_id || !rq->staple || rq->staple_len > KB_MGMT_STATUS_JSON_MAX ||
       read_hex_key(key_hex, pub) != 0)
      return SERVER_MGMT_READ_UNAVAILABLE;
   memset(proof, 0, sizeof(*proof));
   if (kb_mgmt_status_from_json(rq->staple, &st) != 0)
   {
      memset(&st, 0, sizeof(st));
      if (kb_mgmt_status_nonce_from_json(rq->staple, st.nonce) == 0)
      {
         server_mgmt_nonce_result_t consumed = server_mgmt_nonce_consume_purpose(
             &st, rq->peer, rq->server_id, purpose, (uint64_t)rq->now, 0);
         if (consumed == SERVER_MGMT_NONCE_STORAGE)
            return SERVER_MGMT_READ_UNAVAILABLE;
      }
      return SERVER_MGMT_READ_INTEGRITY;
   }
   uint64_t hwm = 0;
   int valid = server_mgmt_status_hwm(&hwm) == 0 &&
               kb_mgmt_status_validate(&st, (uint64_t)rq->now, hwm) == 0 &&
               kb_mgmt_status_verify_signature(&st, pub) == 0 && !strcmp(st.key_id, key_id) &&
               !strcmp(st.caller_issuer, rq->peer->issuer) &&
               !strcmp(st.caller_serial_norm, rq->peer->serial_norm) &&
               !strcmp(st.caller_fingerprint, rq->peer->fingerprint) &&
               !strcmp(st.target_server_id, rq->server_id) &&
               !strcmp(st.target_mgmt_fingerprint, rq->local_fingerprint) &&
               !strcmp(st.purpose, purpose);
   server_mgmt_nonce_result_t consumed = server_mgmt_nonce_consume_purpose(
       &st, rq->peer, rq->server_id, purpose, (uint64_t)rq->now, valid);
   if (consumed != SERVER_MGMT_NONCE_OK)
      return consumed == SERVER_MGMT_NONCE_STORAGE ? SERVER_MGMT_READ_UNAVAILABLE
             : consumed == SERVER_MGMT_NONCE_NOT_FOUND || consumed == SERVER_MGMT_NONCE_EXPIRED ||
                     consumed == SERVER_MGMT_NONCE_ROLLBACK
                 ? SERVER_MGMT_READ_CONFLICT
                 : SERVER_MGMT_READ_INTEGRITY;
   if (!SHA256((const unsigned char *)rq->staple, rq->staple_len, digest))
      return SERVER_MGMT_READ_INTEGRITY;
   memcpy(proof->nonce, st.nonce, sizeof(proof->nonce));
   proof->revocation_generation = st.revocation_generation;
   for (size_t i = 0; i < sizeof(digest); ++i)
      snprintf(proof->staple_sha256 + i * 2, 3, "%02x", digest[i]);
   return SERVER_MGMT_READ_OK;
}

static server_mgmt_read_result_t read_token(void *ctx, const server_mgmt_read_request_t *rq,
                                            server_mgmt_token_claims_t *claims)
{
   (void)ctx;
   const char *path = getenv("AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE");
   char trust[SERVER_MGMT_JWKS_BUNDLE_MAX];
   size_t trust_len = 0;
   if (!path || server_mgmt_jwks_trust_bundle_load(path, trust, sizeof(trust), &trust_len) != 0)
      return SERVER_MGMT_READ_UNAVAILABLE;
   server_mgmt_token_result_t rc = server_mgmt_token_verify_read_claims_cached(
       rq->jwt, rq->jwt_len, trust, trust_len, rq->expected_issuer, rq->server_id, rq->peer->issuer,
       rq->peer->serial_norm, rq->peer->fingerprint, rq->now, kb_client_mtls_management_jwks_fetch,
       NULL, claims);
   OPENSSL_cleanse(trust, sizeof(trust));
   return rc == SERVER_MGMT_TOKEN_OK ? SERVER_MGMT_READ_OK : SERVER_MGMT_READ_INTEGRITY;
}

static server_mgmt_endpoint_jti_result_t
read_jti(void *ctx, const server_mgmt_endpoint_request_t *rq, const server_mgmt_token_claims_t *c)
{
   (void)ctx;
   server_management_jti_t token = {
       c->jti,
       c->issuer,
       c->kid,
       c->audience,
       c->subject,
       c->team_id,
       c->capability,
       c->peer_issuer,
       c->peer_serial,
       c->peer_fingerprint,
       c->request_sha256,
       c->correlation_id,
       c->issued_at,
       c->expires_at,
   };
   server_management_jti_result_t rc = db1_management_jti_consume(&token, rq->now);
   return rc == SERVER_MANAGEMENT_JTI_OK       ? SERVER_MGMT_JTI_OK
          : rc == SERVER_MANAGEMENT_JTI_REPLAY ? SERVER_MGMT_JTI_REPLAY
                                               : SERVER_MGMT_JTI_FAILED;
}

static int read_load(void *ctx, server_mgmt_read_agent_t *out, size_t cap, size_t *count)
{
   (void)ctx;
   return server_mgmt_read_load_agents(out, cap, count);
}

static int read_load_config(void *ctx, server_mgmt_read_config_t *out)
{
   (void)ctx;
   return server_mgmt_read_load_config(out);
}

static int64_t read_publication_generation(int64_t now)
{
   const char *path = getenv("AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE");
   char trust[SERVER_MGMT_JWKS_BUNDLE_MAX];
   size_t trust_len = 0;
   int64_t generation = 0;
   if (!path || server_mgmt_jwks_trust_bundle_load(path, trust, sizeof(trust), &trust_len) != 0 ||
       server_mgmt_jwks_cache_current_generation(trust, trust_len, now, &generation) !=
           SERVER_MGMT_JWKS_CACHE_OK)
      generation = 0;
   OPENSSL_cleanse(trust, sizeof(trust));
   return generation;
}

static server_mgmt_read_result_t read_checkpoint(void *ctx, const server_mgmt_read_request_t *rq,
                                                 const server_mgmt_token_claims_t *claims,
                                                 const server_mgmt_read_status_proof_t *proof)
{
   (void)ctx;
   server_mgmt_endpoint_request_t checkpoint = {
       .jwt = rq->jwt,
       .jwt_len = rq->jwt_len,
       .staple = rq->staple,
       .staple_len = rq->staple_len,
       .expected_issuer = rq->expected_issuer,
       .server_id = rq->server_id,
       .peer = rq->peer,
       .local_fingerprint = rq->local_fingerprint,
       .now = rq->now,
   };
   server_mgmt_checkpoint_result_t rc = server_mgmt_checkpoint_client_verify(
       &checkpoint, claims, proof->revocation_generation, proof->staple_sha256);
   return rc == SERVER_MGMT_CHECKPOINT_OK          ? SERVER_MGMT_READ_OK
          : rc == SERVER_MGMT_CHECKPOINT_DENIED    ? SERVER_MGMT_READ_FORBIDDEN
          : rc == SERVER_MGMT_CHECKPOINT_INTEGRITY ? SERVER_MGMT_READ_INTEGRITY
                                                   : SERVER_MGMT_READ_UNAVAILABLE;
}

int server_http_mgmt_read_error(server_mgmt_read_result_t result, char *resp, int cap)
{
   unsigned char random[32];
   char correlation[44];
   if (RAND_bytes(random, sizeof(random)) != 1 ||
       read_b64url(random, sizeof(random), correlation, sizeof(correlation)) != 0)
      memset(correlation, 'A', sizeof(correlation) - 1), correlation[sizeof(correlation) - 1] = 0;
   int status = result == SERVER_MGMT_READ_FORBIDDEN   ? 403
                : result == SERVER_MGMT_READ_CONFLICT  ? 409
                : result == SERVER_MGMT_READ_INTEGRITY ? 502
                                                       : 503;
   const char *code = result == SERVER_MGMT_READ_FORBIDDEN   ? "forbidden"
                      : result == SERVER_MGMT_READ_CONFLICT  ? "conflict"
                      : result == SERVER_MGMT_READ_INTEGRITY ? "integrity"
                                                             : "unavailable";
   const char *message = result == SERVER_MGMT_READ_FORBIDDEN   ? "Forbidden."
                         : result == SERVER_MGMT_READ_CONFLICT  ? "Request conflict."
                         : result == SERVER_MGMT_READ_INTEGRITY ? "Integrity verification failed."
                                                                : "Service unavailable.";
   snprintf(resp, (size_t)cap,
            "{\"error\":{\"code\":\"%s\",\"message\":\"%s\",\"correlation_id\":\"%s\"}}", code,
            message, correlation);
   return status;
}

static int server_http_mgmt_read(server_mgmt_read_selector_t selector, char *resp, int cap)
{
   if (server_http_management_action_begin() != 0)
      return server_http_mgmt_read_error(SERVER_MGMT_READ_UNAVAILABLE, resp, cap);
   const server_tls_peer_cert_t *peer = server_http_identity_peer_cert();
   const server_tls_peer_cert_t *local = server_http_identity_local_cert();
   char target_buf[128];
   const char *target =
       server_runtime_server_id_load(target_buf, sizeof(target_buf)) ? target_buf : NULL;
   const char *issuer = getenv("AIMEE_SERVER_MGMT_ISSUER");
   const char *jwt = server_http_identity_bearer();
   const char *staple = server_http_identity_status_staple();
   int64_t now = (int64_t)time(NULL);
   int64_t generation = read_publication_generation(now);
   server_mgmt_read_request_t request = {
       .jwt = jwt,
       .jwt_len = strlen(jwt),
       .staple = staple,
       .staple_len = strlen(staple),
       .expected_issuer = issuer,
       .server_id = target,
       .peer = peer,
       .local_issuer = local ? local->issuer : NULL,
       .local_serial = local ? local->serial_norm : NULL,
       .local_fingerprint = local ? local->fingerprint : NULL,
       .publication_generation = generation > 0 ? (uint64_t)generation : 0,
       .now = now,
       .selector = selector,
   };
   server_mgmt_read_deps_t deps = {
       .verify_and_consume_status = read_status,
       .verify_token = read_token,
       .consume_jti = read_jti,
       .load_agents = read_load,
       .load_config = read_load_config,
       .verify_checkpoint = read_checkpoint,
   };
   size_t response_len = 0;
   server_mgmt_read_result_t result =
       server_mgmt_read_dispatch(&request, &deps, resp, (size_t)cap, &response_len);
   (void)response_len;
   int status =
       result == SERVER_MGMT_READ_OK ? 200 : server_http_mgmt_read_error(result, resp, cap);
   server_http_management_action_end();
   return status;
}

int server_http_mgmt_read_agents(char *resp, int cap)
{
   return server_http_mgmt_read(SERVER_MGMT_READ_SELECTOR_AGENTS, resp, cap);
}

int server_http_mgmt_read_config(char *resp, int cap)
{
   return server_http_mgmt_read(SERVER_MGMT_READ_SELECTOR_CONFIG, resp, cap);
}
