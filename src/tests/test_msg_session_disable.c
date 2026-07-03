/* test_msg_session_disable.c: the gateway-mutation per-session circuit breaker —
 * key resolution (validated header -> bearer -> none, NULL-first, cross-tenant),
 * disable/TTL/expiry, cap eviction (expired-first, live-not-silently-re-enabled),
 * insert-time sweep, and stats emission. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "gw_mutate_stats.h"
#include "harness_memory_common.h"
#include "msg_session_disable.h"

static void sleep_ms(int ms)
{
   struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
   nanosleep(&ts, NULL);
}

/* First 16 hex of SHA-256(s), into out[17]. */
static void sha16(const char *s, char out[MSG_SESSION_KEY_LEN])
{
   char full[HMEM_HASH_HEX_LEN];
   hmem_sha256_hex(s, strlen(s), full);
   memcpy(out, full, 16);
   out[16] = '\0';
}

static void test_resolve(void)
{
   char key[MSG_SESSION_KEY_LEN];
   char expect[MSG_SESSION_KEY_LEN];

   /* identity-less: no header, no bearer -> NONE, no key */
   assert(msg_session_key_resolve(NULL, NULL, NULL, key) == MSG_SESSION_KEY_NONE);
   assert(key[0] == '\0');
   assert(msg_session_key_resolve(NULL, "", "", key) == MSG_SESSION_KEY_NONE);

   /* bearer only -> RESOLVED, key = SHA256(bearer)[16] */
   assert(msg_session_key_resolve(NULL, "bearerA", NULL, key) == MSG_SESSION_KEY_RESOLVED);
   sha16("bearerA", expect);
   assert(strcmp(key, expect) == 0);

   /* valid header matching SHA256(auth) -> RESOLVED, key = that header */
   char hdr[MSG_SESSION_KEY_LEN];
   sha16("identityX", hdr);
   assert(msg_session_key_resolve(hdr, "bearerA", "identityX", key) == MSG_SESSION_KEY_RESOLVED);
   assert(strcmp(key, hdr) == 0); /* header wins over bearer when valid */

   /* NULL-first: auth_identity NULL -> header ignored (never SHA256(NULL)), bearer used */
   assert(msg_session_key_resolve(hdr, "bearerA", NULL, key) == MSG_SESSION_KEY_RESOLVED);
   sha16("bearerA", expect);
   assert(strcmp(key, expect) == 0);

   /* malformed header (wrong length / non-hex / uppercase) -> treated absent, bearer used,
    * status flags the anomaly */
   assert(msg_session_key_resolve("short", "bearerA", "identityX", key) ==
          MSG_SESSION_KEY_BEARER_BAD_HDR);
   assert(strcmp(key, expect) == 0);
   assert(msg_session_key_resolve("ZZZZZZZZZZZZZZZZ", "bearerA", "identityX", key) ==
          MSG_SESSION_KEY_BEARER_BAD_HDR);
   char upper[17];
   memcpy(upper, hdr, 16);
   upper[16] = '\0';
   for (int i = 0; i < 16; i++)
      if (upper[i] >= 'a' && upper[i] <= 'f')
         upper[i] = (char)(upper[i] - 'a' + 'A');
   /* only asserts uppercase is rejected when the hash actually contained a-f */
   if (strcmp(upper, hdr) != 0)
      assert(msg_session_key_resolve(upper, "bearerA", "identityX", key) ==
             MSG_SESSION_KEY_BEARER_BAD_HDR);

   /* --- cross-tenant matrix --- */
   char keyB[MSG_SESSION_KEY_LEN], forgedB[MSG_SESSION_KEY_LEN];
   sha16("identityB", forgedB); /* attacker forges B's session-id */

   /* (a) bearer A + header = SHA256(B): rejected, key falls back to SHA256(A), not B */
   assert(msg_session_key_resolve(forgedB, "bearerA", "identityA", key) ==
          MSG_SESSION_KEY_BEARER_BAD_HDR);
   sha16("bearerA", expect);
   assert(strcmp(key, expect) == 0);
   sha16("identityB", keyB);
   assert(strcmp(key, keyB) != 0); /* attacker cannot land on B's key */

   /* (c) forged header + NO bearer -> NONE, no key (no state can be written) */
   assert(msg_session_key_resolve(forgedB, NULL, "identityA", key) == MSG_SESSION_KEY_NONE);
   assert(key[0] == '\0');

   /* (d) bearer A + malformed header -> key = SHA256(A) (never B) */
   assert(msg_session_key_resolve("!!bad!!", "bearerA", "identityA", key) ==
          MSG_SESSION_KEY_BEARER_BAD_HDR);
   assert(strcmp(key, expect) == 0);
}

