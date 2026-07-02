/* test_audit_action.c: unit tests for the S1 args_hash primitive.
 *
 * Exercises the contract in audit_action.h: determinism, key-order and
 * whitespace independence, per-tool allowlist projection (non-allowlisted fields
 * dropped), unknown-tool name-only hashing, key-sensitivity, oversize bounding,
 * and the best-effort failure sentinel. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audit_action.h"

static char g_home[512];

static void set_home_fresh(void)
{
   /* A per-test temp AIMEE_HOME so .audit-key provisioning is isolated. */
   snprintf(g_home, sizeof g_home, "/tmp/aimee-audit-test-%d", (int)getpid());
   mkdir(g_home, 0700);
   setenv("AIMEE_HOME", g_home, 1);
}

static void rm_key(void)
{
   char p[600];
   snprintf(p, sizeof p, "%s/.audit-key", g_home);
   unlink(p);
}

static int is_sentinel(const char *h)
{
   if (strncmp(h, "v1-", 3) != 0)
      return 0;
   for (int i = 3; i < 67; i++)
      if (h[i] != '0')
         return 0;
   return h[67] == '\0';
}

static void hash_of(const char *tool, const char *args, char out[AUDIT_ARGS_HASH_LEN])
{
   int rc = audit_args_hash(tool, args, out, AUDIT_ARGS_HASH_LEN);
   assert(rc == 0);
   assert(strncmp(out, "v1-", 3) == 0);
   assert(strlen(out) == 67);
   assert(!is_sentinel(out)); /* a keyed hash is not the all-zero sentinel */
}

static void test_shape_and_determinism(void)
{
   set_home_fresh();
   rm_key();
   assert(audit_ensure_key() == 0);

   char a[AUDIT_ARGS_HASH_LEN], b[AUDIT_ARGS_HASH_LEN];
   hash_of("Write", "{\"file_path\":\"/x\",\"content\":\"hello\"}", a);
   hash_of("Write", "{\"file_path\":\"/x\",\"content\":\"hello\"}", b);
   assert(strcmp(a, b) == 0); /* deterministic */
}

static void test_key_order_and_whitespace_independent(void)
{
   char a[AUDIT_ARGS_HASH_LEN], b[AUDIT_ARGS_HASH_LEN];
   hash_of("Edit", "{\"file_path\":\"/f\",\"old_string\":\"x\",\"new_string\":\"y\"}", a);
   /* reordered keys + extra whitespace -> same projection -> same hash */
   hash_of("Edit", "{ \"new_string\":\"y\" ,  \"file_path\":\"/f\", \"old_string\":\"x\" }", b);
   assert(strcmp(a, b) == 0);
}

static void test_allowlist_drops_extra_fields(void)
{
   char base[AUDIT_ARGS_HASH_LEN], with_secret[AUDIT_ARGS_HASH_LEN];
   hash_of("Write", "{\"file_path\":\"/x\",\"content\":\"c\"}", base);
   /* a non-allowlisted field (a token/PII) must NOT change the hash */
   hash_of("Write", "{\"file_path\":\"/x\",\"content\":\"c\",\"authorization\":\"Bearer sk-secret\"}",
           with_secret);
   assert(strcmp(base, with_secret) == 0);
}

static void test_distinct_actions_differ(void)
{
   char x[AUDIT_ARGS_HASH_LEN], y[AUDIT_ARGS_HASH_LEN];
   hash_of("Write", "{\"file_path\":\"/a\",\"content\":\"c\"}", x);
   hash_of("Write", "{\"file_path\":\"/b\",\"content\":\"c\"}", y);
   assert(strcmp(x, y) != 0); /* different target -> different digest */
}

static void test_unknown_tool_hashes_name_only(void)
{
   char x[AUDIT_ARGS_HASH_LEN], y[AUDIT_ARGS_HASH_LEN], z[AUDIT_ARGS_HASH_LEN];
   /* unknown tool: args are ignored entirely (name-only) */
   hash_of("MysteryTool", "{\"anything\":\"1\"}", x);
   hash_of("MysteryTool", "{\"totally\":\"different\"}", y);
   assert(strcmp(x, y) == 0);
   /* but a different tool name still differs */
   hash_of("OtherTool", "{\"anything\":\"1\"}", z);
   assert(strcmp(x, z) != 0);
}

static void test_oversize_is_bounded_and_stable(void)
{
   size_t n = 300 * 1024; /* beyond AUDIT_ARGS_MAX_INPUT */
   char *big = malloc(n + 64);
   assert(big);
   int off = snprintf(big, 40, "{\"file_path\":\"/x\",\"content\":\"");
   memset(big + off, 'A', n);
   memcpy(big + off + n, "\"}", 3);
   char a[AUDIT_ARGS_HASH_LEN], b[AUDIT_ARGS_HASH_LEN];
   hash_of("Write", big, a);
   hash_of("Write", big, b);
   assert(strcmp(a, b) == 0); /* oversize path is deterministic, no crash */
   free(big);
}

static void test_key_sensitivity(void)
{
   char k1[AUDIT_ARGS_HASH_LEN], k2[AUDIT_ARGS_HASH_LEN];
   hash_of("Bash", "{\"command\":\"ls\"}", k1);
   /* rotate the key: same input must produce a different digest */
   rm_key();
   assert(audit_ensure_key() == 0);
   hash_of("Bash", "{\"command\":\"ls\"}", k2);
   assert(strcmp(k1, k2) != 0);
}

