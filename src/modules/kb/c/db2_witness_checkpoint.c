#include "db2_witness_checkpoint.h"
#include "db2_vault_witness_provider.h"

#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "db2_internal.h" /* db2_conn */
#include "db_postgres.h"  /* aimee_pg_* */
#include "modules/vault/vault_witness_checkpoint.h"
#include "modules/vault/vault_witness_export.h"
#include "modules/vault/vault_witness_merkle.h"
#include "modules/vault/vault_witness_record.h"

#define CP_ERR 256

static db2_vault_witness_provider_t witness_provider;

void aimee_db2_register_vault_witness_provider(const db2_vault_witness_provider_t *provider)
{
   if (provider)
      witness_provider = *provider;
   else
      memset(&witness_provider, 0, sizeof(witness_provider));
}

static int provider_bytes_result(int rc, uint8_t *out, size_t len)
{
   if (rc == 0)
      return 0;
   if (out)
      OPENSSL_cleanse(out, len);
   return -1;
}

int db2_vault_witness_checkpoint_digest(const vault_witness_checkpoint_t *checkpoint,
                                        uint8_t digest[32])
{
   if (digest)
      OPENSSL_cleanse(digest, 32);
   if (!checkpoint || !digest || !witness_provider.checkpoint_digest)
      return -1;
   return provider_bytes_result(witness_provider.checkpoint_digest(checkpoint, digest), digest, 32);
}

int db2_vault_witness_checkpoint_encode(const vault_witness_checkpoint_t *checkpoint, uint8_t *out,
                                        size_t cap, size_t *out_len)
{
   if (out_len)
      *out_len = 0;
   if (!checkpoint || !out || !out_len || cap == 0 || cap > VAULT_WITNESS_CHECKPOINT_WIRE_MAX ||
       !witness_provider.checkpoint_encode ||
       witness_provider.checkpoint_encode(checkpoint, out, cap, out_len) != 0 || *out_len == 0 ||
       *out_len > cap)
   {
      if (out)
         OPENSSL_cleanse(out, cap);
      if (out_len)
         *out_len = 0;
      return -1;
   }
   return 0;
}

int db2_vault_witness_checkpoint_sign(vault_witness_checkpoint_t *checkpoint)
{
   if (!checkpoint || !witness_provider.checkpoint_sign ||
       witness_provider.checkpoint_sign(checkpoint) != 0 ||
       checkpoint->sig_alg != VAULT_WITNESS_SIG_ED25519 || checkpoint->sig_version == 0)
   {
      if (checkpoint)
      {
         OPENSSL_cleanse(checkpoint->signer_key_id, sizeof(checkpoint->signer_key_id));
         OPENSSL_cleanse(checkpoint->signature, sizeof(checkpoint->signature));
         checkpoint->sig_alg = 0;
         checkpoint->sig_version = 0;
      }
      return -1;
   }
   return 0;
}

int db2_vault_witness_checkpoint_verify(const vault_witness_checkpoint_t *checkpoint,
                                        const vault_witness_anchor_t *anchors, size_t anchor_count)
{
   if (!checkpoint || (!anchors && anchor_count != 0) || !witness_provider.checkpoint_verify)
      return VAULT_WITNESS_CP_MALFORMED;
   int verdict = witness_provider.checkpoint_verify(checkpoint, anchors, anchor_count);
   return verdict >= VAULT_WITNESS_CP_OK && verdict <= VAULT_WITNESS_CP_BAD_SIG
              ? verdict
              : VAULT_WITNESS_CP_MALFORMED;
}

int db2_vault_witness_export_frame(int kind, const uint8_t *payload, size_t payload_len,
                                   uint8_t *out, size_t cap, size_t *out_len)
{
   if (out_len)
      *out_len = 0;
   size_t expected = payload_len <= SIZE_MAX - VAULT_WITNESS_EXPORT_HEADER_LEN
                         ? payload_len + VAULT_WITNESS_EXPORT_HEADER_LEN
                         : 0;
   if (kind < VAULT_WITNESS_EXPORT_RECORD || kind > VAULT_WITNESS_EXPORT_SNAPSHOT ||
       (!payload && payload_len != 0) || !out || !out_len || expected == 0 || cap < expected ||
       !witness_provider.export_frame ||
       witness_provider.export_frame(kind, payload, payload_len, out, cap, out_len) != 0 ||
       *out_len != expected)
   {
      if (out)
         OPENSSL_cleanse(out, cap);
      if (out_len)
         *out_len = 0;
      return -1;
   }
   return 0;
}

