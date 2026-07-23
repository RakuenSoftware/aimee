#include "modules/vault/vault_witness_record.h"

#include <assert.h>
#include <openssl/sha.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* A well-formed audit-ledger record at shard_seq 2 (not first), with a real
 * source predecessor. Fields below are filled by the helper. */
static vault_witness_record_t fixture(void)
{
   vault_witness_record_t r;
   memset(&r, 0, sizeof r);
   r.source = VAULT_WITNESS_SRC_AUDIT;
   r.seal_epoch = 7;
   r.fencing_token = 9;
   r.shard_seq = 2;
   r.has_source_pred = 1;
   r.is_first_in_shard = 0;
   for (int i = 0; i < 32; i++)
   {
      r.source_hash[i] = (uint8_t)(0x10 + i);
      r.source_pred_hash[i] = (uint8_t)(0x30 + i);
      r.witness_pred_hash[i] = (uint8_t)(0x50 + i);
   }
   snprintf(r.source_id, sizeof r.source_id, "42");
   snprintf(r.tenant, sizeof r.tenant, "acme");
   snprintf(r.provider, sizeof r.provider, "anthropic");
   snprintf(r.request_id, sizeof r.request_id, "req-abc");
   snprintf(r.principal, sizeof r.principal, "team-7");
   snprintf(r.provider_cred, sizeof r.provider_cred, "anthropic:default");
   snprintf(r.group_id, sizeof r.group_id, "acme|origin|req-abc");
   snprintf(r.timestamp, sizeof r.timestamp, "2026-07-23T12:00:00Z");
   return r;
}

/* A valid first-in-shard record: shard_seq 1, witness predecessor == genesis. */
static vault_witness_record_t first_fixture(void)
{
   vault_witness_record_t r = fixture();
   r.shard_seq = 1;
   r.is_first_in_shard = 1;
   assert(vault_witness_genesis_sentinel(r.tenant, r.provider, r.witness_pred_hash) == 0);
   return r;
}

static void test_roundtrip(void)
{
   vault_witness_record_t r = fixture();
   uint8_t wire[VAULT_WITNESS_RECORD_MAX];
   size_t len = 0;
   assert(vault_witness_record_encode(&r, wire, sizeof wire, &len) == 0);
   assert(len > 140 && len <= VAULT_WITNESS_RECORD_MAX);

   vault_witness_record_t back;
   assert(vault_witness_record_decode(wire, len, &back) == 0);
   assert(vault_witness_record_equal(&r, &back));

   uint8_t d1[32], d2[32];
   assert(vault_witness_record_digest(&r, d1) == 0);
   assert(vault_witness_record_digest(&back, d2) == 0);
   assert(memcmp(d1, d2, 32) == 0);
}

static void test_first_in_shard(void)
{
   vault_witness_record_t r = first_fixture();
   uint8_t wire[VAULT_WITNESS_RECORD_MAX];
   size_t len = 0;
   assert(vault_witness_record_encode(&r, wire, sizeof wire, &len) == 0);
   vault_witness_record_t back;
   assert(vault_witness_record_decode(wire, len, &back) == 0);
   assert(back.is_first_in_shard == 1 && back.shard_seq == 1);

   /* Genesis sentinel on a non-first record is rejected. */
   vault_witness_record_t bad = first_fixture();
   bad.shard_seq = 5;
   bad.is_first_in_shard = 0; /* keeps witness_pred == genesis */
   uint8_t buf[32];
   assert(vault_witness_record_digest(&bad, buf) == -1);

   /* A non-genesis witness predecessor on a first record is rejected. */
   vault_witness_record_t bad2 = first_fixture();
   memset(bad2.witness_pred_hash, 0xAB, 32);
   assert(vault_witness_record_digest(&bad2, buf) == -1);
}

static void test_source_pred_rules(void)
{
   uint8_t d[32];
   /* Rewrap/open ledgers have no source predecessor. */
   vault_witness_record_t r = fixture();
   r.source = VAULT_WITNESS_SRC_REWRAP;
   assert(vault_witness_record_digest(&r, d) == -1); /* has_source_pred set on non-audit */
   r.has_source_pred = 0;
   memset(r.source_pred_hash, 0, 32);
   assert(vault_witness_record_digest(&r, d) == 0);

   /* Flag clear but a non-zero source predecessor is rejected. */
   vault_witness_record_t r2 = fixture();
   r2.has_source_pred = 0;
   assert(vault_witness_record_digest(&r2, d) == -1);
}

static void test_field_boundary_forgery(void)
{
   /* Plan vector: A(source_id="42", shard_key acme:anthropic) vs
    * B(source_id="42acme", shard_key ":anthropic"). Naive concatenation collides;
    * length-prefixed packing must not. */
   vault_witness_record_t a = fixture();
   snprintf(a.source_id, sizeof a.source_id, "42");
   snprintf(a.tenant, sizeof a.tenant, "acme");
   snprintf(a.provider, sizeof a.provider, "anthropic");

   vault_witness_record_t b = fixture();
   snprintf(b.source_id, sizeof b.source_id, "42acme");
   snprintf(b.tenant, sizeof b.tenant, ":");
   snprintf(b.provider, sizeof b.provider, "anthropic");
   /* Both are shard_seq 2 (not first), so witness_pred need not be genesis. */

   uint8_t da[32], db[32];
   assert(vault_witness_record_digest(&a, da) == 0);
   assert(vault_witness_record_digest(&b, db) == 0);
   assert(memcmp(da, db, 32) != 0);
}