static void test_missing_key_returns_sentinel(void)
{
   rm_key(); /* no key present, do NOT ensure */
   char out[AUDIT_ARGS_HASH_LEN];
   int rc = audit_args_hash("Bash", "{\"command\":\"ls\"}", out, sizeof out);
   assert(rc == -1);
   assert(is_sentinel(out)); /* never HMAC-over-empty; stable sentinel */
}

static void mac_hex(const unsigned char mac[32], char out[65])
{
   static const char hx[] = "0123456789abcdef";
   for (int i = 0; i < 32; i++)
   {
      out[i * 2] = hx[(mac[i] >> 4) & 0xf];
      out[i * 2 + 1] = hx[mac[i] & 0xf];
   }
   out[64] = '\0';
}

/* RFC 4231 known-answer vectors: independently validate the HMAC-SHA256
 * construction (ipad/opad, 64-byte block, key>64 pre-hash), not just internal
 * determinism. */
static void test_hmac_rfc4231_vectors(void)
{
   unsigned char mac[32];
   char hex[65];

   /* Test Case 2: short key. */
   assert(audit_hmac_sha256_testonly((const unsigned char *)"Jefe", 4,
                                     (const unsigned char *)"what do ya want for nothing?", 28,
                                     mac) == 0);
   mac_hex(mac, hex);
   assert(strcmp(hex, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") == 0);

   /* Test Case 6: key longer than the 64-byte block -> exercises the pre-hash path. */
   unsigned char longkey[131];
   memset(longkey, 0xaa, sizeof longkey);
   const char *data6 = "Test Using Larger Than Block-Size Key - Hash Key First";
   assert(audit_hmac_sha256_testonly(longkey, sizeof longkey, (const unsigned char *)data6,
                                     strlen(data6), mac) == 0);
   mac_hex(mac, hex);
   assert(strcmp(hex, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54") == 0);
}

/* Separator bytes inside a value must not forge a component boundary: an
 * attacker stuffing the length-prefix separator into old_string cannot collide
 * with a differently-structured argument set. */
static void test_separator_injection_no_collision(void)
{
   set_home_fresh();
   assert(audit_ensure_key() == 0);
   char a[AUDIT_ARGS_HASH_LEN], b[AUDIT_ARGS_HASH_LEN], b2[AUDIT_ARGS_HASH_LEN];
   hash_of("Edit", "{\"file_path\":\"/f\",\"old_string\":\"b\",\"new_string\":\"c\"}", a);
   /* old_string carries a literal 0x1f () + a fake "K10:new_string" prefix */
   hash_of("Edit",
           "{\"file_path\":\"/f\",\"old_string\":\"b\\u001fK10:new_string\",\"new_string\":\"c\"}",
           b);
   assert(strcmp(a, b) != 0); /* no collision: value bytes != structure */
   /* and the injected-separator variant is itself deterministic */
   hash_of("Edit",
           "{\"file_path\":\"/f\",\"old_string\":\"b\\u001fK10:new_string\",\"new_string\":\"c\"}",
           b2);
   assert(strcmp(b, b2) == 0);
}

static void test_value_truncation_semantics(void)
{
   size_t cap = 8192; /* AUDIT_VALUE_MAX_BYTES */
   char *exact = malloc(64 + cap);
   char *trunc1 = malloc(64 + cap + 16);
   char *trunc2 = malloc(64 + cap + 16);
   assert(exact && trunc1 && trunc2);
   /* content = cap 'A's exactly (not truncated, tag 'F') */
   int off = snprintf(exact, 40, "{\"file_path\":\"/x\",\"content\":\"");
   memset(exact + off, 'A', cap);
   memcpy(exact + off + cap, "\"}", 3);
   /* content = cap 'A's + "EXTRA" (truncated to cap, tag 'T') */
   int o1 = snprintf(trunc1, 40, "{\"file_path\":\"/x\",\"content\":\"");
   memset(trunc1 + o1, 'A', cap);
   memcpy(trunc1 + o1 + cap, "EXTRA\"}", 8);
   /* content = cap 'A's + "OTHER!" (also truncated to same cap 'A's) */
   int o2 = snprintf(trunc2, 40, "{\"file_path\":\"/x\",\"content\":\"");
   memset(trunc2 + o2, 'A', cap);
   memcpy(trunc2 + o2 + cap, "OTHER!\"}", 9);

   char he[AUDIT_ARGS_HASH_LEN], ht1[AUDIT_ARGS_HASH_LEN], ht2[AUDIT_ARGS_HASH_LEN];
   hash_of("Write", exact, he);
   hash_of("Write", trunc1, ht1);
   hash_of("Write", trunc2, ht2);
   assert(strcmp(he, ht1) != 0);  /* exact (F) != truncated (T) at the same prefix */
   assert(strcmp(ht1, ht2) == 0); /* bytes beyond the cap don't affect the digest */
   free(exact);
   free(trunc1);
   free(trunc2);
}

int main(void)
{
   test_shape_and_determinism();
   test_key_order_and_whitespace_independent();
   test_allowlist_drops_extra_fields();
   test_distinct_actions_differ();
   test_unknown_tool_hashes_name_only();
   test_oversize_is_bounded_and_stable();
   test_key_sensitivity();
   test_missing_key_returns_sentinel();
   test_hmac_rfc4231_vectors();
   test_separator_injection_no_collision();
   test_value_truncation_semantics();
   printf("test_audit_action: all passed\n");
   return 0;
}
