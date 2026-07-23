#include "vault_witness_offline.h"

#include <stdlib.h>
#include <string.h>

#include "vault_witness_export.h"
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

   /* Checkpoint signatures + continuity. */
   if (cps.n)
   {
      qsort(cps.v, cps.n, sizeof cps.v[0], cp_sort_cmp);
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

   free(recs.v);
   free(cps.v);
   free(pfs.v);
   return rc;
}
