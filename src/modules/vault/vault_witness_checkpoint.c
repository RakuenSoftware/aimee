#include "vault_witness_checkpoint.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <string.h>

static void put_u16(uint8_t *p, uint16_t v)
{
   p[0] = (uint8_t)(v >> 8);
   p[1] = (uint8_t)v;
}

static void put_u32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)(v >> 24);
   p[1] = (uint8_t)(v >> 16);
   p[2] = (uint8_t)(v >> 8);
   p[3] = (uint8_t)v;
}

static void put_u64(uint8_t *p, uint64_t v)
{
   for (unsigned i = 0; i < 8; i++)
      p[i] = (uint8_t)(v >> (56U - 8U * i));
}

static int all_zero(const uint8_t *p, size_t n)
{
   uint8_t any = 0;
   for (size_t i = 0; i < n; i++)
      any |= p[i];
   return any == 0;
}

/* Structural validity independent of signature. */
static int checkpoint_valid(const vault_witness_checkpoint_t *cp)
{
   if (!cp || cp->version != 1 || cp->sig_alg != VAULT_WITNESS_SIG_ED25519)
      return 0;
   if (!cp->has_predecessor && !all_zero(cp->predecessor_digest, 32))
      return 0;
   size_t ca = strnlen(cp->created_at, VAULT_WITNESS_CP_CREATED_AT_MAX + 1);
   if (ca == 0 || ca > VAULT_WITNESS_CP_CREATED_AT_MAX)
      return 0;
   return 1;
}

int vault_witness_checkpoint_signable(const vault_witness_checkpoint_t *cp, uint8_t *out,
                                      size_t cap, size_t *out_len)
{
   if (!out || !out_len)
      return -1;
   *out_len = 0;
   if (!checkpoint_valid(cp))
      return -1;

   size_t label_len = strlen(VAULT_WITNESS_CHECKPOINT_LABEL);
   size_t ca_len = strlen(cp->created_at);
   size_t need = 4 + label_len + 2 + 8 + 32 + 1 + 32 + 8 + 32 +
                 VAULT_WITNESS_SIGNER_KEY_ID_LEN + 2 + 2 + 4 + ca_len;
   if (cap < need)
      return -1;

   size_t off = 0;
   put_u32(out + off, (uint32_t)label_len);
   off += 4;
   memcpy(out + off, VAULT_WITNESS_CHECKPOINT_LABEL, label_len);
   off += label_len;
   put_u16(out + off, cp->version);
   off += 2;
   put_u64(out + off, cp->seq);
   off += 8;
   memcpy(out + off, cp->root, 32);
   off += 32;
   out[off++] = cp->has_predecessor ? 1 : 0;
   memcpy(out + off, cp->predecessor_digest, 32);
   off += 32;
   put_u64(out + off, cp->shard_count);
   off += 8;
   memcpy(out + off, cp->leaf_snapshot_digest, 32);
   off += 32;
   memcpy(out + off, cp->signer_key_id, VAULT_WITNESS_SIGNER_KEY_ID_LEN);
   off += VAULT_WITNESS_SIGNER_KEY_ID_LEN;
   put_u16(out + off, cp->sig_alg);
   off += 2;
   put_u16(out + off, cp->sig_version);
   off += 2;
   put_u32(out + off, (uint32_t)ca_len);
   off += 4;
   memcpy(out + off, cp->created_at, ca_len);
   off += ca_len;
   *out_len = off;
   return 0;
}

int vault_witness_checkpoint_digest(const vault_witness_checkpoint_t *cp, uint8_t digest[32])
{
   if (!digest)
      return -1;
   OPENSSL_cleanse(digest, 32);
   uint8_t body[512];
   size_t len = 0;
   if (vault_witness_checkpoint_signable(cp, body, sizeof body, &len) != 0)
      return -1;
   int ok = SHA256(body, len, digest) != NULL;
   OPENSSL_cleanse(body, sizeof body);
   if (!ok)
   {
      OPENSSL_cleanse(digest, 32);
      return -1;
   }
   return 0;
}

