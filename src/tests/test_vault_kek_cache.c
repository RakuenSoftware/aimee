/* test_vault_kek_cache.c: WP-C.1 D14 — the principal-keyed KEK cache. Pins the
 * properties the vault's locking model relies on: byte-exact binary KEK storage
 * (embedded 0x00), TTL expiry-on-access, reject-don't-evict at capacity, and
 * cleanse-on-evict. */
#include "vault_kek_cache.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void fill_kek(uint8_t kek[VAULT_KEK_LEN], unsigned seed)
{
   for (int i = 0; i < VAULT_KEK_LEN; i++)
      kek[i] = (uint8_t)(seed + i);
}

/* The KEK is stored and returned byte-exact, including an embedded NUL — a
 * string store (forge broker) would truncate here. */
static void test_binary_exact_roundtrip(void)
{
   vault_kek_cache_clear();
   uint8_t kek[VAULT_KEK_LEN];
   fill_kek(kek, 1);
   kek[7] = 0x00; /* embedded NUL */
   kek[8] = 0x00;
   assert(vault_kek_cache_put("uid:1000", kek, 1000) == 0);

   uint8_t out[VAULT_KEK_LEN];
   memset(out, 0xff, sizeof(out));
   assert(vault_kek_cache_get("uid:1000", 1001, out) == 0);
   assert(memcmp(out, kek, VAULT_KEK_LEN) == 0);

   /* Absent principal misses, output cleansed. */
   memset(out, 0xff, sizeof(out));
   assert(vault_kek_cache_get("uid:2000", 1001, out) == -1);
   uint8_t zero[VAULT_KEK_LEN] = {0};
   assert(memcmp(out, zero, VAULT_KEK_LEN) == 0);
   vault_kek_cache_clear();
   printf("  PASS: test_binary_exact_roundtrip\n");
}

static void test_ttl_expiry_on_access(void)
{
   vault_kek_cache_clear();
   uint8_t kek[VAULT_KEK_LEN];
   fill_kek(kek, 5);
   long t0 = 10000;
   assert(vault_kek_cache_put("uid:1000", kek, t0) == 0);

   uint8_t out[VAULT_KEK_LEN];
   /* Just before TTL: still live. */
   assert(vault_kek_cache_get("uid:1000", t0 + VAULT_KEK_CACHE_TTL_SECONDS - 1, out) == 0);
   /* At/after TTL: expired, evicted, fail-closed. */
   assert(vault_kek_cache_get("uid:1000", t0 + VAULT_KEK_CACHE_TTL_SECONDS, out) == -1);
   /* And it is gone, not merely hidden. */
   assert(vault_kek_cache_has("uid:1000", t0) == 0);
   vault_kek_cache_clear();
   printf("  PASS: test_ttl_expiry_on_access\n");
}

static void test_refresh_updates_key_and_ttl(void)
{
   vault_kek_cache_clear();
   uint8_t k1[VAULT_KEK_LEN], k2[VAULT_KEK_LEN];
   fill_kek(k1, 1);
   fill_kek(k2, 200);
   assert(vault_kek_cache_put("uid:1000", k1, 0) == 0);
   /* Re-put refreshes the key and resets the TTL window. */
   assert(vault_kek_cache_put("uid:1000", k2, 500) == 0);
   uint8_t out[VAULT_KEK_LEN];
   assert(vault_kek_cache_get("uid:1000", 500 + VAULT_KEK_CACHE_TTL_SECONDS - 1, out) == 0);
   assert(memcmp(out, k2, VAULT_KEK_LEN) == 0); /* new key */
   assert(vault_kek_cache_count(500) == 1);     /* still one principal */
   vault_kek_cache_clear();
   printf("  PASS: test_refresh_updates_key_and_ttl\n");
}

static void test_reject_dont_evict_at_capacity(void)
{
   vault_kek_cache_clear();
   uint8_t kek[VAULT_KEK_LEN];
   fill_kek(kek, 3);
   char name[32];
   /* Fill every slot with a LIVE principal. */
   for (int i = 0; i < VAULT_KEK_CACHE_SLOTS; i++)
   {
      snprintf(name, sizeof(name), "uid:%d", 1000 + i);
      assert(vault_kek_cache_put(name, kek, 0) == 0);
   }
   assert(vault_kek_cache_count(0) == VAULT_KEK_CACHE_SLOTS);

   /* One more must be REJECTED — never evict an in-use KEK. */
   assert(vault_kek_cache_put("uid:9999", kek, 0) == -1);
   /* And the first principal is still present (not evicted). */
   uint8_t out[VAULT_KEK_LEN];
   assert(vault_kek_cache_get("uid:1000", 1, out) == 0);

   /* But a full-of-EXPIRED cache accepts a new principal (stale entries reaped). */
   long later = VAULT_KEK_CACHE_TTL_SECONDS + 1;
   assert(vault_kek_cache_put("uid:9999", kek, later) == 0);
   assert(vault_kek_cache_get("uid:9999", later + 1, out) == 0);
   vault_kek_cache_clear();
   printf("  PASS: test_reject_dont_evict_at_capacity\n");
}

static void test_evict_and_clear(void)
{
   vault_kek_cache_clear();
   uint8_t kek[VAULT_KEK_LEN];
   fill_kek(kek, 9);
   assert(vault_kek_cache_put("uid:1000", kek, 0) == 0);
   assert(vault_kek_cache_put("uid:1001", kek, 0) == 0);

   vault_kek_cache_evict("uid:1000");
   uint8_t out[VAULT_KEK_LEN];
   assert(vault_kek_cache_get("uid:1000", 1, out) == -1); /* gone */
   assert(vault_kek_cache_get("uid:1001", 1, out) == 0);  /* sibling intact */

   vault_kek_cache_clear();
   assert(vault_kek_cache_count(1) == 0);
   printf("  PASS: test_evict_and_clear\n");
}

int main(void)
{
   test_binary_exact_roundtrip();
   test_ttl_expiry_on_access();
   test_refresh_updates_key_and_ttl();
   test_reject_dont_evict_at_capacity();
   test_evict_and_clear();
   printf("vault_kek_cache: all tests passed\n");
   return 0;
}
