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
   printf("test_audit_action: all passed\n");
   return 0;
}