int db2_vault_witness_leaf_hash(const char *tenant, const char *provider, uint64_t sequence,
                                const uint8_t head_hash[32], uint8_t out[32])
{
   if (out)
      OPENSSL_cleanse(out, 32);
   if (!tenant || !provider || !head_hash || !out || !witness_provider.leaf_hash)
      return -1;
   return provider_bytes_result(
       witness_provider.leaf_hash(tenant, provider, sequence, head_hash, out), out, 32);
}

int db2_vault_witness_merkle_root(const vault_witness_leaf_t *leaves, size_t count,
                                  uint8_t root[32])
{
   if (root)
      OPENSSL_cleanse(root, 32);
   if ((!leaves && count != 0) || count > VAULT_WITNESS_SHARD_CEILING || !root ||
       !witness_provider.merkle_root)
      return -1;
   return provider_bytes_result(witness_provider.merkle_root(leaves, count, root), root, 32);
}

int db2_vault_witness_record_digest(const vault_witness_record_t *record, uint8_t digest[32])
{
   if (digest)
      OPENSSL_cleanse(digest, 32);
   if (!record || !digest || !witness_provider.record_digest)
      return -1;
   return provider_bytes_result(witness_provider.record_digest(record, digest), digest, 32);
}

int db2_vault_witness_record_encode(const vault_witness_record_t *record, uint8_t *out, size_t cap,
                                    size_t *out_len)
{
   if (out_len)
      *out_len = 0;
   if (!record || !out || !out_len || cap == 0 || cap > VAULT_WITNESS_RECORD_MAX ||
       !witness_provider.record_encode ||
       witness_provider.record_encode(record, out, cap, out_len) != 0 || *out_len == 0 ||
       *out_len > cap)
   {
      if (out)
         OPENSSL_cleanse(out, cap);
      if (out_len)
         *out_len = 0;
      return -1;
   }
   return 0;
}

int db2_vault_witness_shard_key_hash(const char *tenant, const char *provider, uint8_t out[8])
{
   if (out)
      OPENSSL_cleanse(out, 8);
   if (!tenant || !provider || !out || !witness_provider.shard_key_hash)
      return -1;
   return provider_bytes_result(witness_provider.shard_key_hash(tenant, provider, out), out, 8);
}

int db2_vault_witness_signer_identity(uint8_t public_key[32], uint8_t key_id[16])
{
   if (public_key)
      OPENSSL_cleanse(public_key, 32);
   if (key_id)
      OPENSSL_cleanse(key_id, 16);
   if (!public_key || !key_id || !witness_provider.signer_identity ||
       witness_provider.signer_identity(public_key, key_id) != 0)
   {
      if (public_key)
         OPENSSL_cleanse(public_key, 32);
      if (key_id)
         OPENSSL_cleanse(key_id, 16);
      return -1;
   }
   return 0;
}

int db2_vault_witness_verify_checkpoint_run(const vault_witness_checkpoint_t *checkpoints,
                                            size_t count, size_t *gap_after_index)
{
   if (gap_after_index)
      *gap_after_index = 0;
   if ((!checkpoints && count != 0) || !witness_provider.verify_checkpoint_run)
      return VAULT_WITNESS_CONTINUITY_BROKEN;
   int verdict = witness_provider.verify_checkpoint_run(checkpoints, count, gap_after_index);
   return verdict >= VAULT_WITNESS_CONTINUITY_OK && verdict <= VAULT_WITNESS_CONTINUITY_BROKEN
              ? verdict
              : VAULT_WITNESS_CONTINUITY_BROKEN;
}