static uint16_t get_u16(const uint8_t *p)
{
   return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t get_u32(const uint8_t *p)
{
   return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t get_u64(const uint8_t *p)
{
   uint64_t v = 0;
   for (unsigned i = 0; i < 8; i++)
      v = (v << 8) | p[i];
   return v;
}

int vault_witness_checkpoint_encode(const vault_witness_checkpoint_t *cp, uint8_t *out, size_t cap,
                                    size_t *out_len)
{
   if (!out || !out_len)
      return -1;
   *out_len = 0;
   size_t body = 0;
   if (vault_witness_checkpoint_signable(cp, out, cap, &body) != 0)
      return -1;
   if (cap < body + VAULT_WITNESS_ED25519_SIG_LEN)
      return -1;
   memcpy(out + body, cp->signature, VAULT_WITNESS_ED25519_SIG_LEN);
   *out_len = body + VAULT_WITNESS_ED25519_SIG_LEN;
   return 0;
}

int vault_witness_checkpoint_decode(const uint8_t *wire, size_t wire_len,
                                    vault_witness_checkpoint_t *cp)
{
   if (!cp)
      return -1;
   OPENSSL_cleanse(cp, sizeof *cp);
   if (!wire)
      return -1;
   size_t label_len = strlen(VAULT_WITNESS_CHECKPOINT_LABEL);
   /* Fixed portion after the length-prefixed label: version(2) seq(8) root(32)
    * has_pred(1) pred(32) shard_count(8) leaf_digest(32) key_id(16) sig_alg(2)
    * sig_version(2) created_at_len(4) ... created_at(N) sig(64). */
   size_t off = 0;
   if (wire_len < 4)
      return -1;
   if (get_u32(wire) != label_len)
      return -1;
   off = 4;
   if (off + label_len > wire_len ||
       CRYPTO_memcmp(wire + off, VAULT_WITNESS_CHECKPOINT_LABEL, label_len) != 0)
      return -1;
   off += label_len;
   const size_t fixed = 2 + 8 + 32 + 1 + 32 + 8 + 32 + VAULT_WITNESS_SIGNER_KEY_ID_LEN + 2 + 2 + 4;
   if (off + fixed > wire_len)
      return -1;

   vault_witness_checkpoint_t tmp;
   memset(&tmp, 0, sizeof tmp);
   tmp.version = get_u16(wire + off);
   off += 2;
   tmp.seq = get_u64(wire + off);
   off += 8;
   memcpy(tmp.root, wire + off, 32);
   off += 32;
   tmp.has_predecessor = wire[off++] ? 1 : 0;
   memcpy(tmp.predecessor_digest, wire + off, 32);
   off += 32;
   tmp.shard_count = get_u64(wire + off);
   off += 8;
   memcpy(tmp.leaf_snapshot_digest, wire + off, 32);
   off += 32;
   memcpy(tmp.signer_key_id, wire + off, VAULT_WITNESS_SIGNER_KEY_ID_LEN);
   off += VAULT_WITNESS_SIGNER_KEY_ID_LEN;
   tmp.sig_alg = get_u16(wire + off);
   off += 2;
   tmp.sig_version = get_u16(wire + off);
   off += 2;
   uint32_t ca_len = get_u32(wire + off);
   off += 4;
   if (ca_len > VAULT_WITNESS_CP_CREATED_AT_MAX || off + ca_len + VAULT_WITNESS_ED25519_SIG_LEN != wire_len)
      return -1;
   if (memchr(wire + off, 0, ca_len) != NULL)
      return -1;
   memcpy(tmp.created_at, wire + off, ca_len);
   tmp.created_at[ca_len] = '\0';
   off += ca_len;
   memcpy(tmp.signature, wire + off, VAULT_WITNESS_ED25519_SIG_LEN);

   /* Re-encoding must reproduce the input, which also enforces checkpoint_valid
    * (the has_pred/zero-digest, version, sig_alg, created_at rules). */
   uint8_t re[VAULT_WITNESS_CHECKPOINT_WIRE_MAX];
   size_t re_len = 0;
   if (vault_witness_checkpoint_encode(&tmp, re, sizeof re, &re_len) != 0 || re_len != wire_len ||
       CRYPTO_memcmp(re, wire, wire_len) != 0)
   {
      OPENSSL_cleanse(&tmp, sizeof tmp);
      return -1;
   }
   *cp = tmp;
   OPENSSL_cleanse(&tmp, sizeof tmp);
   return 0;
}

int vault_witness_checkpoint_sign_ed25519(vault_witness_checkpoint_t *cp,
                                          const uint8_t priv[VAULT_WITNESS_ED25519_PRIV_LEN])
{
   if (!cp || !priv)
      return -1;
   /* Clear any stale signature first, so a failed sign never leaves prior bytes. */
   OPENSSL_cleanse(cp->signature, VAULT_WITNESS_ED25519_SIG_LEN);
   cp->sig_alg = VAULT_WITNESS_SIG_ED25519;
   uint8_t body[512];
   size_t len = 0;
   if (vault_witness_checkpoint_signable(cp, body, sizeof body, &len) != 0)
      return -1;

   int rc = -1;
   EVP_PKEY *pkey =
       EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, priv, VAULT_WITNESS_ED25519_PRIV_LEN);
   EVP_MD_CTX *ctx = EVP_MD_CTX_new();
   if (pkey && ctx && EVP_DigestSignInit(ctx, NULL, NULL, NULL, pkey) == 1)
   {
      size_t siglen = VAULT_WITNESS_ED25519_SIG_LEN;
      if (EVP_DigestSign(ctx, cp->signature, &siglen, body, len) == 1 &&
          siglen == VAULT_WITNESS_ED25519_SIG_LEN)
         rc = 0;
   }
   EVP_MD_CTX_free(ctx);
   EVP_PKEY_free(pkey);
   OPENSSL_cleanse(body, sizeof body);
   return rc;
}

static int ed25519_verify(const uint8_t pub[32], const uint8_t *msg, size_t msg_len,
                          const uint8_t sig[64])
{
   int ok = 0;
   EVP_PKEY *pkey =
       EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pub, VAULT_WITNESS_ED25519_PUB_LEN);
   EVP_MD_CTX *ctx = EVP_MD_CTX_new();
   if (pkey && ctx && EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, pkey) == 1)
      ok = EVP_DigestVerify(ctx, sig, VAULT_WITNESS_ED25519_SIG_LEN, msg, msg_len) == 1;
   EVP_MD_CTX_free(ctx);
   EVP_PKEY_free(pkey);
   return ok;
}

