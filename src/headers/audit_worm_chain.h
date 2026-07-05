/* audit_worm_chain.h: the pure, engine-agnostic WORM chain primitives shared by
 * the aimee-server SQLite store (audit_worm.c) and the aimee-kb Postgres store
 * (db2/kb_audit_worm.c), so both produce BYTE-IDENTICAL row hashes + checkpoint
 * MACs (R1-1: shared code + test vectors). No storage engine dependency — just
 * SHA-256 (wfe), dstr, and the chain key file. */
#ifndef AIMEE_AUDIT_WORM_CHAIN_H
#define AIMEE_AUDIT_WORM_CHAIN_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Domain-separation + algorithm tag folded into every row_hash. Bump to version
 * the canonicalization/hash. */
#define AUDIT_WORM_DOMAIN "aimee.audit.worm.v1"
/* Genesis prev_hash: 32 zero bytes as 64 lowercase hex chars. */
#define AUDIT_WORM_GENESIS_PREV "0000000000000000000000000000000000000000000000000000000000000000"
/* Hard cap on a row's detail payload (bytes); oversized detail is truncated with a
 * marker folded into the hash (R2-8). */
#define AUDIT_WORM_DETAIL_MAX 16384

   /* 32 raw bytes -> 64 lowercase hex chars + NUL. */
   void audit_worm_hex32(const unsigned char in[32], char out[65]);

   /* row_hash = SHA256( DOMAIN "\n" prev_hash "\n" <length-prefixed fixed fields> ),
    * fields in the fixed order (seq, actor_role, actor_principal, action, subject,
    * verdict, key_id, detail). ts is deliberately excluded (seq is the sole ordering
    * authority). Length-prefixing makes the encoding injective. out_hex >= 65. */
   void audit_worm_row_hash(long long seq, const char *actor_role, const char *actor_principal,
                            const char *action, const char *subject, const char *verdict,
                            const char *key_id, const char *detail, const char *prev_hash,
                            char out_hex[65]);

   /* MAC a checkpoint commits over: the head (hash + seq) it attests, under key_id.
    * out_hex >= 65. */
   void audit_worm_ckpt_mac(const unsigned char key[32], const char *head_hash, long long head_seq,
                            const char *key_id, char out_hex[65]);

   /* Load (creating from CSPRNG on first use) the dedicated chain key at
    * $AIMEE_HOME/.audit-chain-key (0600), distinct from the args-hash .audit-key.
    * key_id = first 16 hex of SHA256(key). Returns 0 on success, -1 on failure. */
   int audit_worm_chain_key_load(unsigned char key[32], char key_id[17]);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_AUDIT_WORM_CHAIN_H */
