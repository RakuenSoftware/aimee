/* db1/mgmt_nonce.h: management challenge nonces and the revocation high-water
 * mark.
 *
 * The peer certificate, the signature check and the decision that a request is
 * `valid` all stay in the daemon, which is where mTLS belongs. What moved is
 * the row: issuing a challenge, consuming it exactly once, and the generation
 * counter that stops a replayed status report rolling the server backwards.
 *
 * The nonce crosses as lowercase hex for the reason db1/mgmt_jwks_cache.h gives
 * for digests: it is 32 raw bytes, the wire carries NUL-terminated text, and a
 * nonce truncated at its first zero byte would match a DIFFERENT challenge --
 * one an attacker can make likely by asking for enough of them. 32 bytes is 64
 * hex characters and nothing else is.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_MGMT_NONCE_H
#define DEC_DB1_MGMT_NONCE_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* 32 bytes as hex, plus the NUL. */
#define DB1_MGMT_NONCE_HEX    65
#define DB1_MGMT_NONCE_TEXT   128
#define DB1_MGMT_NONCE_DIGEST 72
/* How many live challenges may exist at once, and how long one lives. Both
   belong with the row: the cap is enforced inside the issuing transaction, and
   enforcing it anywhere else would let two issuers pass the check together. */
#define DB1_MGMT_NONCE_CAP 128
#define DB1_MGMT_NONCE_TTL 15

   /* The answers a consume can give. They match the daemon's enum one for one
      and stay distinct because the caller acts differently on each: a mismatch
      is an attacker, an expiry is a slow client, a rollback is a replayed
      report, and storage is neither. */
   typedef enum
   {
      DB1_MGMT_NONCE_OK = 0,
      DB1_MGMT_NONCE_NOT_FOUND = 1,
      DB1_MGMT_NONCE_MISMATCH = 2,
      DB1_MGMT_NONCE_EXPIRED = 3,
      DB1_MGMT_NONCE_ROLLBACK = 4,
      DB1_MGMT_NONCE_INVALID = 5,
      DB1_MGMT_NONCE_SATURATED = 6,
      DB1_MGMT_NONCE_STORAGE = 7
   } db1_mgmt_nonce_result_t;

   typedef struct
   {
      char nonce[DB1_MGMT_NONCE_HEX];
      char peer_issuer[DB1_MGMT_NONCE_TEXT];
      char peer_serial_norm[DB1_MGMT_NONCE_TEXT];
      char peer_fingerprint[DB1_MGMT_NONCE_DIGEST];
      char channel_binding[DB1_MGMT_NONCE_DIGEST];
      char target_server_id[DB1_MGMT_NONCE_TEXT];
      char purpose[DB1_MGMT_NONCE_TEXT];
      int64_t now;
   } db1_mgmt_nonce_issue_t;

   typedef struct
   {
      char nonce[DB1_MGMT_NONCE_HEX];
      char peer_issuer[DB1_MGMT_NONCE_TEXT];
      char peer_serial_norm[DB1_MGMT_NONCE_TEXT];
      char peer_fingerprint[DB1_MGMT_NONCE_DIGEST];
      char channel_binding[DB1_MGMT_NONCE_DIGEST];
      char target_server_id[DB1_MGMT_NONCE_TEXT];
      char purpose[DB1_MGMT_NONCE_TEXT];
      int64_t now;
      int64_t revocation_generation;
      /* The daemon's verdict on the signature, passed in rather than repeated:
         this side does not verify, it records. */
      int valid;
   } db1_mgmt_nonce_consume_t;

   /* Drop challenges a restart made unanswerable, keeping the generation. */
   int db1_mgmt_nonce_clear(void);

   /* Record a challenge. OK, SATURATED when the cap is already reached, or
      STORAGE. The prune, the count and the insert are one transaction, so two
      issuers cannot both find room. Answers the enum.
    *
    * There is no expires_at out-parameter: it is `now + DB1_MGMT_NONCE_TTL`,
    * and both sides read that constant from this header, so a caller computing
    * it cannot disagree with the row that was written. */
   int db1_mgmt_nonce_issue(const db1_mgmt_nonce_issue_t *in);

   /* Consume a challenge exactly once and advance the high-water mark. The
      lookup, the delete and the advance are one transaction: a nonce that is
      read and then deleted separately can be spent twice. Answers the enum. */
   int db1_mgmt_nonce_consume(const db1_mgmt_nonce_consume_t *in);

   /* The revocation high-water mark. 0 on success, -1 when there is no row or
      the store failed. */
   int db1_mgmt_status_hwm_read(int64_t *generation);
   int db1_mgmt_status_hwm_set(int64_t generation);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_MGMT_NONCE_H */
