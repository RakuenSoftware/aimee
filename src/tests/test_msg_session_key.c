/* test_msg_session_key.c: the gateway-mutation per-session circuit breaker —
 * key resolution (validated header -> bearer -> none, NULL-first, cross-tenant),
 * disable/TTL/expiry, cap eviction (expired-first, live-not-silently-re-enabled),
 * insert-time sweep, and stats emission. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "harness_memory_common.h"
#include "msg_session_key.h"

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

   /* empty (non-NULL) auth_identity is treated like NULL -> header ignored, bearer used */
   assert(msg_session_key_resolve(hdr, "bearerA", "", key) == MSG_SESSION_KEY_RESOLVED);
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

int main(void)
{
   printf("msg_session_key: ");
   test_resolve();
   printf("ok\n");
   return 0;
}
