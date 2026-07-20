/* modules/aws/sts_cache.h: instance-local, NON-authoritative STS session cache.
 *
 * A bounded in-memory cache of minted STS sessions, keyed on a tenant-and-policy-
 * COMPLETE key so a session minted for one identity/policy is NEVER returned for
 * any different value. The key includes: federation mode, org/team, provider
 * key-slot + credential generation, RoleArn, RoleSessionName, ExternalId (mode b)
 * OR {issuer,audience,subject} (mode a), AWS partition + SORTED region set, target
 * id, and the normalized session-policy SHA-256 hash.
 *
 * A hit requires an EXACT key match AND now < expiration AND generation == the
 * current generation. A generation bump (rotation / entitlement revocation) or TTL
 * expiry invalidates. Pure/offline: `now` is passed in (no clock inside). Depends
 * on libc + OpenSSL (SHA-256 for the policy-hash helper). */
#ifndef DEC_STS_CACHE_H
#define DEC_STS_CACHE_H 1

#include "aws_sts.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define STS_CACHE_MAX     64
#define STS_CACHE_TTL_MAX 900 /* hard ceiling: a session is never live > 900s */

   typedef enum
   {
      STS_FED_ASSUME_ROLE = 0,  /* mode b (ExternalId) */
      STS_FED_WEB_IDENTITY = 1, /* mode a (issuer/audience/subject) */
   } sts_fed_mode_t;

   /* The complete cache key. Populate every field for the mode; the unused-mode
    * fields stay empty (""), and are still compared (so a mode switch misses). Use
    * sts_cache_key_init to zero it, and sts_cache_region_set to build the sorted
    * region string. */
   typedef struct
   {
      int federation_mode; /* sts_fed_mode_t */
      char org_team[128];
      char key_slot[64];
      long credential_generation;
      char role_arn[256];
      char role_session_name[128];
      char external_id[256]; /* mode b */
      char issuer[512];      /* mode a */
      char audience[512];    /* mode a */
      char subject[256];     /* mode a */
      char partition[16];
      char region_set[512]; /* sorted, comma-joined (via sts_cache_region_set) */
      char target_id[256];
      char policy_hash[65]; /* SHA-256 hex of the canonical session policy */
   } sts_cache_key_t;

   typedef struct
   {
      int used;
      sts_cache_key_t key;
      aws_sts_credentials_t creds;
      long expiration;
      long generation;
   } sts_cache_entry_t;

   typedef struct
   {
      sts_cache_entry_t entries[STS_CACHE_MAX];
   } sts_cache_t;

   /* Zero a key (all fields empty). */
   void sts_cache_key_init(sts_cache_key_t *k);

   /* Build the sorted, comma-joined region string into k->region_set from `regions`
    * (n entries). Returns 0 on success, -1 on overflow/too many. */
   int sts_cache_region_set(sts_cache_key_t *k, const char *const *regions, size_t n);

   /* Set k->policy_hash to the SHA-256 hex of `policy_json`. */
   void sts_cache_set_policy_hash(sts_cache_key_t *k, const char *policy_json);

   /* Exact field-by-field key equality (constant-shape; no memcmp of padding). */
   int sts_cache_key_eq(const sts_cache_key_t *a, const sts_cache_key_t *b);

   void sts_cache_init(sts_cache_t *c);

   /* Insert/replace the session for `key`. `expiration` is the absolute expiry; it is
    * clamped so the entry never lives longer than STS_CACHE_TTL_MAX past `now`.
    * Returns 0 on success, -1 on invalid args. */
   int sts_cache_put(sts_cache_t *c, const sts_cache_key_t *key, const aws_sts_credentials_t *creds,
                     long now, long expiration, long generation);

   /* Return the live credentials for `key` ONLY on an exact key match AND now <
    * expiration AND entry.generation == current_generation; else NULL (miss). */
   const aws_sts_credentials_t *sts_cache_get(sts_cache_t *c, const sts_cache_key_t *key, long now,
                                              long current_generation);

#ifdef __cplusplus
}
#endif

#endif /* DEC_STS_CACHE_H */
