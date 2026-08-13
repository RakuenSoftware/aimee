#include "server/server_mgmt_checkpoint_client.h"
#include "db1.h"
#include "kb/kb_mgmt_client.h"
#include "kb_mgmt_status.h"
#include "server_mgmt_status.h"

#include <assert.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* Unused production transport seams; verify_with injects the fake below. */
int kb_mgmt_endpoint_validate(const char *e)
{
   return e && !strncmp(e, "https://", 8) ? 0 : -1;
}
int kb_mgmt_client_session_open_deadline(kb_mgmt_client_session_t *s, const char *a, const char *b,
                                         const char *c, const char *d, const char *e, const char *f,
                                         const char *g, uint64_t h, int i)
{
   (void)s;
   (void)a;
   (void)b;
   (void)c;
   (void)d;
   (void)e;
   (void)f;
   (void)g;
   (void)h;
   (void)i;
   return -1;
}
int kb_mgmt_client_session_checkpoint_deadline(kb_mgmt_client_session_t *s, const char *b,
                                               uint64_t d, char *r, size_t c, int *st)
{
   (void)s;
   (void)b;
   (void)d;
   (void)r;
   (void)c;
   (void)st;
   return -1;
}
void kb_mgmt_client_session_close(kb_mgmt_client_session_t *s)
{
   (void)s;
}
int kb_tls_peer_fingerprint(SSL *s, char *o, size_t c)
{
   (void)s;
   (void)o;
   (void)c;
   return -1;
}

typedef struct
{
   unsigned char private_key[32];
   int status;
   int transport_rc;
   int revoked;
   int generation_delta;
   int stale;
   int bad_digest;
   int bad_key;
   int bad_signature;
   int malformed_error;
   const char *expected_purpose;
   int calls;
} fake_t;

static void sha(const char *s, char out[65])
{
   unsigned char d[32];
   assert(SHA256((const unsigned char *)s, strlen(s), d));
   for (size_t i = 0; i < 32; i++)
      snprintf(out + i * 2, 3, "%02x", d[i]);
}

