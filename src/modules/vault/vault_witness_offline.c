#include "vault_witness_offline.h"

#include <openssl/sha.h>
#include <stdlib.h>
#include <string.h>

#include "vault_witness_export.h"
#include "vault_witness_merkle.h"
#include "vault_witness_proof.h"
#include "vault_witness_record.h"
#include "vault_witness_verify.h"

/* Bound total collected items so a hostile stream cannot exhaust memory. The
 * stream itself is already resident (the caller reads it whole), and the smallest
 * record frame is ~156 bytes, so the input size is the primary bound; this cap is
 * the backstop. Kept low enough that the worst case stays well under a gigabyte. */
#define OFFLINE_MAX_ITEMS (1u * 1024u * 1024u)

typedef struct
{
   vault_witness_record_t *v;
   size_t n, cap;
} rec_vec_t;
typedef struct
{
   vault_witness_checkpoint_t *v;
   size_t n, cap;
} cp_vec_t;
typedef struct
{
   vault_witness_proof_t *v;
   size_t n, cap;
} pf_vec_t;
/* A leaf snapshot is variable-length, so it is held by reference into the caller's
 * stream rather than copied. */
typedef struct
{
   uint64_t seq;
   const uint8_t *bytes;
   size_t len;
} snap_t;
typedef struct
{
   snap_t *v;
   size_t n, cap;
} sn_vec_t;

static int sn_push(sn_vec_t *a, const snap_t *r)
{
   if (a->n == a->cap)
   {
      size_t nc = a->cap ? a->cap * 2 : 32;
      if (nc > OFFLINE_MAX_ITEMS)
         return -1;
      void *nv = realloc(a->v, nc * sizeof *a->v);
      if (!nv)
         return -1;
      a->v = nv;
      a->cap = nc;
   }
   a->v[a->n++] = *r;
   return 0;
}

static int rec_push(rec_vec_t *a, const vault_witness_record_t *r)
{
   if (a->n == a->cap)
   {
      size_t nc = a->cap ? a->cap * 2 : 64;
      if (nc > OFFLINE_MAX_ITEMS)
         return -1;
      void *nv = realloc(a->v, nc * sizeof *a->v);
      if (!nv)
         return -1;
      a->v = nv;
      a->cap = nc;
   }
   a->v[a->n++] = *r;
   return 0;
}
static int cp_push(cp_vec_t *a, const vault_witness_checkpoint_t *r)
{
   if (a->n == a->cap)
   {
      size_t nc = a->cap ? a->cap * 2 : 32;
      if (nc > OFFLINE_MAX_ITEMS)
         return -1;
      void *nv = realloc(a->v, nc * sizeof *a->v);
      if (!nv)
         return -1;
      a->v = nv;
      a->cap = nc;
   }
   a->v[a->n++] = *r;
   return 0;
}
static int pf_push(pf_vec_t *a, const vault_witness_proof_t *r)
{
   if (a->n == a->cap)
   {
      size_t nc = a->cap ? a->cap * 2 : 32;
      if (nc > OFFLINE_MAX_ITEMS)
         return -1;
      void *nv = realloc(a->v, nc * sizeof *a->v);
      if (!nv)
         return -1;
      a->v = nv;
      a->cap = nc;
   }
   a->v[a->n++] = *r;
   return 0;
}

static int rec_sort_cmp(const void *a, const void *b)
{
   const vault_witness_record_t *x = a, *y = b;
   int c = strcmp(x->tenant, y->tenant);
   if (c)
      return c;
   c = strcmp(x->provider, y->provider);
   if (c)
      return c;
   return (x->shard_seq < y->shard_seq) ? -1 : (x->shard_seq > y->shard_seq);
}

static int cp_sort_cmp(const void *a, const void *b)
{
   const vault_witness_checkpoint_t *x = a, *y = b;
   return (x->seq < y->seq) ? -1 : (x->seq > y->seq);
}


/* Parse a leaf snapshot and rebuild its sparse-Merkle root.
 *
 * Wire form (produced by the checkpoint producer):
 *   u32 count, then per leaf: u16 tlen, tenant, u16 plen, provider, u64 seq, head[32]
 *
 * Returns 1 if the rebuilt root equals `want_root`, 0 if it does not, and -1 if the
 * snapshot is malformed (which the caller treats the same as a mismatch: a snapshot
 * that cannot be parsed cannot support the comparison it exists for).
 */
