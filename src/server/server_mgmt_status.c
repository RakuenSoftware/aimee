#include "server_mgmt_status.h"
#include "mgmt_nonce.h"
#include "db1_internal.h"

#include <openssl/rand.h>
#include <sqlite3.h>
#include <string.h>

#define NONCE_CAP 128
#define NONCE_TTL 15

/* Bytes to lowercase hex. The nonce is 32 raw bytes here and stays that way for
   the caller; the store speaks hex because the wire has no bytes. */
static void nonce_hex(const unsigned char *bytes, size_t len, char *out)
{
   static const char digits[] = "0123456789abcdef";
   for (size_t i = 0; i < len; i++)
   {
      out[i * 2] = digits[bytes[i] >> 4];
      out[i * 2 + 1] = digits[bytes[i] & 0x0f];
   }
   out[len * 2] = '\0';
}

static int purpose_known(const char *purpose)
{
   return purpose &&
          (!strcmp(purpose, "management.health.v1") || !strcmp(purpose, "management.action.v1") ||
           !strcmp(purpose, "management.read.v1") || !strcmp(purpose, "management.read.config.v1"));
}

int server_mgmt_status_init(void)
{
   return db1_mgmt_nonce_clear();
}

int server_mgmt_nonce_issue_purpose(const server_tls_peer_cert_t *p, const char *target,
                                    const char *purpose, uint64_t now,
                                    unsigned char nonce[KB_MGMT_STATUS_NONCE_LEN],
                                    uint64_t *expires)
{
   /* Everything about the peer is checked here, where the certificate is. */
   if (!p || !p->issuer[0] || !p->serial_norm[0] || strlen(p->fingerprint) != 64 ||
       strlen(p->channel_binding) != 64 || !target || !target[0] || strlen(target) > 127 ||
       !purpose_known(purpose) || !nonce || !expires ||
       now > (uint64_t)INT64_MAX - DB1_MGMT_NONCE_TTL || RAND_bytes(nonce, 32) != 1)
      return SERVER_MGMT_NONCE_INVALID;

   db1_mgmt_nonce_issue_t in;
   memset(&in, 0, sizeof in);
   nonce_hex(nonce, 32, in.nonce);
   snprintf(in.peer_issuer, sizeof in.peer_issuer, "%s", p->issuer);
   snprintf(in.peer_serial_norm, sizeof in.peer_serial_norm, "%s", p->serial_norm);
   snprintf(in.peer_fingerprint, sizeof in.peer_fingerprint, "%s", p->fingerprint);
   snprintf(in.channel_binding, sizeof in.channel_binding, "%s", p->channel_binding);
   snprintf(in.target_server_id, sizeof in.target_server_id, "%s", target);
   snprintf(in.purpose, sizeof in.purpose, "%s", purpose);
   in.now = (int64_t)now;

   int rc = db1_mgmt_nonce_issue(&in);
   if (rc == DB1_MGMT_NONCE_OK)
   {
      *expires = now + DB1_MGMT_NONCE_TTL;
      return SERVER_MGMT_NONCE_OK;
   }
   return rc == DB1_MGMT_NONCE_SATURATED ? SERVER_MGMT_NONCE_SATURATED : SERVER_MGMT_NONCE_STORAGE;
}

int server_mgmt_nonce_issue(const server_tls_peer_cert_t *p, const char *target, uint64_t now,
                            unsigned char nonce[KB_MGMT_STATUS_NONCE_LEN], uint64_t *expires)
{
   return server_mgmt_nonce_issue_purpose(p, target, "management.health.v1", now, nonce, expires);
}

server_mgmt_nonce_result_t
server_mgmt_nonce_consume_purpose(const kb_mgmt_status_t *st, const server_tls_peer_cert_t *p,
                                  const char *target, const char *purpose, uint64_t now, int valid)
{
   if (!st || !p || !target || !purpose_known(purpose) || now > (uint64_t)INT64_MAX ||
       st->revocation_generation > (uint64_t)INT64_MAX)
      return SERVER_MGMT_NONCE_INVALID;

   db1_mgmt_nonce_consume_t in;
   memset(&in, 0, sizeof in);
   nonce_hex(st->nonce, 32, in.nonce);
   snprintf(in.peer_issuer, sizeof in.peer_issuer, "%s", p->issuer);
   snprintf(in.peer_serial_norm, sizeof in.peer_serial_norm, "%s", p->serial_norm);
   snprintf(in.peer_fingerprint, sizeof in.peer_fingerprint, "%s", p->fingerprint);
   snprintf(in.channel_binding, sizeof in.channel_binding, "%s", p->channel_binding);
   snprintf(in.target_server_id, sizeof in.target_server_id, "%s", target);
   snprintf(in.purpose, sizeof in.purpose, "%s", purpose);
   in.now = (int64_t)now;
   in.revocation_generation = (int64_t)st->revocation_generation;
   in.valid = valid;

   /* One for one with the store's answers: every one of these means something
      different to the caller, so none of them may collapse into another. */
   switch (db1_mgmt_nonce_consume(&in))
   {
   case DB1_MGMT_NONCE_OK:
      return SERVER_MGMT_NONCE_OK;
   case DB1_MGMT_NONCE_NOT_FOUND:
      return SERVER_MGMT_NONCE_NOT_FOUND;
   case DB1_MGMT_NONCE_MISMATCH:
      return SERVER_MGMT_NONCE_MISMATCH;
   case DB1_MGMT_NONCE_EXPIRED:
      return SERVER_MGMT_NONCE_EXPIRED;
   case DB1_MGMT_NONCE_ROLLBACK:
      return SERVER_MGMT_NONCE_ROLLBACK;
   case DB1_MGMT_NONCE_INVALID:
      return SERVER_MGMT_NONCE_INVALID;
   default:
      return SERVER_MGMT_NONCE_STORAGE;
   }
}

server_mgmt_nonce_result_t server_mgmt_nonce_consume(const kb_mgmt_status_t *st,
                                                     const server_tls_peer_cert_t *p,
                                                     const char *target, uint64_t now, int valid)
{
   return server_mgmt_nonce_consume_purpose(st, p, target, "management.health.v1", now, valid);
}

int server_mgmt_status_hwm(uint64_t *generation)
{
   if (!generation)
      return -1;
   int64_t found = 0;
   if (db1_mgmt_status_hwm_read(&found) != 0 || found < 0)
      return -1;
   *generation = (uint64_t)found;
   return 0;
}

int server_mgmt_status_hwm_advance(uint64_t generation)
{
   if (generation > (uint64_t)INT64_MAX)
      return -1;
   return db1_mgmt_status_hwm_set((int64_t)generation);
}