/* Leaf snapshot wire format (canonical, so a consumer can rebuild the tree):
 *   u32 count, then per leaf: u16 tlen, tenant, u16 plen, provider, u64 seq, head[32].
 * leaf_snapshot_digest = SHA-256 over these bytes. */
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

/* A growable byte buffer for the leaf snapshot. */
typedef struct
{
   uint8_t *buf;
   size_t len, cap;
   int oom;
} bytebuf_t;

static void bb_append(bytebuf_t *b, const void *p, size_t n)
{
   if (b->oom)
      return;
   /* Size arithmetic is overflow-checked so a hostile or corrupt length can never
    * under-allocate and then memcpy out of bounds. Callers bound n well below this
    * (a leaf's tenant/provider are capped at VAULT_WITNESS_*_MAX), but the buffer is
    * a generic helper and defends itself regardless of caller. */
   if (n > SIZE_MAX - b->len)
   {
      b->oom = 1;
      return;
   }
   if (b->len + n > b->cap)
   {
      size_t ncap = b->cap ? b->cap * 2 : 4096;
      while (ncap < b->len + n)
      {
         if (ncap > SIZE_MAX / 2)
         {
            b->oom = 1;
            return;
         }
         ncap *= 2;
      }
      uint8_t *nb = realloc(b->buf, ncap);
      if (!nb)
      {
         b->oom = 1;
         return;
      }
      b->buf = nb;
      b->cap = ncap;
   }
   memcpy(b->buf + b->len, p, n);
   b->len += n;
}

static int leaf_cmp(const void *a, const void *b)
{
   return memcmp(((const vault_witness_leaf_t *)a)->key, ((const vault_witness_leaf_t *)b)->key, 8);
}

static db2_witness_checkpoint_result_t map_sqlstate(const char *st)
{
   if (!st)
      return DB2_WITNESS_CP_ERROR;
   if (strcmp(st, "P7W01") == 0)
      return DB2_WITNESS_CP_HEAD_MISMATCH;
   if (strcmp(st, "P7W02") == 0)
      return DB2_WITNESS_CP_CEILING;
   if (strcmp(st, "P7W03") == 0)
      return DB2_WITNESS_CP_FENCE_STALE;
   /* P7W05 (seq stale) means a concurrent producer committed the next checkpoint;
    * retry picks up the new max. Treat like a serialization failure. */
   if (strcmp(st, "P7W05") == 0 || strcmp(st, "40001") == 0 || strcmp(st, "40P01") == 0 ||
       strcmp(st, "23505") == 0)
      return DB2_WITNESS_CP_TRANSIENT;
   return DB2_WITNESS_CP_ERROR;
}

/* Reconstruct the previous checkpoint's digest (its predecessor role for the new
 * one) from its stored columns, keeping all digesting in C. Returns 0 and sets
 * *has_pred/pred, or -1 on error. has_pred is 0 when no prior checkpoint exists. */
