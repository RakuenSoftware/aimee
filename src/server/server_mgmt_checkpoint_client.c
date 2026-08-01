#define _POSIX_C_SOURCE 200809L
#include "server_mgmt_checkpoint_client.h"

#include "kb_client_management.h"
#include "kb_mgmt_endpoint.h"
#include "kb_mgmt_status.h"
#include "kb_tls.h"
#include "server_mgmt_status.h"
#include "runtime_secret.h"

#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct
{
   pthread_mutex_t mutex;
   pthread_cond_t idle;
   int configured;
   int stopping;
   unsigned active;
   char endpoint[512];
   char ca[SERVER_MGMT_CHECKPOINT_PEM_MAX + 1];
   char cert[SERVER_MGMT_CHECKPOINT_PEM_MAX + 1];
   char key[SERVER_MGMT_CHECKPOINT_PEM_MAX + 1];
   char pin[65];
   char secondary[65];
   char key_id[65];
   unsigned char public_key[32];
} checkpoint_runtime_t;

static checkpoint_runtime_t runtime = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .idle = PTHREAD_COND_INITIALIZER,
};

static uint64_t monotonic_ms(void)
{
   struct timespec ts;
   return clock_gettime(CLOCK_MONOTONIC, &ts) == 0
              ? (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000
              : UINT64_MAX;
}

static int lower_hex(const char *s, size_t n)
{
   if (!s || strnlen(s, n + 1) != n)
      return 0;
   for (size_t i = 0; i < n; i++)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static int ascii_token(const char *s, size_t max)
{
   size_t n = s ? strnlen(s, max + 1) : 0;
   if (!n || n > max)
      return 0;
   for (size_t i = 0; i < n; i++)
      if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') ||
            (s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == '_' || s[i] == '-'))
         return 0;
   return 1;
}

int server_mgmt_checkpoint_pin_matches(const char *actual, const char *primary,
                                       const char *secondary)
{
   return lower_hex(actual, 64) && lower_hex(primary, 64) &&
          (!secondary || !secondary[0] || lower_hex(secondary, 64)) &&
          (CRYPTO_memcmp(actual, primary, 64) == 0 ||
           (secondary && secondary[0] && CRYPTO_memcmp(actual, secondary, 64) == 0));
}

static int decode_key(const char *hex, unsigned char out[32])
{
   if (!lower_hex(hex, 64))
      return -1;
   for (size_t i = 0; i < 32; i++)
   {
      unsigned a = (unsigned char)hex[i * 2], b = (unsigned char)hex[i * 2 + 1];
      a = a <= '9' ? a - '0' : a - 'a' + 10;
      b = b <= '9' ? b - '0' : b - 'a' + 10;
      out[i] = (unsigned char)((a << 4) | b);
   }
   return 0;
}

static int read_root_file(const char *path, char *out, size_t cap)
{
   if (!path || path[0] != '/' || !out || cap < 2)
      return -1;
   int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
   struct stat st;
   if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
       (st.st_mode & (S_IWGRP | S_IWOTH)) || st.st_size < 1 || (uintmax_t)st.st_size >= cap)
   {
      if (fd >= 0)
         close(fd);
      return -1;
   }
   size_t used = 0;
   while (used < (size_t)st.st_size)
   {
      ssize_t n = read(fd, out + used, (size_t)st.st_size - used);
      if (n <= 0)
      {
         close(fd);
         OPENSSL_cleanse(out, cap);
         return -1;
      }
      used += (size_t)n;
   }
   char extra;
   int ok = read(fd, &extra, 1) == 0;
   close(fd);
   if (!ok)
   {
      OPENSSL_cleanse(out, cap);
      return -1;
   }
   out[used] = 0;
   return 0;
}

static int b64url(const unsigned char *in, size_t n, char *out, size_t cap)
{
   size_t need = 4 * ((n + 2) / 3);
   if (!in || !out || cap <= need || n > INT_MAX)
      return -1;
   int got = EVP_EncodeBlock((unsigned char *)out, in, (int)n);
   if (got < 1)
      return -1;
   for (int i = 0; i < got; i++)
      out[i] = out[i] == '+' ? '-' : (out[i] == '/' ? '_' : out[i]);
   while (got && out[got - 1] == '=')
      got--;
   out[got] = 0;
   return 0;
}

