/* modules/aws/sts_cache.c: instance-local STS session cache. See sts_cache.h.
 * Pure/offline; now passed in; bounded + generation/TTL invalidation. */

#include "sts_cache.h"

#include "aws_sigv4.h" /* aws_sha256_hex */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sts_cache_key_init(sts_cache_key_t *k)
{
   if (k)
      memset(k, 0, sizeof(*k));
}

static int cmp_str(const void *a, const void *b)
{
   return strcmp((const char *)a, (const char *)b);
}

int sts_cache_region_set(sts_cache_key_t *k, const char *const *regions, size_t n)
{
   if (!k)
      return -1;
   k->region_set[0] = '\0';
   if (n == 0)
      return 0;
   if (n > 32)
      return -1;
   char sorted[32][64];
   for (size_t i = 0; i < n; i++)
   {
      if (!regions[i] || strlen(regions[i]) >= sizeof(sorted[0]))
         return -1;
      snprintf(sorted[i], sizeof(sorted[0]), "%s", regions[i]);
   }
   qsort(sorted, n, sizeof(sorted[0]), cmp_str);
   size_t o = 0;
   for (size_t i = 0; i < n; i++)
   {
      int w =
          snprintf(k->region_set + o, sizeof(k->region_set) - o, "%s%s", i ? "," : "", sorted[i]);
      if (w < 0 || (size_t)w >= sizeof(k->region_set) - o)
      {
         k->region_set[0] = '\0';
         return -1;
      }
      o += (size_t)w;
   }
   return 0;
}

void sts_cache_set_policy_hash(sts_cache_key_t *k, const char *policy_json)
{
   if (!k)
      return;
   aws_sha256_hex((const unsigned char *)(policy_json ? policy_json : ""),
                  policy_json ? strlen(policy_json) : 0, k->policy_hash);
}

int sts_cache_key_eq(const sts_cache_key_t *a, const sts_cache_key_t *b)
{
   if (!a || !b)
      return 0;
   return a->federation_mode == b->federation_mode &&
          a->credential_generation == b->credential_generation &&
          strcmp(a->org_team, b->org_team) == 0 && strcmp(a->key_slot, b->key_slot) == 0 &&
          strcmp(a->role_arn, b->role_arn) == 0 &&
          strcmp(a->role_session_name, b->role_session_name) == 0 &&
          strcmp(a->external_id, b->external_id) == 0 && strcmp(a->issuer, b->issuer) == 0 &&
          strcmp(a->audience, b->audience) == 0 && strcmp(a->subject, b->subject) == 0 &&
          strcmp(a->partition, b->partition) == 0 && strcmp(a->region_set, b->region_set) == 0 &&
          strcmp(a->target_id, b->target_id) == 0 && strcmp(a->policy_hash, b->policy_hash) == 0;
}

void sts_cache_init(sts_cache_t *c)
{
   if (c)
      memset(c, 0, sizeof(*c));
}

int sts_cache_put(sts_cache_t *c, const sts_cache_key_t *key, const aws_sts_credentials_t *creds,
                  long now, long expiration, long generation)
{
   if (!c || !key || !creds)
      return -1;
   /* Clamp the stored expiry to the hard TTL ceiling. */
   long ceiling = now + STS_CACHE_TTL_MAX;
   if (expiration > ceiling)
      expiration = ceiling;

   /* Replace an existing entry with the same key. */
   int slot = -1;
   for (int i = 0; i < STS_CACHE_MAX; i++)
      if (c->entries[i].used && sts_cache_key_eq(&c->entries[i].key, key))
      {
         slot = i;
         break;
      }
   /* Else take a free slot, or evict an expired/soonest-to-expire entry. */
   if (slot < 0)
      for (int i = 0; i < STS_CACHE_MAX; i++)
         if (!c->entries[i].used)
         {
            slot = i;
            break;
         }
   if (slot < 0)
   {
      long soonest = 0;
      for (int i = 0; i < STS_CACHE_MAX; i++)
      {
         if (c->entries[i].expiration <= now) /* expired: evict immediately */
         {
            slot = i;
            break;
         }
         if (slot < 0 || c->entries[i].expiration < soonest)
         {
            slot = i;
            soonest = c->entries[i].expiration;
         }
      }
   }
   if (slot < 0)
      return -1;
   c->entries[slot].used = 1;
   c->entries[slot].key = *key;
   c->entries[slot].creds = *creds;
   c->entries[slot].expiration = expiration;
   c->entries[slot].generation = generation;
   return 0;
}

const aws_sts_credentials_t *sts_cache_get(sts_cache_t *c, const sts_cache_key_t *key, long now,
                                           long current_generation)
{
   if (!c || !key)
      return NULL;
   for (int i = 0; i < STS_CACHE_MAX; i++)
   {
      const sts_cache_entry_t *e = &c->entries[i];
      if (!e->used)
         continue;
      if (!sts_cache_key_eq(&e->key, key))
         continue;
      if (now >= e->expiration)
         return NULL; /* TTL expiry -> miss */
      if (e->generation != current_generation)
         return NULL; /* generation bump -> miss */
      return &e->creds;
   }
   return NULL;
}