static void test_decoder_rejections(void)
{
   vault_witness_record_t r = fixture();
   uint8_t wire[VAULT_WITNESS_RECORD_MAX];
   size_t len = 0;
   assert(vault_witness_record_encode(&r, wire, sizeof wire, &len) == 0);
   vault_witness_record_t out;

   /* Short buffer. */
   assert(vault_witness_record_decode(wire, 139, &out) == -1);
   /* Trailing byte: declared length no longer matches. */
   assert(vault_witness_record_decode(wire, len + 1, &out) == -1);
   /* Truncated body. */
   assert(vault_witness_record_decode(wire, len - 1, &out) == -1);

   /* Corrupt magic. */
   uint8_t bad[VAULT_WITNESS_RECORD_MAX];
   memcpy(bad, wire, len);
   bad[0] ^= 0xFF;
   assert(vault_witness_record_decode(bad, len, &out) == -1);

   /* Wrong version. */
   memcpy(bad, wire, len);
   bad[9] = 2;
   assert(vault_witness_record_decode(bad, len, &out) == -1);

   /* Unknown discriminator. */
   memcpy(bad, wire, len);
   bad[16] = 9;
   assert(vault_witness_record_decode(bad, len, &out) == -1);

   /* Unknown flag bit. */
   memcpy(bad, wire, len);
   bad[17] = 0x80;
   assert(vault_witness_record_decode(bad, len, &out) == -1);

   /* Nonzero reserved. */
   memcpy(bad, wire, len);
   bad[18] = 1;
   assert(vault_witness_record_decode(bad, len, &out) == -1);
}

static void test_empty_and_overlong_shardkey(void)
{
   uint8_t d[32];
   vault_witness_record_t r = fixture();
   r.tenant[0] = '\0'; /* empty shard-key component */
   assert(vault_witness_record_digest(&r, d) == -1);

   vault_witness_record_t r2 = fixture();
   memset(r2.provider, 'x', VAULT_WITNESS_PROVIDER_MAX + 1); /* fills buffer, no NUL room */
   r2.provider[VAULT_WITNESS_PROVIDER_MAX] = 'x';            /* strnlen > cap */
   /* provider array is [MAX+1]; force over-cap by making it exactly MAX+1 nonzero */
   uint8_t d2[32];
   /* strnlen(provider, MAX+1) == MAX+1 > MAX -> reject */
   assert(vault_witness_record_digest(&r2, d2) == -1);
}

static void test_max_shardkey_roundtrips(void)
{
   vault_witness_record_t r = fixture();
   memset(r.tenant, 'a', VAULT_WITNESS_TENANT_MAX);
   r.tenant[VAULT_WITNESS_TENANT_MAX] = '\0';
   memset(r.provider, 'b', VAULT_WITNESS_PROVIDER_MAX);
   r.provider[VAULT_WITNESS_PROVIDER_MAX] = '\0';
   uint8_t wire[VAULT_WITNESS_RECORD_MAX];
   size_t len = 0;
   assert(vault_witness_record_encode(&r, wire, sizeof wire, &len) == 0);
   vault_witness_record_t back;
   assert(vault_witness_record_decode(wire, len, &back) == 0);
   assert(vault_witness_record_equal(&r, &back));
}

static void test_shard_key_hash_and_genesis_distinct(void)
{
   uint8_t k1[8], k2[8];
   assert(vault_witness_shard_key_hash("acme", "anthropic", k1) == 0);
   assert(vault_witness_shard_key_hash("acm", "eanthropic", k2) == 0); /* boundary shift */
   assert(memcmp(k1, k2, 8) != 0);

   uint8_t g1[32], g2[32];
   assert(vault_witness_genesis_sentinel("acme", "anthropic", g1) == 0);
   assert(vault_witness_genesis_sentinel("acme", "openai", g2) == 0);
   assert(memcmp(g1, g2, 32) != 0);
}

/* Deterministic decode fuzz: every truncation and every single-byte corruption of
 * a valid record must return safely (0 or -1), never read out of bounds. Meant to
 * be run under ASAN/UBSAN, where an OOB read aborts. */
static void test_decode_fuzz(void)
{
   vault_witness_record_t r = fixture();
   uint8_t wire[VAULT_WITNESS_RECORD_MAX];
   size_t len = 0;
   assert(vault_witness_record_encode(&r, wire, sizeof wire, &len) == 0);
   vault_witness_record_t out;

   for (size_t cut = 0; cut <= len; cut++)
      (void)vault_witness_record_decode(wire, cut, &out);

   for (size_t i = 0; i < len; i++)
   {
      uint8_t saved = wire[i];
      for (unsigned bit = 0; bit < 8; bit++)
      {
         wire[i] = (uint8_t)(saved ^ (1u << bit));
         (void)vault_witness_record_decode(wire, len, &out);
      }
      wire[i] = saved;
   }
}

int main(void)
{
   test_roundtrip();
   test_first_in_shard();
   test_source_pred_rules();
   test_field_boundary_forgery();
   test_decoder_rejections();
   test_empty_and_overlong_shardkey();
   test_max_shardkey_roundtrips();
   test_shard_key_hash_and_genesis_distinct();
   test_decode_fuzz();
   printf("test_vault_witness_record: all passed\n");
   return 0;
}