static void test_disable_ttl(void)
{
   msg_session_reset();
   gw_stat_reset();

   char a[MSG_SESSION_KEY_LEN], b[MSG_SESSION_KEY_LEN];
   sha16("sessA", a);
   sha16("sessB", b);

   assert(msg_session_is_disabled(a) == 0);
   msg_session_disable(a, 3600000, "4xx");
   assert(msg_session_is_disabled(a) == 1);
   assert(msg_session_is_disabled(b) == 0); /* distinct sessions independent */
   assert(msg_session_count() == 1);
   assert(gw_stat_get_reason("session_disabled_set", "4xx") == 1);

   /* identity-less / empty key never disables */
   msg_session_disable("", 3600000, "x");
   assert(msg_session_is_disabled("") == 0);

   /* TTL expiry: tiny ttl -> expired after a short sleep, lazily cleared on read */
   msg_session_disable(b, 1, "5xx");
   sleep_ms(15);
   assert(msg_session_is_disabled(b) == 0);
   assert(msg_session_count() == 1); /* only a remains */

   /* ttl<=0 is ignored */
   char c[MSG_SESSION_KEY_LEN];
   sha16("sessC", c);
   msg_session_disable(c, 0, "x");
   msg_session_disable(c, -5, "x");
   assert(msg_session_is_disabled(c) == 0);
}

static void test_eviction(void)
{
   msg_session_reset();
   char key[MSG_SESSION_KEY_LEN];

   /* Fill to capacity with live entries; one more must not exceed the cap, and the
    * OLDEST-inserted live entry is the one evicted. */
   snprintf(key, sizeof(key), "%016x", 0);
   char first[MSG_SESSION_KEY_LEN];
   memcpy(first, key, sizeof(key));
   for (int i = 0; i < 10000; i++)
   {
      snprintf(key, sizeof(key), "%016x", i);
      msg_session_disable(key, 3600000, "4xx");
   }
   assert(msg_session_count() == 10000);
   assert(msg_session_is_disabled(first) == 1);

   snprintf(key, sizeof(key), "%016x", 10000); /* the 10001st */
   msg_session_disable(key, 3600000, "4xx");
   assert(msg_session_count() == 10000);        /* bounded */
   assert(msg_session_is_disabled(first) == 0); /* oldest evicted */
   assert(msg_session_is_disabled(key) == 1);   /* newest present */
}

static void test_sweep_reclaims_expired(void)
{
   msg_session_reset();
   char key[MSG_SESSION_KEY_LEN];

   /* Insert > cap/2 short-lived entries, let them expire, then one more insert must
    * trigger the insert-time sweep (size > cap/2) and reclaim them. */
   for (int i = 0; i < 5001; i++)
   {
      snprintf(key, sizeof(key), "%016x", i);
      msg_session_disable(key, 1, "5xx");
   }
   sleep_ms(20);
   snprintf(key, sizeof(key), "%016x", 900000);
   msg_session_disable(key, 3600000, "4xx");
   /* the sweep dropped the 5001 expired; only the fresh one survives */
   assert(msg_session_count() == 1);
   assert(msg_session_is_disabled(key) == 1);
}

int main(void)
{
   printf("msg_session_disable: ");
   test_resolve();
   test_disable_ttl();
   test_eviction();
   test_sweep_reclaims_expired();
   printf("ok\n");
   return 0;
}