static int previous_digest(void *conn, int *has_pred, uint8_t pred[32])
{
   *has_pred = 0;
   memset(pred, 0, 32);
   char err[CP_ERR];
   /* seq is BIGINT, read via aimee_pg_column_int64, which does not touch the single
    * blob cache the bytea columns share — so it folds into this one query safely and
    * the input to the digest is provably exactly one row. */
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT root,has_predecessor,predecessor_digest,shard_count,leaf_snapshot_digest,"
       "signer_key_id,sig_alg,sig_version,created_at,seq FROM kb_vault_witness_checkpoint "
       "ORDER BY seq DESC LIMIT 1",
       err, sizeof err);
   if (!st)
      return -1;
   aimee_pg_step_t sr = aimee_pg_step(st, err, sizeof err);
   if (sr == AIMEE_PG_ERR)
   {
      aimee_pg_finalize(st);
      return -1;
   }
   if (sr != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st); /* no prior checkpoint: first one has no predecessor */
      return 0;
   }
   vault_witness_checkpoint_t prev;
   memset(&prev, 0, sizeof prev);
   prev.version = 1;
   /* aimee_pg_column_blob caches ONE blob per statement and frees the previous on
    * the next call, so every bytea must be copied out BEFORE reading the next one. */
   int ok = 1;
   /* aimee_pg_column_text also returns per-statement storage; copy the boolean out
    * into a local immediately so a future column-read reordering cannot invalidate
    * the pointer before we consume it. */
   const char *hp = aimee_pg_column_text(st, 1);
   int hp_present = (hp != NULL);
   prev.has_predecessor = (hp && hp[0] == 't');
   const void *b = aimee_pg_column_blob(st, 0);
   ok = ok && b && aimee_pg_column_bytes(st, 0) == 32;
   if (ok)
      memcpy(prev.root, b, 32);
   b = aimee_pg_column_blob(st, 2);
   ok = ok && b && aimee_pg_column_bytes(st, 2) == 32;
   if (ok)
      memcpy(prev.predecessor_digest, b, 32);
   prev.shard_count = (uint64_t)aimee_pg_column_int64(st, 3);
   b = aimee_pg_column_blob(st, 4);
   ok = ok && b && aimee_pg_column_bytes(st, 4) == 32;
   if (ok)
      memcpy(prev.leaf_snapshot_digest, b, 32);
   b = aimee_pg_column_blob(st, 5);
   ok = ok && b && aimee_pg_column_bytes(st, 5) == 16;
   if (ok)
      memcpy(prev.signer_key_id, b, VAULT_WITNESS_SIGNER_KEY_ID_LEN);
   prev.sig_alg = (uint16_t)aimee_pg_column_int64(st, 6);
   prev.sig_version = (uint16_t)aimee_pg_column_int64(st, 7);
   const char *ca = aimee_pg_column_text(st, 8);
   ok = ok && hp_present && ca;
   if (ok)
      snprintf(prev.created_at, sizeof prev.created_at, "%s", ca);
   prev.seq = (uint64_t)aimee_pg_column_int64(st, 9); /* seq, from the same row */
   aimee_pg_finalize(st);
   if (!ok)
      return -1;
   if (db2_vault_witness_checkpoint_digest(&prev, pred) != 0)
      return -1;
   *has_pred = 1;
   return 0;
}