static void digest_hex(const unsigned char digest[32], char out[65])
{
   static const char h[] = "0123456789abcdef";
   for (size_t i = 0; i < 32; i++)
   {
      out[i * 2] = h[digest[i] >> 4];
      out[i * 2 + 1] = h[digest[i] & 15];
   }
   out[64] = 0;
}

int server_mgmt_checkpoint_request_build(const server_mgmt_endpoint_request_t *rq,
                                         const server_mgmt_token_claims_t *claims,
                                         uint64_t generation, const char *staple_digest, char *out,
                                         size_t cap, char digest[65])
{
   kb_mgmt_status_t staple;
   const char *purpose = NULL;
   char nonce[48], issuer[808];
   size_t serial_len =
       rq && rq->peer ? strnlen(rq->peer->serial_norm, sizeof(rq->peer->serial_norm)) : 0;
   size_t issuer_len = rq && rq->peer ? strnlen(rq->peer->issuer, sizeof(rq->peer->issuer)) : 0;
   if (out && cap)
      out[0] = 0;
   if (digest)
      digest[0] = 0;
   if (!rq || !claims || !rq->staple || !rq->peer || !out || !cap || !digest ||
       !lower_hex(staple_digest, 64) || !lower_hex(claims->correlation_id, 64) ||
       !lower_hex(claims->jti, 64) || !lower_hex(claims->request_sha256, 64) || serial_len < 1 ||
       serial_len >= sizeof(rq->peer->serial_norm) ||
       !lower_hex(rq->peer->serial_norm, serial_len) || issuer_len < 1 ||
       issuer_len >= sizeof(rq->peer->issuer) || !ascii_token(rq->server_id, 127) ||
       rq->staple_len < 1 || rq->staple_len > KB_MGMT_STATUS_JSON_MAX ||
       strnlen(rq->staple, rq->staple_len + 1) != rq->staple_len ||
       !lower_hex(rq->peer->fingerprint, 64) ||
       kb_mgmt_status_from_json(rq->staple, &staple) != 0 ||
       staple.revocation_generation != generation ||
       b64url(staple.nonce, sizeof(staple.nonce), nonce, sizeof(nonce)) ||
       b64url((const unsigned char *)rq->peer->issuer, issuer_len, issuer, sizeof(issuer)))
      return -1;
   if (!strcmp(claims->capability, "remote_reads") &&
       (!strcmp(staple.purpose, "management.read.v1") ||
        !strcmp(staple.purpose, "management.read.config.v1")))
      purpose = staple.purpose;
   else if (strcmp(claims->capability, "remote_reads") &&
            !strcmp(staple.purpose, "management.action.v1"))
      purpose = staple.purpose;
   else
      return -1;
   int n = snprintf(out, cap,
                    "{\"version\":\"1\",\"purpose\":\"%s\",\"nonce\":\"%s\","
                    "\"caller_issuer_b64\":\"%s\",\"caller_serial\":\"%s\","
                    "\"caller_fingerprint\":\"%s\",\"target\":\"%s\","
                    "\"staple_generation\":\"%llu\",\"staple_sha256\":\"%s\","
                    "\"correlation_id\":\"%s\",\"jti\":\"%s\",\"request_sha256\":\"%s\"}",
                    purpose, nonce, issuer, rq->peer->serial_norm, rq->peer->fingerprint,
                    rq->server_id, (unsigned long long)generation, staple_digest,
                    claims->correlation_id, claims->jti, claims->request_sha256);
   unsigned char raw[32];
   if (n < 0 || (size_t)n >= cap || (size_t)n > KB_MGMT_CHECKPOINT_JSON_MAX ||
       !SHA256((const unsigned char *)out, (size_t)n, raw))
   {
      out[0] = 0;
      return -1;
   }
   digest_hex(raw, digest);
   return n;
}