static int snapshot_rebuilds_root(const uint8_t *p, size_t len, const uint8_t want_root[32])
{
   if (!p || len < 4 || !want_root)
      return -1;
   size_t off = 0;
   uint32_t count = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
                    (uint32_t)p[3];
   off = 4;
   /* The producer refuses to build past the ceiling, so a snapshot claiming more
    * leaves than that is malformed rather than merely large. */
   if (count == 0 || count > VAULT_WITNESS_SHARD_CEILING)
      return -1;
   vault_witness_leaf_t *leaves = calloc(count, sizeof *leaves);
   if (!leaves)
      return -1;
   int rc = -1;
   for (uint32_t i = 0; i < count; i++)
   {
      char tenant[VAULT_WITNESS_TENANT_MAX + 1], provider[VAULT_WITNESS_PROVIDER_MAX + 1];
      if (off + 2 > len)
         goto out;
      size_t tl = ((size_t)p[off] << 8) | p[off + 1];
      off += 2;
      if (tl > VAULT_WITNESS_TENANT_MAX || off + tl > len)
         goto out;
      memcpy(tenant, p + off, tl);
      tenant[tl] = '\0';
      off += tl;
      if (off + 2 > len)
         goto out;
      size_t pl = ((size_t)p[off] << 8) | p[off + 1];
      off += 2;
      if (pl > VAULT_WITNESS_PROVIDER_MAX || off + pl > len)
         goto out;
      memcpy(provider, p + off, pl);
      provider[pl] = '\0';
      off += pl;
      if (off + 8 + 32 > len)
         goto out;
      uint64_t seq = 0;
      for (unsigned b = 0; b < 8; b++)
         seq = (seq << 8) | p[off + b];
      off += 8;
      const uint8_t *head = p + off;
      off += 32;
      if (vault_witness_shard_key_hash(tenant, provider, leaves[i].key) != 0 ||
          vault_witness_leaf_hash(tenant, provider, seq, head, leaves[i].hash) != 0)
         goto out;
   }
   /* Trailing bytes mean the snapshot is not what it declares itself to be. */
   if (off != len)
      goto out;
   uint8_t root[32];
   if (vault_witness_merkle_root(leaves, count, root) != 0)
      goto out;
   rc = (memcmp(root, want_root, 32) == 0) ? 1 : 0;

out:
   free(leaves);
   return rc;
}