db2_witness_checkpoint_result_t db2_witness_checkpoint_produce(int64_t *out_seq)
{
   void *conn = db2_conn();
   if (!conn)
      return DB2_WITNESS_CP_TRANSIENT;
   char err[CP_ERR], state[6] = "";
   db2_witness_checkpoint_result_t rc = DB2_WITNESS_CP_ERROR;
   vault_witness_leaf_t *leaves = NULL;
   size_t n = 0, cap = 0;
   bytebuf_t snap = {0};

   if (aimee_pg_exec_sqlstate(conn, "BEGIN ISOLATION LEVEL REPEATABLE READ", state, err,
                              sizeof err) != 0)
      return DB2_WITNESS_CP_TRANSIENT;

   /* 1. Fence to revalidate at persist time. Read via the definer accessor, not
    * kb_vault_control directly: the control row is owner-only, and the runtime role
    * the cadence connects as on the hardened tier has no access to it. */
   int64_t fence = -1;
   {
      aimee_pg_stmt_t *st =
          aimee_pg_prepare(conn, "SELECT org_vault_witness_control_fence()", err, sizeof err);
      if (st && aimee_pg_step(st, err, sizeof err) == AIMEE_PG_ROW)
         fence = aimee_pg_column_int64(st, 0);
      if (st)
         aimee_pg_finalize(st);
   }
   if (fence < 0)
      goto rollback;

   /* 2. Verified leaves (raises P7W01 / P7W02 inside the scan). */
   {
      aimee_pg_stmt_t *st = aimee_pg_prepare(
          conn, "SELECT tenant,provider,seq,head_hash FROM org_vault_witness_checkpoint_leaves()",
          err, sizeof err);
      if (!st)
         goto rollback;
      aimee_pg_step_t sr;
      while ((sr = aimee_pg_step(st, err, sizeof err)) == AIMEE_PG_ROW)
      {
         const char *tenant = aimee_pg_column_text(st, 0);
         const char *provider = aimee_pg_column_text(st, 1);
         int64_t seq = aimee_pg_column_int64(st, 2);
         const void *head = aimee_pg_column_blob(st, 3);
         if (!tenant || !provider || !head || aimee_pg_column_bytes(st, 3) != 32 || seq <= 0)
         {
            aimee_pg_finalize(st);
            goto rollback;
         }
         if (n == cap)
         {
            size_t ncap = cap ? cap * 2 : 64;
            vault_witness_leaf_t *nl = realloc(leaves, ncap * sizeof *leaves);
            if (!nl)
            {
               aimee_pg_finalize(st);
               goto rollback;
            }
            leaves = nl;
            cap = ncap;
         }
         if (db2_vault_witness_shard_key_hash(tenant, provider, leaves[n].key) != 0 ||
             db2_vault_witness_leaf_hash(tenant, provider, (uint64_t)seq, head, leaves[n].hash) !=
                 0)
         {
            aimee_pg_finalize(st);
            goto rollback;
         }
         /* Snapshot: u16 tlen, tenant, u16 plen, provider, u64 seq, head[32]. */
         uint8_t hdr[2];
         size_t tl = strlen(tenant), pl = strlen(provider);
         /* Refuse at the source rather than serialise a snapshot the offline verifier
          * would reject. tenant/provider are capped at VAULT_WITNESS_*_MAX when a record
          * is appended, so this only fires if that invariant was violated upstream — in
          * which case emitting evidence the verifier cannot rebuild (and a u16 length that
          * would truncate) is worse than failing the checkpoint loudly here. */
         if (tl > VAULT_WITNESS_TENANT_MAX || pl > VAULT_WITNESS_PROVIDER_MAX)
         {
            aimee_pg_finalize(st);
            goto rollback;
         }
         put_u16(hdr, (uint16_t)tl);
         bb_append(&snap, hdr, 2);
         bb_append(&snap, tenant, tl);
         put_u16(hdr, (uint16_t)pl);
         bb_append(&snap, hdr, 2);
         bb_append(&snap, provider, pl);
         uint8_t sb[8];
         put_u64(sb, (uint64_t)seq);
         bb_append(&snap, sb, 8);
         bb_append(&snap, head, 32);
         n++;
      }
      const char *ss = aimee_pg_sqlstate(st);
      char stbuf[6] = "";
      if (ss)
         snprintf(stbuf, sizeof stbuf, "%s", ss);
      aimee_pg_finalize(st);
      if (sr == AIMEE_PG_ERR)
      {
         rc = map_sqlstate(stbuf);
         goto rollback;
      }
      if (snap.oom)
         goto rollback;
   }

   /* No non-empty shards yet: nothing to checkpoint. The cadence treats this as a
    * benign no-op, so an idle kb does not accrue empty heartbeat checkpoints. The
    * chain begins at the first checkpoint that covers real activity. */
   if (n == 0)
   {
      rc = DB2_WITNESS_CP_EMPTY;
      goto rollback;
   }

   /* Finalize the stored snapshot as count || body, so leaf_snapshot_digest is
    * SHA-256 over exactly the stored bytes (a verifier recomputes it directly). */
   {
      bytebuf_t full = {0};
      uint8_t cnt[4];
      put_u32(cnt, (uint32_t)n);
      bb_append(&full, cnt, 4);
      if (snap.len)
         bb_append(&full, snap.buf, snap.len);
      free(snap.buf);
      snap = full;
      if (snap.oom)
         goto rollback;
   }

   /* 3. Root over the sorted leaves. */
   if (n > 1)
      qsort(leaves, n, sizeof *leaves, leaf_cmp);
   uint8_t root[32];
   if (db2_vault_witness_merkle_root(leaves, n, root) != 0)
      goto rollback; /* duplicate key collision, or over ceiling */

   /* 4. Predecessor digest from the prior checkpoint (C-side digesting). */
   int has_pred = 0;
   uint8_t pred[32];
   if (previous_digest(conn, &has_pred, pred) != 0)
      goto rollback;

   /* Next monotonic seq. It is part of the signed body, so it must be fixed BEFORE
    * signing; persist re-verifies it is still the next value under the fence lock. */
   int64_t next_seq = -1;
   {
      aimee_pg_stmt_t *st = aimee_pg_prepare(
          conn, "SELECT COALESCE(max(seq),0)+1 FROM kb_vault_witness_checkpoint", err, sizeof err);
      if (st && aimee_pg_step(st, err, sizeof err) == AIMEE_PG_ROW)
         next_seq = aimee_pg_column_int64(st, 0);
      if (st)
         aimee_pg_finalize(st);
   }
   if (next_seq < 1)
      goto rollback;

   /* 5. Build + 6. sign. created_at from the DB for a real timestamp. */
   vault_witness_checkpoint_t cp;
   memset(&cp, 0, sizeof cp);
   cp.version = 1;
   cp.seq = (uint64_t)next_seq;
   cp.has_predecessor = has_pred;
   if (has_pred)
      memcpy(cp.predecessor_digest, pred, 32);
   cp.shard_count = n;
   memcpy(cp.root, root, 32);
   /* leaf_snapshot_digest = SHA-256 over the exact stored snapshot bytes. */
   SHA256(snap.buf ? snap.buf : (const uint8_t *)"", snap.len, cp.leaf_snapshot_digest);
   cp.sig_version = 1;
   {
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, "SELECT pg_now_text()", err, sizeof err);
      const char *ts = NULL;
      if (st && aimee_pg_step(st, err, sizeof err) == AIMEE_PG_ROW)
         ts = aimee_pg_column_text(st, 0);
      snprintf(cp.created_at, sizeof cp.created_at, "%s", ts ? ts : "1970-01-01 00:00:00");
      if (st)
         aimee_pg_finalize(st);
   }
   if (db2_vault_witness_checkpoint_sign(&cp) != 0)
      goto rollback;

   /* 7. Persist (fenced, monotonic). */
   int64_t new_seq = -1;
   {
      aimee_pg_stmt_t *st = aimee_pg_prepare(
          conn,
          "SELECT "
          "org_vault_witness_checkpoint_persist(?1,?2,?3::boolean,?4,?5,?6,?7,?8,?9::smallint,"
          "?10,?11,?12)",
          err, sizeof err);
      if (!st)
         goto rollback;
      int bad =
          aimee_pg_bind_int64(st, "?1", (int64_t)cp.seq) != 0 ||
          aimee_pg_bind_blob(st, "?2", cp.root, 32) != 0 ||
          aimee_pg_bind_text(st, "?3", cp.has_predecessor ? "true" : "false") != 0 ||
          aimee_pg_bind_blob(st, "?4", cp.predecessor_digest, 32) != 0 ||
          aimee_pg_bind_int64(st, "?5", (int64_t)cp.shard_count) != 0 ||
          aimee_pg_bind_blob(st, "?6", snap.buf ? snap.buf : (const uint8_t *)"", (int)snap.len) !=
              0 ||
          aimee_pg_bind_blob(st, "?7", cp.leaf_snapshot_digest, 32) != 0 ||
          aimee_pg_bind_blob(st, "?8", cp.signer_key_id, VAULT_WITNESS_SIGNER_KEY_ID_LEN) != 0 ||
          aimee_pg_bind_int64(st, "?9", cp.sig_alg) != 0 ||
          aimee_pg_bind_int64(st, "?10", cp.sig_version) != 0 ||
          aimee_pg_bind_blob(st, "?11", cp.signature, 64) != 0 ||
          aimee_pg_bind_int64(st, "?12", fence) != 0;
      if (bad)
      {
         aimee_pg_finalize(st);
         goto rollback;
      }
      aimee_pg_step_t sr = aimee_pg_step(st, err, sizeof err);
      if (sr == AIMEE_PG_ROW)
         new_seq = aimee_pg_column_int64(st, 0);
      const char *ss = aimee_pg_sqlstate(st);
      char stbuf[6] = "";
      if (ss)
         snprintf(stbuf, sizeof stbuf, "%s", ss);
      aimee_pg_finalize(st);
      if (sr == AIMEE_PG_ERR)
      {
         rc = map_sqlstate(stbuf);
         goto rollback;
      }
   }
   if (new_seq < 0)
      goto rollback;

   if (aimee_pg_exec(conn, "COMMIT", err, sizeof err) != 0)
   {
      rc = DB2_WITNESS_CP_TRANSIENT;
      goto rollback;
   }
   if (out_seq)
      *out_seq = new_seq;
   free(leaves);
   free(snap.buf);
   return DB2_WITNESS_CP_OK;