static int fake_transport(void *opaque, const server_mgmt_checkpoint_material_t *m,
                          const char *request, uint64_t deadline, char *response, size_t cap,
                          int *status)
{
   fake_t *f = opaque;
   f->calls++;
   assert(m && deadline > 0);
   const char *purpose = f->expected_purpose ? f->expected_purpose : "management.action.v1";
   char prefix[96];
   assert(snprintf(prefix, sizeof(prefix), "{\"version\":\"1\",\"purpose\":\"%s\"", purpose) > 0);
   assert(strstr(request, prefix) == request);
   if (f->transport_rc)
      return f->transport_rc;
   *status = f->status ? f->status : 200;
   if (*status != 200)
   {
      const char *body = *status == 400   ? "{\"error\":\"bad_request\"}"
                         : *status == 403 ? "{\"error\":\"denied\"}"
                         : *status == 409 ? "{\"error\":\"conflict\"}"
                                          : "{\"error\":\"unavailable\"}";
      snprintf(response, cap, "%s", f->malformed_error ? "{\"error\":\"nope\"}" : body);
      return 0;
   }
   char digest[65];
   sha(request, digest);
   time_t now = time(NULL);
   assert(now > 10);
   kb_mgmt_checkpoint_t checkpoint = {
       .version = 1,
       .revoked = f->revoked,
       .generation = (uint64_t)(7 + f->generation_delta),
       .issued_at = (uint64_t)(f->stale ? now - 10 : now - 1),
       .expires_at = (uint64_t)(f->stale ? now - 5 : now + 4),
   };
   snprintf(checkpoint.request_sha256, sizeof(checkpoint.request_sha256), "%s",
            f->bad_digest ? "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                          : digest);
   snprintf(checkpoint.key_id, sizeof(checkpoint.key_id), "%s", f->bad_key ? "wrong" : "key-1");
   assert(kb_mgmt_checkpoint_sign(&checkpoint, f->private_key) == 0);
   if (f->bad_signature)
      checkpoint.signature[0] ^= 1;
   assert(kb_mgmt_checkpoint_to_json(&checkpoint, response, cap) == 0);
   return 0;
}

static void make_staple(uint64_t generation, const char *purpose,
                        char out[KB_MGMT_STATUS_JSON_MAX + 1])
{
   unsigned char key[32] = {1};
   kb_mgmt_status_t s = {
       .version = 1, .issued_at = 95, .expires_at = 105, .revocation_generation = generation};
   memset(s.nonce, 7, sizeof(s.nonce));
   snprintf(s.key_id, sizeof(s.key_id), "key-1");
   snprintf(s.caller_issuer, sizeof(s.caller_issuer), "/CN=management-ca");
   snprintf(s.caller_serial_norm, sizeof(s.caller_serial_norm), "02");
   memset(s.caller_fingerprint, 'a', 64);
   s.caller_fingerprint[64] = 0;
   snprintf(s.target_server_id, sizeof(s.target_server_id), "server-1");
   memset(s.target_mgmt_fingerprint, 'b', 64);
   s.target_mgmt_fingerprint[64] = 0;
   snprintf(s.purpose, sizeof(s.purpose), "%s", purpose);
   assert(kb_mgmt_status_sign(&s, key) == 0);
   assert(kb_mgmt_status_to_json(&s, out, KB_MGMT_STATUS_JSON_MAX + 1) == 0);
}

int main(void)
{
   char a[65], b[65], c[65];
   memset(a, 'a', 64);
   memset(b, 'b', 64);
   memset(c, 'c', 64);
   a[64] = b[64] = c[64] = 0;
   assert(server_mgmt_checkpoint_pin_matches(a, a, NULL));
   assert(server_mgmt_checkpoint_pin_matches(b, a, b));
   assert(!server_mgmt_checkpoint_pin_matches(c, a, b));
   a[0] = 'A';
   assert(!server_mgmt_checkpoint_pin_matches(a, a, NULL));

   char dbpath[256];
   snprintf(dbpath, sizeof dbpath, "%s/aimee-checkpoint-client-XXXXXX", platform_tmpdir());
   int fd = mkstemp(dbpath);
   assert(fd >= 0);
   close(fd);
   assert(db1_init(dbpath) == 0 && server_mgmt_status_init() == 0);

   fake_t fake = {0};
   for (size_t i = 0; i < sizeof(fake.private_key); i++)
      fake.private_key[i] = (unsigned char)(i + 1);
   EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, fake.private_key, 32);
   unsigned char public_key[32];
   size_t public_len = sizeof(public_key);
   assert(pkey && EVP_PKEY_get_raw_public_key(pkey, public_key, &public_len) == 1);
   EVP_PKEY_free(pkey);
   server_mgmt_checkpoint_material_t material = {
       "https://authority", "ca", "cert", "key", b, NULL, "key-1", public_key};

   server_tls_peer_cert_t peer = {0};
   snprintf(peer.issuer, sizeof(peer.issuer), "/CN=management-ca");
   snprintf(peer.serial_norm, sizeof(peer.serial_norm), "02");
   memset(peer.fingerprint, 'a', 64);
   peer.fingerprint[64] = 0;
   char staple[KB_MGMT_STATUS_JSON_MAX + 1];
   make_staple(7, "management.action.v1", staple);
   server_mgmt_endpoint_request_t rq = {.staple = staple,
                                        .staple_len = strlen(staple),
                                        .server_id = "server-1",
                                        .peer = &peer,
                                        .now = 102};
   server_mgmt_token_claims_t claims = {0};
   memset(claims.correlation_id, 'c', 64);
   claims.correlation_id[64] = 0;
   memset(claims.jti, 'd', 64);
   claims.jti[64] = 0;
   memset(claims.request_sha256, 'e', 64);
   claims.request_sha256[64] = 0;
   char staple_digest[65];
   memset(staple_digest, 'f', 64);
   staple_digest[64] = 0;
   char canonical[4097], canonical_digest[65];
   assert(server_mgmt_checkpoint_request_build(&rq, &claims, 7, staple_digest, canonical,
                                               sizeof(canonical), canonical_digest) > 0);
   assert(strstr(canonical, "\"staple_generation\":\"7\""));
   assert(strlen(canonical_digest) == 64);

   assert(server_mgmt_checkpoint_client_verify_with(&material, fake_transport, &fake, &rq, &claims,
                                                    7, staple_digest) == SERVER_MGMT_CHECKPOINT_OK);
   uint64_t hwm = 0;
   assert(server_mgmt_status_hwm(&hwm) == 0 && hwm == 7);
   /* A highest-seen generation is a floor, not a single-use value. Multiple
    * independently authorized actions at the same generation remain valid. */
   assert(server_mgmt_checkpoint_client_verify_with(&material, fake_transport, &fake, &rq, &claims,
                                                    7, staple_digest) == SERVER_MGMT_CHECKPOINT_OK);
   fake.revoked = 1;
   assert(server_mgmt_checkpoint_client_verify_with(&material, fake_transport, &fake, &rq, &claims,
                                                    7, staple_digest) ==
          SERVER_MGMT_CHECKPOINT_DENIED);
   fake.revoked = 0;
   fake.bad_digest = 1;
   assert(server_mgmt_checkpoint_client_verify_with(&material, fake_transport, &fake, &rq, &claims,
                                                    7, staple_digest) ==
          SERVER_MGMT_CHECKPOINT_INTEGRITY);
   fake.bad_digest = 0;
   fake.bad_signature = 1;
   assert(server_mgmt_checkpoint_client_verify_with(&material, fake_transport, &fake, &rq, &claims,
                                                    7, staple_digest) ==
          SERVER_MGMT_CHECKPOINT_INTEGRITY);
   fake.bad_signature = 0;
   fake.stale = 1;
   assert(server_mgmt_checkpoint_client_verify_with(&material, fake_transport, &fake, &rq, &claims,
                                                    7, staple_digest) ==
          SERVER_MGMT_CHECKPOINT_INTEGRITY);
   fake.stale = 0;
   fake.bad_key = 1;
   assert(server_mgmt_checkpoint_client_verify_with(&material, fake_transport, &fake, &rq, &claims,
                                                    7, staple_digest) ==
          SERVER_MGMT_CHECKPOINT_INTEGRITY);
   fake.bad_key = 0;
   fake.generation_delta = 1;
   assert(server_mgmt_checkpoint_client_verify_with(&material, fake_transport, &fake, &rq, &claims,
                                                    7, staple_digest) ==
          SERVER_MGMT_CHECKPOINT_INTEGRITY);
   fake.generation_delta = 0;
   fake.status = 400;
   assert(server_mgmt_checkpoint_client_verify_with(&material, fake_transport, &fake, &rq, &claims,
                                                    7, staple_digest) ==
          SERVER_MGMT_CHECKPOINT_INTEGRITY);
   fake.status = 403;
   assert(server_mgmt_checkpoint_client_verify_with(&material, fake_transport, &fake, &rq, &claims,
                                                    7, staple_digest) ==
          SERVER_MGMT_CHECKPOINT_DENIED);
   fake.status = 409;
   assert(server_mgmt_checkpoint_client_verify_with(&material, fake_transport, &fake, &rq, &claims,
                                                    7, staple_digest) ==
          SERVER_MGMT_CHECKPOINT_INTEGRITY);
   fake.status = 503;
   assert(server_mgmt_checkpoint_client_verify_with(&material, fake_transport, &fake, &rq, &claims,
                                                    7, staple_digest) ==
          SERVER_MGMT_CHECKPOINT_UNAVAILABLE);
   fake.malformed_error = 1;
   assert(server_mgmt_checkpoint_client_verify_with(&material, fake_transport, &fake, &rq, &claims,
                                                    7, staple_digest) ==
          SERVER_MGMT_CHECKPOINT_INTEGRITY);
   fake.status = 0;
   fake.malformed_error = 0;
   fake.transport_rc = -2;
   assert(server_mgmt_checkpoint_client_verify_with(&material, fake_transport, &fake, &rq, &claims,
                                                    7, staple_digest) ==
          SERVER_MGMT_CHECKPOINT_INTEGRITY);

   fake.transport_rc = 0;
   fake.expected_purpose = "management.read.v1";
   snprintf(claims.capability, sizeof(claims.capability), "remote_reads");
   make_staple(7, "management.read.v1", staple);
   rq.staple_len = strlen(staple);
   assert(server_mgmt_checkpoint_client_verify_with(&material, fake_transport, &fake, &rq, &claims,
                                                    7, staple_digest) == SERVER_MGMT_CHECKPOINT_OK);

   fake.expected_purpose = "management.read.config.v1";
   make_staple(7, "management.read.config.v1", staple);
   rq.staple_len = strlen(staple);
   assert(server_mgmt_checkpoint_client_verify_with(&material, fake_transport, &fake, &rq, &claims,
                                                    7, staple_digest) == SERVER_MGMT_CHECKPOINT_OK);

   fake.expected_purpose = "management.read.v1";
   make_staple(6, "management.read.v1", staple);
   rq.staple_len = strlen(staple);
   fake.transport_rc = 0;
   fake.generation_delta = -1;
   assert(server_mgmt_checkpoint_client_verify_with(&material, fake_transport, &fake, &rq, &claims,
                                                    6, staple_digest) ==
          SERVER_MGMT_CHECKPOINT_INTEGRITY);
   assert(fake.calls == 17);
   assert(server_mgmt_status_hwm_advance(9) == 0);
   assert(server_mgmt_status_hwm_advance(8) == -1);
   assert(server_mgmt_status_hwm(&hwm) == 0 && hwm == 9);
   db1_shutdown();
   unlink(dbpath);
   puts("server management checkpoint client tests passed");
   return 0;
}
