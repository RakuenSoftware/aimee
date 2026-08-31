/* db1/mgmt_jwks_cache.h: the management-JWKS cache row.
 *
 * The verification around this cache -- loading the trust bundle, checking the
 * envelope signature, comparing digests -- stays in the daemon, which is where
 * outside-world trust belongs. Only the row moved.
 *
 * Digests cross as lowercase hex rather than as bytes. The frame carries
 * length-prefixed fields, but every payload kind it has is NUL-terminated text,
 * and a SHA-256 with a zero byte in it would arrive short -- then compare equal
 * to whatever followed it in the buffer, in the path that decides whether a
 * management token is trusted. Hex is a bijection with a self-checking length:
 * 32 bytes is 64 characters and nothing else is. The bytes themselves never
 * leave the two sides; only the spelling in between changed, and the daemon
 * still compares them with CRYPTO_memcmp.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_MGMT_JWKS_CACHE_H
#define DEC_DB1_MGMT_JWKS_CACHE_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_MGMT_JWKS_ENVELOPE_MAX 3072
#define DB1_MGMT_JWKS_BYTES_MAX    1024
/* Hex doubles, so a field carrying N bytes of hex needs 2N + 1. Sizing one of
   these for the raw length overflows it by exactly its own length, which is
   what the first version of this header did. */
#define DB1_MGMT_JWKS_BYTES_HEX (2 * DB1_MGMT_JWKS_BYTES_MAX + 1)
/* 32 bytes as hex, plus the NUL. */
#define DB1_MGMT_JWKS_DIGEST_HEX 65

   typedef struct
   {
      int64_t generation;
      int64_t valid_from;
      int64_t valid_until;
      char envelope[DB1_MGMT_JWKS_ENVELOPE_MAX];
      char envelope_sha256[DB1_MGMT_JWKS_DIGEST_HEX];
      char manifest_sha256[DB1_MGMT_JWKS_DIGEST_HEX];
      char trust_bundle_sha256[DB1_MGMT_JWKS_DIGEST_HEX];
   } db1_mgmt_jwks_row_t;

   typedef struct
   {
      int64_t valid_from;
      int64_t valid_until;
      int64_t fetched_at;
      char jwks[DB1_MGMT_JWKS_BYTES_HEX];
      char envelope[DB1_MGMT_JWKS_ENVELOPE_MAX];
      char envelope_sha256[DB1_MGMT_JWKS_DIGEST_HEX];
      char manifest_sha256[DB1_MGMT_JWKS_DIGEST_HEX];
      char trust_bundle_sha256[DB1_MGMT_JWKS_DIGEST_HEX];
   } db1_mgmt_jwks_install_t;

   /* Read the cached row. 1 when present, 0 when there is none, -1 on a store
    * failure -- three answers the caller acts on differently, so they stay
    * three. */
   int db1_mgmt_jwks_read(db1_mgmt_jwks_row_t *out);

   /* The cached generation, by the same three answers. */
   int db1_mgmt_jwks_generation(int64_t *out);

   /* Install the first row, or confirm the one already there is the same
    * envelope and bundle. 0 installed or already identical, 1 a DIFFERENT
    * envelope is cached (the caller must not overwrite it), -1 store failure.
    * The compare and the insert are one transaction: two servers installing at
    * once must not both believe they won. */
   int db1_mgmt_jwks_install(const db1_mgmt_jwks_install_t *in);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_MGMT_JWKS_CACHE_H */