rollback:
   aimee_pg_exec(conn, "ROLLBACK", err, sizeof err);
   free(leaves);
   free(snap.buf);
   return rc;
}

int db2_witness_checkpoint_anchor_coverage(const uint8_t *key_id, size_t key_id_len,
                                           int64_t *out_unknown, char *sample, size_t sample_cap)
{
   if (sample && sample_cap)
      sample[0] = '\0';
   if (out_unknown)
      *out_unknown = 0;
   if (!key_id || key_id_len != VAULT_WITNESS_SIGNER_KEY_ID_LEN || !out_unknown)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[CP_ERR];
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        /* No min(bytea) in PostgreSQL, so pick a
                         * sample via array_agg rather than an
                         * aggregate over the bytea itself. */
                        "SELECT count(*), "
                        "encode((array_agg(signer_key_id ORDER BY seq))[1],'hex') "
                        "FROM kb_vault_witness_checkpoint WHERE signer_key_id <> ?1",
                        err, sizeof err);
   if (!st)
      return -1;
   if (aimee_pg_bind_blob(st, "?1", key_id, (int)key_id_len) != 0)
   {
      aimee_pg_finalize(st);
      return -1;
   }
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof err) == AIMEE_PG_ROW)
   {
      *out_unknown = aimee_pg_column_int64(st, 0);
      const char *hex = aimee_pg_column_text(st, 1);
      if (sample && sample_cap && hex)
         snprintf(sample, sample_cap, "%s", hex);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_witness_checkpoint_verify_run(int limit, db2_witness_verify_report_t *out)
{
   if (!out || limit <= 0)
      return -1;
   memset(out, 0, sizeof *out);
   void *conn = db2_conn();
   if (!conn)
      return -1;

   uint8_t pub[VAULT_WITNESS_ED25519_PUB_LEN], key_id[VAULT_WITNESS_SIGNER_KEY_ID_LEN];
   if (db2_vault_witness_signer_identity(pub, key_id) != 0)
      return -1;
   vault_witness_anchor_t anchor;
   memset(&anchor, 0, sizeof anchor);
   memcpy(anchor.key_id, key_id, sizeof key_id);
   memcpy(anchor.ed25519_pub, pub, sizeof pub);

   char err[CP_ERR];
   /* Ascending within the newest `limit`, so continuity is checked in chain order.
    * The oldest checkpoint in the window legitimately has a predecessor outside it;
    * continuity therefore starts from the second row in the window. */
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT seq,root,has_predecessor,predecessor_digest,shard_count,leaf_snapshot_digest,"
       "signer_key_id,sig_alg,sig_version,signature,created_at FROM ("
       "SELECT * FROM kb_vault_witness_checkpoint ORDER BY seq DESC LIMIT ?1) w ORDER BY seq",
       err, sizeof err);
   if (!st)
      return -1;
   if (aimee_pg_bind_int64(st, "?1", limit) != 0)
   {
      aimee_pg_finalize(st);
      return -1;
   }

   vault_witness_checkpoint_t *cps = NULL;
   size_t n = 0, cap = 0;
   int rc = -1;
   aimee_pg_step_t sr;
   while ((sr = aimee_pg_step(st, err, sizeof err)) == AIMEE_PG_ROW)
   {
      if (n == cap)
      {
         size_t ncap = cap ? cap * 2 : 64;
         vault_witness_checkpoint_t *nc = realloc(cps, ncap * sizeof *nc);
         if (!nc)
            goto done;
         cps = nc;
         cap = ncap;
      }
      vault_witness_checkpoint_t *cp = &cps[n];
      memset(cp, 0, sizeof *cp);
      cp->version = 1;
      cp->seq = (uint64_t)aimee_pg_column_int64(st, 0);
      /* Every bytea copied out before the next is read (single blob cache). */
      const void *b = aimee_pg_column_blob(st, 1);
      if (!b || aimee_pg_column_bytes(st, 1) != 32)
         goto done;
      memcpy(cp->root, b, 32);
      const char *hp = aimee_pg_column_text(st, 2);
      cp->has_predecessor = (hp && hp[0] == 't');
      b = aimee_pg_column_blob(st, 3);
      if (!b || aimee_pg_column_bytes(st, 3) != 32)
         goto done;
      memcpy(cp->predecessor_digest, b, 32);
      cp->shard_count = (uint64_t)aimee_pg_column_int64(st, 4);
      b = aimee_pg_column_blob(st, 5);
      if (!b || aimee_pg_column_bytes(st, 5) != 32)
         goto done;
      memcpy(cp->leaf_snapshot_digest, b, 32);
      b = aimee_pg_column_blob(st, 6);
      if (!b || aimee_pg_column_bytes(st, 6) != VAULT_WITNESS_SIGNER_KEY_ID_LEN)
         goto done;
      memcpy(cp->signer_key_id, b, VAULT_WITNESS_SIGNER_KEY_ID_LEN);
      cp->sig_alg = (uint16_t)aimee_pg_column_int64(st, 7);
      cp->sig_version = (uint16_t)aimee_pg_column_int64(st, 8);
      b = aimee_pg_column_blob(st, 9);
      if (!b || aimee_pg_column_bytes(st, 9) != 64)
         goto done;
      memcpy(cp->signature, b, 64);
      snprintf(cp->created_at, sizeof cp->created_at, "%s", aimee_pg_column_text(st, 10));
      n++;
   }
   if (sr == AIMEE_PG_ERR)
      goto done;

   for (size_t i = 0; i < n; i++)
   {
      switch (db2_vault_witness_checkpoint_verify(&cps[i], &anchor, 1))
      {
      case VAULT_WITNESS_CP_OK:
         break;
      case VAULT_WITNESS_CP_UNKNOWN_KEY:
      case VAULT_WITNESS_CP_REVOKED_KEY:
         out->unknown_key++;
         break;
      default:
         out->bad_signature++;
         break;
      }
      out->checked++;
   }
   if (n > 1)
   {
      size_t gap = 0;
      switch (db2_vault_witness_verify_checkpoint_run(cps, n, &gap))
      {
      case VAULT_WITNESS_CONTINUITY_OK:
         break;
      case VAULT_WITNESS_CONTINUITY_UNPROVEN:
         out->continuity_unproven = 1;
         break;
      default:
         out->continuity_broken = 1;
         break;
      }
   }
   rc = 0;

done:
   aimee_pg_finalize(st);
   free(cps);
   OPENSSL_cleanse(pub, sizeof pub);
   OPENSSL_cleanse(key_id, sizeof key_id);
   return rc;
}

int db2_witness_checkpoint_freshness(int64_t *out_count, int64_t *out_age_seconds)
{
   if (out_count)
      *out_count = 0;
   if (out_age_seconds)
      *out_age_seconds = 0;
   if (!out_count || !out_age_seconds)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[CP_ERR];
   /* One scan: the count, and the age of the newest checkpoint. When empty the age
    * column is NULL, reported as 0 with count 0 — the caller reads count first. */
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT count(*), "
       "COALESCE(EXTRACT(EPOCH FROM (CURRENT_TIMESTAMP - MAX(created_at)::timestamp))::bigint,0) "
       "FROM kb_vault_witness_checkpoint",
       err, sizeof err);
   if (!st)
      return -1;
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof err) == AIMEE_PG_ROW)
   {
      *out_count = aimee_pg_column_int64(st, 0);
      *out_age_seconds = aimee_pg_column_int64(st, 1);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}