vault_witness_cp_verdict_t vault_witness_checkpoint_verify(const vault_witness_checkpoint_t *cp,
                                                           const vault_witness_anchor_t *anchors,
                                                           size_t n_anchors)
{
   if (!checkpoint_valid(cp) || (n_anchors && !anchors))
      return VAULT_WITNESS_CP_MALFORMED;

   const vault_witness_anchor_t *match = NULL;
   for (size_t i = 0; i < n_anchors; i++)
      if (CRYPTO_memcmp(anchors[i].key_id, cp->signer_key_id, VAULT_WITNESS_SIGNER_KEY_ID_LEN) == 0)
      {
         match = &anchors[i];
         break;
      }
   if (!match)
      return VAULT_WITNESS_CP_UNKNOWN_KEY;
   if (match->revoked)
      return VAULT_WITNESS_CP_REVOKED_KEY;

   uint8_t body[512];
   size_t len = 0;
   if (vault_witness_checkpoint_signable(cp, body, sizeof body, &len) != 0)
      return VAULT_WITNESS_CP_MALFORMED;
   int ok = ed25519_verify(match->ed25519_pub, body, len, cp->signature);
   OPENSSL_cleanse(body, sizeof body);
   return ok ? VAULT_WITNESS_CP_OK : VAULT_WITNESS_CP_BAD_SIG;
}

vault_witness_continuity_t
vault_witness_checkpoint_continuity(const vault_witness_checkpoint_t *cp,
                                    const uint8_t *expected_predecessor)
{
   if (!cp)
      return VAULT_WITNESS_CONTINUITY_BROKEN;
   if (!expected_predecessor)
      return VAULT_WITNESS_CONTINUITY_UNPROVEN;
   if (!cp->has_predecessor)
      return VAULT_WITNESS_CONTINUITY_BROKEN;
   return CRYPTO_memcmp(cp->predecessor_digest, expected_predecessor, 32) == 0
              ? VAULT_WITNESS_CONTINUITY_OK
              : VAULT_WITNESS_CONTINUITY_BROKEN;
}