static int real_transport(void *ctx, const server_mgmt_checkpoint_material_t *m, const char *body,
                          uint64_t deadline, char *response, size_t cap, int *status)
{
   (void)ctx;
   kb_mgmt_client_session_t session;
   if (kb_mgmt_client_session_open_deadline(&session, m->endpoint, m->ca_pem, m->client_cert_pem,
                                            m->client_key_pem, NULL, NULL, NULL, deadline, 1))
      return -1;
   char actual[65] = {0};
   int pinned = kb_tls_peer_fingerprint(session.ssl, actual, sizeof(actual)) == 0 &&
                server_mgmt_checkpoint_pin_matches(actual, m->leaf_pin, m->secondary_leaf_pin);
   int rc = pinned ? kb_mgmt_client_session_checkpoint_deadline(&session, body, deadline, response,
                                                                cap, status)
                   : -2;
   kb_mgmt_client_session_close(&session);
   OPENSSL_cleanse(actual, sizeof(actual));
   return rc;
}

server_mgmt_checkpoint_result_t server_mgmt_checkpoint_client_verify_with(
    const server_mgmt_checkpoint_material_t *m, server_mgmt_checkpoint_transport_fn transport,
    void *transport_ctx, const server_mgmt_endpoint_request_t *rq,
    const server_mgmt_token_claims_t *claims, uint64_t generation, const char *staple_digest)
{
   char request[KB_MGMT_CHECKPOINT_JSON_MAX + 1], request_digest[65];
   char response[KB_MGMT_CHECKPOINT_JSON_MAX + 1] = {0};
   if (!m || !transport || !m->endpoint || !m->ca_pem || !m->client_cert_pem ||
       !m->client_key_pem || !m->key_id || !m->public_key ||
       server_mgmt_checkpoint_request_build(rq, claims, generation, staple_digest, request,
                                            sizeof(request), request_digest) < 0)
      return SERVER_MGMT_CHECKPOINT_UNAVAILABLE;
   uint64_t now_ms = monotonic_ms();
   if (now_ms == UINT64_MAX || now_ms > UINT64_MAX - 5000)
      return SERVER_MGMT_CHECKPOINT_UNAVAILABLE;
   int status = 0;
   int tr =
       transport(transport_ctx, m, request, now_ms + 5000, response, sizeof(response), &status);
   if (tr == -2)
      return SERVER_MGMT_CHECKPOINT_INTEGRITY;
   if (tr != 0)
      return SERVER_MGMT_CHECKPOINT_UNAVAILABLE;
   if (status != 200)
   {
      const char *expected = status == 400   ? "{\"error\":\"bad_request\"}"
                             : status == 403 ? "{\"error\":\"denied\"}"
                             : status == 409 ? "{\"error\":\"conflict\"}"
                             : status == 503 ? "{\"error\":\"unavailable\"}"
                                             : NULL;
      if (!expected || strcmp(response, expected))
         return SERVER_MGMT_CHECKPOINT_INTEGRITY;
      return status == 503   ? SERVER_MGMT_CHECKPOINT_UNAVAILABLE
             : status == 403 ? SERVER_MGMT_CHECKPOINT_DENIED
                             : SERVER_MGMT_CHECKPOINT_INTEGRITY;
   }
   kb_mgmt_checkpoint_t checkpoint;
   uint64_t hwm = 0;
   time_t wall_now = time(NULL);
   if (kb_mgmt_checkpoint_from_json(response, &checkpoint) || wall_now < 0 ||
       server_mgmt_status_hwm(&hwm) != 0 ||
       kb_mgmt_checkpoint_validate(&checkpoint, (uint64_t)wall_now, hwm) != 0 ||
       strcmp(checkpoint.key_id, m->key_id) ||
       kb_mgmt_checkpoint_verify_signature(&checkpoint, m->public_key) != 0 ||
       CRYPTO_memcmp(checkpoint.request_sha256, request_digest, 64) != 0 ||
       checkpoint.generation != generation)
      return SERVER_MGMT_CHECKPOINT_INTEGRITY;
   if (checkpoint.revoked)
      return SERVER_MGMT_CHECKPOINT_DENIED;
   if (server_mgmt_status_hwm_advance(checkpoint.generation) != 0)
      return SERVER_MGMT_CHECKPOINT_UNAVAILABLE;
   return SERVER_MGMT_CHECKPOINT_OK;
}