int vault_witness_offline_verify(const uint8_t *stream, size_t stream_len,
                                 const vault_witness_anchor_t *anchors, size_t n_anchors,
                                 vault_witness_offline_report_t *report)
{
   if (!report || (stream_len && !stream) || (n_anchors && !anchors))
      return -1;
   memset(report, 0, sizeof *report);
   report->continuity = VAULT_WITNESS_CONTINUITY_OK;

   rec_vec_t recs = {0};
   cp_vec_t cps = {0};
   pf_vec_t pfs = {0};
   sn_vec_t sns = {0};
   int rc = 0;

   /* Parse the frame stream. */
   size_t off = 0;
   while (off + VAULT_WITNESS_EXPORT_HEADER_LEN <= stream_len)
   {
      /* payload_len lives in the export header; parse needs the full frame, so read
       * the declared length from the header first. */
      const uint8_t *hdr = stream + off;
      uint32_t plen = ((uint32_t)hdr[12] << 24) | ((uint32_t)hdr[13] << 16) |
                      ((uint32_t)hdr[14] << 8) | (uint32_t)hdr[15];
      size_t frame_len = (size_t)VAULT_WITNESS_EXPORT_HEADER_LEN + plen;
      if (plen > stream_len || off + frame_len > stream_len)
      {
         report->malformed = 1;
         report->any_tamper = 1;
         break;
      }
      vault_witness_export_kind_t kind;
      const uint8_t *payload = NULL;
      size_t payload_len = 0;
      vault_witness_export_parse_t pr =
          vault_witness_export_parse(stream + off, frame_len, &kind, &payload, &payload_len);
      report->frames++;
      off += frame_len;
      if (pr != VAULT_WITNESS_EXPORT_PARSE_OK)
      {
         report->malformed = 1;
         report->any_tamper = 1;
         continue;
      }
      if (kind == VAULT_WITNESS_EXPORT_RECORD)
      {
         vault_witness_record_t r;
         if (vault_witness_record_decode(payload, payload_len, &r) != 0 || rec_push(&recs, &r) != 0)
         {
            report->malformed = 1;
            report->any_tamper = 1;
            continue;
         }
         report->records++;
      }
      else if (kind == VAULT_WITNESS_EXPORT_CHECKPOINT)
      {
         vault_witness_checkpoint_t cp;
         if (vault_witness_checkpoint_decode(payload, payload_len, &cp) != 0 ||
             cp_push(&cps, &cp) != 0)
         {
            report->malformed = 1;
            report->any_tamper = 1;
            continue;
         }
         report->checkpoints++;
      }
      else if (kind == VAULT_WITNESS_EXPORT_PROOF)
      {
         vault_witness_proof_t p;
         if (vault_witness_proof_decode(payload, payload_len, &p) != 0 || pf_push(&pfs, &p) != 0)
         {
            report->malformed = 1;
            report->any_tamper = 1;
            continue;
         }
         report->proofs++;
      }
      else if (kind == VAULT_WITNESS_EXPORT_SNAPSHOT)
      {
         /* Payload: u64 checkpoint_seq (big-endian) || the stored snapshot bytes. */
         if (payload_len < 8)
         {
            report->malformed = 1;
            report->any_tamper = 1;
            continue;
         }
         snap_t sn;
         sn.seq = 0;
         for (unsigned i = 0; i < 8; i++)
            sn.seq = (sn.seq << 8) | payload[i];
         sn.bytes = payload + 8;
         sn.len = payload_len - 8;
         if (sn_push(&sns, &sn) != 0)
         {
            report->malformed = 1;
            report->any_tamper = 1;
            continue;
         }
         report->snapshots++;
      }
      else
         report->unknown_frames++;
   }
   if (off != stream_len)
   {
      report->malformed = 1;
      report->any_tamper = 1;
   }

   /* Per-shard record chains. A retained stream may legitimately repeat records
    * (re-emission after a restart, a collector retry), so byte-identical repeats at
    * the same shard_seq are collapsed. Two DIFFERENT records at the same shard_seq
    * are a fork — the attacker rewrote history — and are hard tamper evidence. */
   if (recs.n)
   {
      qsort(recs.v, recs.n, sizeof recs.v[0], rec_sort_cmp);
      size_t w = 0;
      for (size_t i = 0; i < recs.n; i++)
      {
         if (w > 0 && strcmp(recs.v[w - 1].tenant, recs.v[i].tenant) == 0 &&
             strcmp(recs.v[w - 1].provider, recs.v[i].provider) == 0 &&
             recs.v[w - 1].shard_seq == recs.v[i].shard_seq)
         {
            uint8_t da[32], db[32];
            int same = vault_witness_record_digest(&recs.v[w - 1], da) == 0 &&
                       vault_witness_record_digest(&recs.v[i], db) == 0 &&
                       memcmp(da, db, 32) == 0;
            if (same)
               report->records_duplicate++;
            else
            {
               report->records_conflict++;
               report->any_tamper = 1;
            }
            continue; /* collapse either way; the conflict is already recorded */
         }
         recs.v[w++] = recs.v[i];
      }
      recs.n = w;
      size_t i = 0;
      while (i < recs.n)
      {
         size_t j = i + 1;
         while (j < recs.n && strcmp(recs.v[j].tenant, recs.v[i].tenant) == 0 &&
                strcmp(recs.v[j].provider, recs.v[i].provider) == 0)
            j++;
         size_t brk = 0;
         vault_witness_chain_result_t cr = vault_witness_verify_chain(recs.v + i, j - i, &brk);
         if (cr == VAULT_WITNESS_CHAIN_OK)
            report->shards_ok++;
         else
         {
            report->shards_broken++;
            report->any_tamper = 1;
         }
         i = j;
      }
   }

   /* Checkpoint signatures + continuity. As with records, a retained stream may
    * legitimately repeat a checkpoint: emission re-sends one whose leaf snapshot
    * was not accepted, and a reset cursor re-sends the whole run. Byte-identical
    * repeats at the same seq are therefore collapsed. Two DIFFERENT checkpoints at
    * one seq are a fork — the signer certified two histories — and are hard tamper
    * evidence. Without this collapse a benign re-emission reads as CONTINUITY_BROKEN
    * (the duplicate's has_predecessor is false mid-run), which is a false tampering
    * alarm on a completely healthy system. */
   if (cps.n)
   {
      qsort(cps.v, cps.n, sizeof cps.v[0], cp_sort_cmp);
      size_t cw = 0;
      for (size_t i = 0; i < cps.n; i++)
      {
         if (cw > 0 && cps.v[cw - 1].seq == cps.v[i].seq)
         {
            uint8_t da[32], db[32];
            int same = vault_witness_checkpoint_digest(&cps.v[cw - 1], da) == 0 &&
                       vault_witness_checkpoint_digest(&cps.v[i], db) == 0 &&
                       memcmp(da, db, 32) == 0;
            if (same)
               report->checkpoints_duplicate++;
            else
            {
               report->checkpoints_conflict++;
               report->any_tamper = 1;
            }
            continue; /* collapse either way; a conflict is already recorded */
         }
         cps.v[cw++] = cps.v[i];
      }
      cps.n = cw;
      for (size_t k = 0; k < cps.n; k++)
      {
         switch (vault_witness_checkpoint_verify(&cps.v[k], anchors, n_anchors))
         {
         case VAULT_WITNESS_CP_OK:
            report->checkpoints_ok++;
            break;
         case VAULT_WITNESS_CP_BAD_SIG:
            report->checkpoints_bad_sig++;
            report->any_tamper = 1;
            break;
         case VAULT_WITNESS_CP_UNKNOWN_KEY:
            report->checkpoints_unknown_key++;
            report->any_tamper = 1;
            break;
         case VAULT_WITNESS_CP_REVOKED_KEY:
            report->checkpoints_revoked++;
            report->any_tamper = 1;
            break;
         default:
            report->any_tamper = 1;
            break;
         }
      }
      size_t gap = 0;
      report->continuity = vault_witness_verify_checkpoint_run(cps.v, cps.n, &gap);
      if (report->continuity == VAULT_WITNESS_CONTINUITY_BROKEN)
         report->any_tamper = 1;
      /* CONTINUITY_UNPROVEN is a work item, not a hard tamper: it does not set
       * any_tamper, but the caller must surface it (never treat it as clean). */
   }

   /* Proofs against their matching checkpoint's root. */
   for (size_t k = 0; k < pfs.n; k++)
   {
      const vault_witness_checkpoint_t *match = NULL;
      for (size_t c = 0; c < cps.n; c++)
         if (cps.v[c].seq == pfs.v[k].checkpoint_seq)
         {
            match = &cps.v[c];
            break;
         }
      if (!match)
      {
         report->proofs_unmatched++;
         continue; /* cannot verify without the checkpoint; not itself a tamper */
      }
      if (vault_witness_proof_verify(&pfs.v[k], match->root))
         report->proofs_ok++;
      else
      {
         report->proofs_bad++;
         report->any_tamper = 1;
      }
   }

   /* Leaf snapshots against the leaf_snapshot_digest inside their own signed
    * checkpoint. This is what makes an emitted snapshot trustworthy enough to
    * rebuild the tree from: the signature already commits to the digest, so a
    * substituted or truncated snapshot cannot pass. A snapshot whose checkpoint is
    * not in this stream is unverifiable, not tampered — it is reported separately
    * so an operator can go fetch the missing checkpoint. */
   for (size_t k = 0; k < sns.n; k++)
   {
      const vault_witness_checkpoint_t *match = NULL;
      for (size_t c = 0; c < cps.n; c++)
         if (cps.v[c].seq == sns.v[k].seq)
         {
            match = &cps.v[c];
            break;
         }
      if (!match)
      {
         report->snapshots_unmatched++;
         continue;
      }
      uint8_t d[32];
      SHA256(sns.v[k].len ? sns.v[k].bytes : (const uint8_t *)"", sns.v[k].len, d);
      if (memcmp(d, match->leaf_snapshot_digest, 32) != 0)
      {
         report->snapshots_bad++;
         report->any_tamper = 1;
         continue;
      }
      /* The digest match proves the snapshot is the byte string the signature
       * committed to. It does NOT prove those bytes describe the leaf set the
       * checkpoint's root covers — a checkpoint carrying a root and a snapshot
       * digest that do not correspond would still pass. Rebuilding the tree from the
       * snapshot and comparing to the signed root is what makes the snapshot usable
       * for the cross-gap leaf comparison, so it is checked rather than assumed. */
      int root_ok = snapshot_rebuilds_root(sns.v[k].bytes, sns.v[k].len, match->root);
      if (root_ok == 1)
         report->snapshots_ok++;
      else
      {
         report->snapshots_root_mismatch++;
         report->any_tamper = 1;
      }
   }

   free(recs.v);
   free(cps.v);
   free(pfs.v);
   free(sns.v);
   return rc;
}
