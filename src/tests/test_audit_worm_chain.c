/* test_audit_worm_chain.c — golden-vector + drift test for the engine-agnostic
 * WORM chain primitives (modules/audit/audit_worm_chain.c).
 *
 * These primitives are shared VERBATIM by the aimee-server SQLite store
 * (modules/audit/audit_worm.c) and the aimee-kb Postgres store
 * (db2/kb_audit_worm.c), so both must produce byte-identical row hashes and
 * checkpoint MACs. This test is the single source of truth for that cross-engine
 * contract: it pins the canonical hash of a known row (the same literal asserted
 * from each store's side in test_audit_worm.c / test_kb_audit_worm.c) and locks
 * the canonicalization structure both engines depend on, so a change to either
 * store's hashing is caught here rather than silently splitting the two chains.
 *
 * Unlike the two store tests, this exercises only the pure primitives — no SQLite
 * or Postgres — so it is a fast, dependency-light unit test.
 *
 * The pinned hash/MAC literals are the canonical values on the standard build
 * host (little-endian x86_64), matching the identical vector pinned in the two
 * store tests. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "audit_worm_chain.h"

/* hex32 uses hand-verifiable vectors (no SHA needed): sequential bytes map to
 * their own two-hex-digit values, and all-zero bytes are the genesis prev. */
static void test_hex32(void)
{
   unsigned char in[32];
   char out[65];

   for (int i = 0; i < 32; i++)
      in[i] = (unsigned char)i;
   audit_worm_hex32(in, out);
   assert(strcmp(out, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f") == 0);

   memset(in, 0, sizeof in);
   audit_worm_hex32(in, out);
   assert(strcmp(out, AUDIT_WORM_GENESIS_PREV) == 0);

   printf("  test_hex32: ok\n");
}

/* THE cross-engine contract vector. This literal is asserted independently from
 * each store's side (test_audit_worm.c::test_cross_engine_vector and
 * test_kb_audit_worm.c::test_cross_engine_vector); all three must stay identical
 * or the two engines' chains diverge. */
static void test_genesis_row_hash(void)
{
   char h[65];
   audit_worm_row_hash(1, "primary", "u", "tool.read", "v1-1", "allow", "", "{}",
                       AUDIT_WORM_GENESIS_PREV, h);
   assert(strcmp(h, "3c2adf68ae8f1b704780ffedd32522e06468e34a69205240fbb358a7122ff986") == 0);
   printf("  test_genesis_row_hash: ok\n");
}

/* row_hash must be deterministic and depend on every field (an injective,
 * length-prefixed encoding), and must chain via prev_hash. These properties are
 * what guarantee two engines that feed the same logical row get the same hash. */
static void test_row_hash_field_sensitivity(void)
{
   char base[65], v[65];
   audit_worm_row_hash(1, "primary", "u", "tool.read", "v1-1", "allow", "", "{}",
                       AUDIT_WORM_GENESIS_PREV, base);

   /* deterministic */
   audit_worm_row_hash(1, "primary", "u", "tool.read", "v1-1", "allow", "", "{}",
                       AUDIT_WORM_GENESIS_PREV, v);
   assert(strcmp(base, v) == 0);

/* Each single-field change must move the hash. */
#define DIFF(...)                                                                                  \
   do                                                                                              \
   {                                                                                               \
      audit_worm_row_hash(__VA_ARGS__, v);                                                         \
      assert(strcmp(base, v) != 0);                                                                \
   } while (0)
   DIFF(2, "primary", "u", "tool.read", "v1-1", "allow", "", "{}",
        AUDIT_WORM_GENESIS_PREV); /* seq */
   DIFF(1, "backup", "u", "tool.read", "v1-1", "allow", "", "{}",
        AUDIT_WORM_GENESIS_PREV); /* role */
   DIFF(1, "primary", "x", "tool.read", "v1-1", "allow", "", "{}",
        AUDIT_WORM_GENESIS_PREV); /* principal */
   DIFF(1, "primary", "u", "tool.write", "v1-1", "allow", "", "{}",
        AUDIT_WORM_GENESIS_PREV); /* action */
   DIFF(1, "primary", "u", "tool.read", "v2-1", "allow", "", "{}",
        AUDIT_WORM_GENESIS_PREV); /* subject */
   DIFF(1, "primary", "u", "tool.read", "v1-1", "block", "", "{}",
        AUDIT_WORM_GENESIS_PREV); /* verdict */
   DIFF(1, "primary", "u", "tool.read", "v1-1", "allow", "k1", "{}",
        AUDIT_WORM_GENESIS_PREV); /* key_id */
   DIFF(1, "primary", "u", "tool.read", "v1-1", "allow", "", "{\"a\":1}",
        AUDIT_WORM_GENESIS_PREV); /* detail */
   {
      char altprev[65];
      memcpy(altprev, AUDIT_WORM_GENESIS_PREV, sizeof altprev);
      altprev[0] = '1'; /* different prev_hash -> different row_hash (the chain link) */
      DIFF(1, "primary", "u", "tool.read", "v1-1", "allow", "", "{}", altprev);
   }
#undef DIFF

   /* Length-prefixing must make the field encoding injective: moving a character
    * across a field boundary must not collide. */
   char x[65], y[65];
   audit_worm_row_hash(1, "ab", "", "act", "", "", "", "", AUDIT_WORM_GENESIS_PREV, x);
   audit_worm_row_hash(1, "a", "b", "act", "", "", "", "", AUDIT_WORM_GENESIS_PREV, y);
   assert(strcmp(x, y) != 0);

   printf("  test_row_hash_field_sensitivity: ok\n");
}

/* Checkpoint MAC must be deterministic and bound to the key, the attested head
 * (hash + seq), and the key_id. */
static void test_ckpt_mac(void)
{
   const char *head = "3c2adf68ae8f1b704780ffedd32522e06468e34a69205240fbb358a7122ff986";
   unsigned char k1[32], k2[32];
   char m[65], v[65];
   memset(k1, 0x01, sizeof k1);
   memset(k2, 0x02, sizeof k2);

   audit_worm_ckpt_mac(k1, head, 1, "kid0", m);

   audit_worm_ckpt_mac(k1, head, 1, "kid0", v);
   assert(strcmp(m, v) == 0); /* deterministic */
   audit_worm_ckpt_mac(k2, head, 1, "kid0", v);
   assert(strcmp(m, v) != 0); /* key-bound */
   audit_worm_ckpt_mac(k1, head, 2, "kid0", v);
   assert(strcmp(m, v) != 0); /* head-seq-bound */
   audit_worm_ckpt_mac(k1, "0000000000000000000000000000000000000000000000000000000000000000", 1,
                       "kid0", v);
   assert(strcmp(m, v) != 0); /* head-hash-bound */
   audit_worm_ckpt_mac(k1, head, 1, "kid1", v);
   assert(strcmp(m, v) != 0); /* key_id-bound */

   printf("  test_ckpt_mac: ok\n");
}

int main(void)
{
   test_hex32();
   test_genesis_row_hash();
   test_row_hash_field_sensitivity();
   test_ckpt_mac();
   printf("all tests passed\n");
   return 0;
}