int server_mgmt_checkpoint_client_start(const server_http_management_config_t *c)
{
   pthread_mutex_lock(&runtime.mutex);
   if (runtime.configured || runtime.active || runtime.stopping)
   {
      pthread_mutex_unlock(&runtime.mutex);
      return -1;
   }
   if (!c || !c->enabled)
   {
      pthread_mutex_unlock(&runtime.mutex);
      return 0;
   }
   int ok = kb_mgmt_endpoint_validate(c->status_endpoint) == 0 &&
            lower_hex(c->status_leaf_pin, 64) &&
            (!c->status_secondary_leaf_pin[0] || lower_hex(c->status_secondary_leaf_pin, 64)) &&
            c->status_key_id[0] && strlen(c->status_key_id) <= 64 &&
            decode_key(c->status_public_key, runtime.public_key) == 0 &&
            read_root_file(c->status_ca, runtime.ca, sizeof(runtime.ca)) == 0 &&
            read_root_file(c->status_client_cert, runtime.cert, sizeof(runtime.cert)) == 0 &&
            runtime_secret_get(c->status_client_key, runtime.key, sizeof(runtime.key));
   if (ok)
   {
      snprintf(runtime.endpoint, sizeof(runtime.endpoint), "%s", c->status_endpoint);
      snprintf(runtime.pin, sizeof(runtime.pin), "%s", c->status_leaf_pin);
      snprintf(runtime.secondary, sizeof(runtime.secondary), "%s", c->status_secondary_leaf_pin);
      snprintf(runtime.key_id, sizeof(runtime.key_id), "%s", c->status_key_id);
      runtime.configured = 1;
   }
   else
   {
      OPENSSL_cleanse(runtime.ca, sizeof(runtime.ca));
      OPENSSL_cleanse(runtime.cert, sizeof(runtime.cert));
      OPENSSL_cleanse(runtime.key, sizeof(runtime.key));
      OPENSSL_cleanse(runtime.public_key, sizeof(runtime.public_key));
   }
   pthread_mutex_unlock(&runtime.mutex);
   return ok ? 0 : -1;
}

void server_mgmt_checkpoint_client_stop(void)
{
   pthread_mutex_lock(&runtime.mutex);
   runtime.stopping = 1;
   while (runtime.active)
      pthread_cond_wait(&runtime.idle, &runtime.mutex);
   OPENSSL_cleanse(runtime.ca, sizeof(runtime.ca));
   OPENSSL_cleanse(runtime.cert, sizeof(runtime.cert));
   OPENSSL_cleanse(runtime.key, sizeof(runtime.key));
   OPENSSL_cleanse(runtime.public_key, sizeof(runtime.public_key));
   memset(runtime.endpoint, 0, sizeof(runtime.endpoint));
   memset(runtime.pin, 0, sizeof(runtime.pin));
   memset(runtime.secondary, 0, sizeof(runtime.secondary));
   memset(runtime.key_id, 0, sizeof(runtime.key_id));
   runtime.configured = 0;
   runtime.stopping = 0;
   pthread_mutex_unlock(&runtime.mutex);
}

server_mgmt_checkpoint_result_t
server_mgmt_checkpoint_client_verify(const server_mgmt_endpoint_request_t *rq,
                                     const server_mgmt_token_claims_t *claims, uint64_t generation,
                                     const char *staple_digest)
{
   pthread_mutex_lock(&runtime.mutex);
   if (!runtime.configured || runtime.stopping)
   {
      pthread_mutex_unlock(&runtime.mutex);
      return SERVER_MGMT_CHECKPOINT_UNAVAILABLE;
   }
   runtime.active++;
   server_mgmt_checkpoint_material_t material = {
       runtime.endpoint, runtime.ca,         runtime.cert,
       runtime.key,      runtime.pin,        runtime.secondary[0] ? runtime.secondary : NULL,
       runtime.key_id,   runtime.public_key,
   };
   pthread_mutex_unlock(&runtime.mutex);
   server_mgmt_checkpoint_result_t result = server_mgmt_checkpoint_client_verify_with(
       &material, real_transport, NULL, rq, claims, generation, staple_digest);
   pthread_mutex_lock(&runtime.mutex);
   runtime.active--;
   if (!runtime.active)
      pthread_cond_broadcast(&runtime.idle);
   pthread_mutex_unlock(&runtime.mutex);
   return result;
}
