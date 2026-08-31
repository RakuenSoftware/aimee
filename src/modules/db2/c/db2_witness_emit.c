#include "db2_witness_emit.h"
#include "db2_witness_checkpoint.h"
#include "db2_vault_witness_provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db2_internal.h" /* db2_conn */
#include "db_postgres.h"  /* aimee_pg_* */
#include "modules/vault/vault_witness_checkpoint.h"
#include "modules/vault/vault_witness_record.h"

#define EMIT_ERR        256
#define EMIT_MAX_SHARDS 4096
/* Rows per read. Bounded so each statement, each frame loop and each cursor
 * advance stays small; the caller's budget governs how many batches a run drains.
 * The SQL reader caps any limit at 1000, so this must not exceed it. */
#define EMIT_BATCH_ROWS 256

/* One pending stream position from org_vault_witness_emit_pending(). */
typedef struct
{
   int kind;
   char tenant[VAULT_WITNESS_TENANT_MAX + 1];
   char provider[VAULT_WITNESS_PROVIDER_MAX + 1];
   int64_t last_emitted;
   int64_t head_seq;
} pending_t;

static void put_u64(uint8_t *p, uint64_t v)
{
   for (unsigned i = 0; i < 8; i++)
      p[i] = (uint8_t)(v >> (56U - 8U * i));
}

/* Copy a fixed-width bytea out of the current row. The blob cache holds ONE blob
 * per statement and frees the previous on the next call, so every value must be
 * copied before the next blob column is read. */
static int copy_fixed(aimee_pg_stmt_t *st, int col, uint8_t *dst, int want)
{
   const void *b = aimee_pg_column_blob(st, col);
   if (!b || aimee_pg_column_bytes(st, col) != want)
      return -1;
   memcpy(dst, b, (size_t)want);
   return 0;
}

static void copy_text(char *dst, size_t cap, const char *src)
{
   snprintf(dst, cap, "%s", src ? src : "");
}

/* Advance a cursor. Failure here is not fatal to the run: the evidence is already
 * published, and a stale cursor only causes re-emission, which is tolerated. */
static void advance_cursor(void *conn, int kind, const char *tenant, const char *provider,
                           int64_t seq)
{
   char err[EMIT_ERR];
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT org_vault_witness_emit_advance(?1::smallint,?2,?3,?4)", err, sizeof err);
   if (!st)
      return;
   if (aimee_pg_bind_int64(st, "?1", kind) == 0 && aimee_pg_bind_text(st, "?2", tenant) == 0 &&
       aimee_pg_bind_text(st, "?3", provider) == 0 && aimee_pg_bind_int64(st, "?4", seq) == 0)
      (void)aimee_pg_step(st, err, sizeof err);
   aimee_pg_finalize(st);
}

/* Emit one batch of a shard's pending records, starting strictly after `after`.
 * Sets *out_last to the highest shard_seq actually accepted by the sink (or leaves
 * it at `after` if none) and *out_rows to the number of rows the reader returned,
 * which the caller uses to tell "drained" from "hit the batch limit". */
static db2_witness_emit_result_t emit_shard_batch(void *conn, const pending_t *p, int64_t after,
                                                  int limit, db2_witness_emit_sink_fn sink,
                                                  void *ctx, db2_witness_emit_stats_t *stats,
                                                  int64_t *out_last, size_t *out_rows)
{
   char err[EMIT_ERR];
   *out_last = after;
   *out_rows = 0;
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT shard_seq,source_kind,source_id,source_hash,has_source_pred,source_pred_hash,"
       "witness_pred_hash,record_hash,request_id,principal,provider_cred,group_id,event_ts,"
       "seal_epoch,fencing_token FROM org_vault_witness_emit_batch(?1,?2,?3,?4)",
       err, sizeof err);
   if (!st)
      return DB2_WITNESS_EMIT_ERROR;
   if (aimee_pg_bind_text(st, "?1", p->tenant) != 0 ||
       aimee_pg_bind_text(st, "?2", p->provider) != 0 ||
       aimee_pg_bind_int64(st, "?3", after) != 0 || aimee_pg_bind_int64(st, "?4", limit) != 0)
   {
      aimee_pg_finalize(st);
      return DB2_WITNESS_EMIT_ERROR;
   }

   db2_witness_emit_result_t rc = DB2_WITNESS_EMIT_OK;
   int64_t last_ok = after;
   aimee_pg_step_t sr;
   while ((sr = aimee_pg_step(st, err, sizeof err)) == AIMEE_PG_ROW)
   {
      (*out_rows)++;
      vault_witness_record_t r;
      memset(&r, 0, sizeof r);
      int64_t shard_seq = aimee_pg_column_int64(st, 0);
      int64_t src_kind = aimee_pg_column_int64(st, 1);
      if (shard_seq <= 0 || src_kind < 0 || src_kind > 2)
      {
         rc = DB2_WITNESS_EMIT_ERROR;
         break;
      }
      r.shard_seq = (uint64_t)shard_seq;
      r.source = (vault_witness_source_t)src_kind;
      r.is_first_in_shard = (shard_seq == 1);
      copy_text(r.tenant, sizeof r.tenant, p->tenant);
      copy_text(r.provider, sizeof r.provider, p->provider);
      copy_text(r.source_id, sizeof r.source_id, aimee_pg_column_text(st, 2));
      uint8_t stored_hash[32];
      const char *hsp = NULL;
      if (copy_fixed(st, 3, r.source_hash, 32) != 0)
      {
         rc = DB2_WITNESS_EMIT_ERROR;
         break;
      }
      hsp = aimee_pg_column_text(st, 4);
      r.has_source_pred = (hsp && hsp[0] == 't');
      if (copy_fixed(st, 5, r.source_pred_hash, 32) != 0 ||
          copy_fixed(st, 6, r.witness_pred_hash, 32) != 0 ||
          copy_fixed(st, 7, stored_hash, 32) != 0)
      {
         rc = DB2_WITNESS_EMIT_ERROR;
         break;
      }
      copy_text(r.request_id, sizeof r.request_id, aimee_pg_column_text(st, 8));
      copy_text(r.principal, sizeof r.principal, aimee_pg_column_text(st, 9));
      copy_text(r.provider_cred, sizeof r.provider_cred, aimee_pg_column_text(st, 10));
      copy_text(r.group_id, sizeof r.group_id, aimee_pg_column_text(st, 11));
      copy_text(r.timestamp, sizeof r.timestamp, aimee_pg_column_text(st, 12));
      r.seal_epoch = (uint64_t)aimee_pg_column_int64(st, 13);
      r.fencing_token = (uint64_t)aimee_pg_column_int64(st, 14);

      /* Parity: the re-encoded record must digest to exactly what E1 stored. This
       * is the check that makes an emitted copy worth comparing against; without it
       * a drifted encoder would publish well-formed evidence that can never match. */
      uint8_t recomputed[32];
      if (db2_vault_witness_record_digest(&r, recomputed) != 0 ||
          memcmp(recomputed, stored_hash, 32) != 0)
      {
         rc = DB2_WITNESS_EMIT_PARITY_MISMATCH;
         break;
      }

      uint8_t wire[VAULT_WITNESS_RECORD_MAX];
      size_t wlen = 0;
      if (db2_vault_witness_record_encode(&r, wire, sizeof wire, &wlen) != 0)
      {
         rc = DB2_WITNESS_EMIT_ERROR;
         break;
      }
      uint8_t frame[VAULT_WITNESS_EXPORT_HEADER_LEN + VAULT_WITNESS_RECORD_MAX];
      size_t flen = 0;
      if (db2_vault_witness_export_frame(VAULT_WITNESS_EXPORT_RECORD, wire, wlen, frame,
                                         sizeof frame, &flen) != 0)
      {
         rc = DB2_WITNESS_EMIT_ERROR;
         break;
      }
      if (sink(ctx, VAULT_WITNESS_EXPORT_RECORD, frame, flen) != 0)
      {
         stats->sink_failures++;
         rc = DB2_WITNESS_EMIT_SINK_FAILED;
         break;
      }
      stats->records_emitted++;
      last_ok = shard_seq;
   }
   if (sr == AIMEE_PG_ERR && rc == DB2_WITNESS_EMIT_OK)
      rc = DB2_WITNESS_EMIT_TRANSIENT;
   aimee_pg_finalize(st);

   *out_last = last_ok;
   if (last_ok > after)
      advance_cursor(conn, 0, p->tenant, p->provider, last_ok);
   return rc;
}

/* Drain one shard, batch by batch, until it is empty or the per-run budget is
 * spent. Draining in batches rather than one huge query keeps each statement and
 * each cursor advance bounded, so a kill mid-drain loses at most one batch of
 * progress (and loses no evidence — the cursor simply lags and the next tick
 * re-reads). The budget is what stops a large backlog from monopolising the
 * periodic loop; when it bites, the shard stays behind and the backlog gauge shows
 * it rather than the drain silently appearing complete. */
static db2_witness_emit_result_t emit_shard(void *conn, const pending_t *p, int batch,
                                            uint64_t budget, db2_witness_emit_sink_fn sink,
                                            void *ctx, db2_witness_emit_stats_t *stats)
{
   int64_t cursor = p->last_emitted;
   uint64_t spent = 0;
   for (;;)
   {
      int64_t last = cursor;
      size_t rows = 0;
      db2_witness_emit_result_t rc =
          emit_shard_batch(conn, p, cursor, batch, sink, ctx, stats, &last, &rows);
      if (rc != DB2_WITNESS_EMIT_OK)
         return rc;
      if (last <= cursor)
         return DB2_WITNESS_EMIT_OK; /* nothing accepted: drained, or sink took none */
      cursor = last;
      spent += rows;
      /* A short batch means the shard is drained; a full one means there may be
       * more, so keep going until the budget is spent. */
      if (rows < (size_t)batch || spent >= budget)
         return DB2_WITNESS_EMIT_OK;
   }
}

/* Emit pending checkpoints, each immediately followed by its leaf snapshot. */
static db2_witness_emit_result_t emit_checkpoints(void *conn, int64_t after, int limit,
                                                  db2_witness_emit_sink_fn sink, void *ctx,
                                                  db2_witness_emit_stats_t *stats)
{
   char err[EMIT_ERR];
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT seq,root,has_predecessor,predecessor_digest,shard_count,leaf_snapshot_digest,"
       "signer_key_id,sig_alg,sig_version,signature,created_at,leaf_snapshot "
       "FROM org_vault_witness_emit_checkpoints(?1,?2)",
       err, sizeof err);
   if (!st)
      return DB2_WITNESS_EMIT_ERROR;
   if (aimee_pg_bind_int64(st, "?1", after) != 0 || aimee_pg_bind_int64(st, "?2", limit) != 0)
   {
      aimee_pg_finalize(st);
      return DB2_WITNESS_EMIT_ERROR;
   }

   db2_witness_emit_result_t rc = DB2_WITNESS_EMIT_OK;
   int64_t last_ok = after;
   uint8_t *snap = NULL;
   aimee_pg_step_t sr;
   while ((sr = aimee_pg_step(st, err, sizeof err)) == AIMEE_PG_ROW)
   {
      vault_witness_checkpoint_t cp;
      memset(&cp, 0, sizeof cp);
      cp.version = 1;
      int64_t seq = aimee_pg_column_int64(st, 0);
      if (seq <= 0)
      {
         rc = DB2_WITNESS_EMIT_ERROR;
         break;
      }
      cp.seq = (uint64_t)seq;
      if (copy_fixed(st, 1, cp.root, 32) != 0)
      {
         rc = DB2_WITNESS_EMIT_ERROR;
         break;
      }
      const char *hp = aimee_pg_column_text(st, 2);
      cp.has_predecessor = (hp && hp[0] == 't');
      if (copy_fixed(st, 3, cp.predecessor_digest, 32) != 0)
      {
         rc = DB2_WITNESS_EMIT_ERROR;
         break;
      }
      cp.shard_count = (uint64_t)aimee_pg_column_int64(st, 4);
      if (copy_fixed(st, 5, cp.leaf_snapshot_digest, 32) != 0 ||
          copy_fixed(st, 6, cp.signer_key_id, VAULT_WITNESS_SIGNER_KEY_ID_LEN) != 0)
      {
         rc = DB2_WITNESS_EMIT_ERROR;
         break;
      }
      cp.sig_alg = (uint16_t)aimee_pg_column_int64(st, 7);
      cp.sig_version = (uint16_t)aimee_pg_column_int64(st, 8);
      if (copy_fixed(st, 9, cp.signature, 64) != 0)
      {
         rc = DB2_WITNESS_EMIT_ERROR;
         break;
      }
      copy_text(cp.created_at, sizeof cp.created_at, aimee_pg_column_text(st, 10));
      /* Snapshot bytes last, and copied out immediately for the same blob-cache
       * reason; it is variable-length so it needs its own allocation. */
      const void *sb = aimee_pg_column_blob(st, 11);
      int sblen = aimee_pg_column_bytes(st, 11);
      if (sblen < 0 || (sblen > 0 && !sb))
      {
         rc = DB2_WITNESS_EMIT_ERROR;
         break;
      }
      free(snap);
      snap = malloc((size_t)sblen ? (size_t)sblen : 1);
      if (!snap)
      {
         rc = DB2_WITNESS_EMIT_ERROR;
         break;
      }
      if (sblen > 0)
         memcpy(snap, sb, (size_t)sblen);

      uint8_t wire[VAULT_WITNESS_CHECKPOINT_WIRE_MAX];
      size_t wlen = 0;
      if (db2_vault_witness_checkpoint_encode(&cp, wire, sizeof wire, &wlen) != 0)
      {
         rc = DB2_WITNESS_EMIT_ERROR;
         break;
      }
      uint8_t frame[VAULT_WITNESS_EXPORT_HEADER_LEN + VAULT_WITNESS_CHECKPOINT_WIRE_MAX];
      size_t flen = 0;
      if (db2_vault_witness_export_frame(VAULT_WITNESS_EXPORT_CHECKPOINT, wire, wlen, frame,
                                         sizeof frame, &flen) != 0)
      {
         rc = DB2_WITNESS_EMIT_ERROR;
         break;
      }
      if (sink(ctx, VAULT_WITNESS_EXPORT_CHECKPOINT, frame, flen) != 0)
      {
         stats->sink_failures++;
         rc = DB2_WITNESS_EMIT_SINK_FAILED;
         break;
      }
      stats->checkpoints_emitted++;

      /* Snapshot payload: u64 checkpoint_seq || stored snapshot bytes. */
      size_t plen = 8 + (size_t)sblen;
      uint8_t *sframe = malloc(VAULT_WITNESS_EXPORT_HEADER_LEN + plen);
      uint8_t *payload = malloc(plen);
      if (!sframe || !payload)
      {
         free(sframe);
         free(payload);
         rc = DB2_WITNESS_EMIT_ERROR;
         break;
      }
      put_u64(payload, cp.seq);
      if (sblen > 0)
         memcpy(payload + 8, snap, (size_t)sblen);
      size_t sflen = 0;
      int fr = db2_vault_witness_export_frame(VAULT_WITNESS_EXPORT_SNAPSHOT, payload, plen, sframe,
                                              VAULT_WITNESS_EXPORT_HEADER_LEN + plen, &sflen);
      free(payload);
      if (fr != 0)
      {
         free(sframe);
         rc = DB2_WITNESS_EMIT_ERROR;
         break;
      }
      int sunk = sink(ctx, VAULT_WITNESS_EXPORT_SNAPSHOT, sframe, sflen);
      free(sframe);
      if (sunk != 0)
      {
         /* The checkpoint went out but its snapshot did not. Do NOT advance past
          * this seq: the next run re-emits both. The offline verifier collapses the
          * byte-identical duplicate checkpoint (vault_witness_offline_verify dedupes
          * same-seq checkpoints exactly as it dedupes records), so the retry is not
          * mistaken for a fork. Losing the snapshot silently would leave the
          * cross-gap leaf comparison without its inputs. */
         stats->sink_failures++;
         rc = DB2_WITNESS_EMIT_SINK_FAILED;
         break;
      }
      stats->snapshots_emitted++;
      last_ok = seq;
   }
   if (sr == AIMEE_PG_ERR && rc == DB2_WITNESS_EMIT_OK)
      rc = DB2_WITNESS_EMIT_TRANSIENT;
   aimee_pg_finalize(st);
   free(snap);

   if (last_ok > after)
      advance_cursor(conn, 1, "", "", last_ok);
   return rc;
}

db2_witness_emit_result_t db2_witness_emit_run(db2_witness_emit_sink_fn sink, void *ctx,
                                               int max_per_stream, db2_witness_emit_stats_t *out)
{
   db2_witness_emit_stats_t stats;
   memset(&stats, 0, sizeof stats);
   if (out)
      memset(out, 0, sizeof *out);
   if (!sink || max_per_stream <= 0)
      return DB2_WITNESS_EMIT_ERROR;

   void *conn = db2_conn();
   if (!conn)
      return DB2_WITNESS_EMIT_TRANSIENT;

   /* Snapshot the pending set first and finalize the statement, so the per-stream
    * emit queries below are not nested inside an open cursor. */
   char err[EMIT_ERR];
   pending_t *pend = NULL;
   size_t n = 0, cap = 0;
   int64_t cp_after = 0, cp_head = 0;
   int have_cp = 0;
   {
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                             "SELECT kind,tenant,provider,last_emitted,head_seq "
                                             "FROM org_vault_witness_emit_pending()",
                                             err, sizeof err);
      if (!st)
         return DB2_WITNESS_EMIT_ERROR;
      aimee_pg_step_t sr;
      int bad = 0;
      while ((sr = aimee_pg_step(st, err, sizeof err)) == AIMEE_PG_ROW)
      {
         int kind = (int)aimee_pg_column_int64(st, 0);
         int64_t last = aimee_pg_column_int64(st, 3);
         int64_t head = aimee_pg_column_int64(st, 4);
         if (kind == 1)
         {
            cp_after = last;
            cp_head = head;
            have_cp = 1;
            continue;
         }
         if (kind != 0 || n >= EMIT_MAX_SHARDS)
         {
            bad = 1;
            break;
         }
         if (n == cap)
         {
            size_t ncap = cap ? cap * 2 : 16;
            pending_t *np = realloc(pend, ncap * sizeof *np);
            if (!np)
            {
               bad = 1;
               break;
            }
            pend = np;
            cap = ncap;
         }
         memset(&pend[n], 0, sizeof pend[n]);
         pend[n].kind = 0;
         copy_text(pend[n].tenant, sizeof pend[n].tenant, aimee_pg_column_text(st, 1));
         copy_text(pend[n].provider, sizeof pend[n].provider, aimee_pg_column_text(st, 2));
         pend[n].last_emitted = last;
         pend[n].head_seq = head;
         n++;
      }
      if (sr == AIMEE_PG_ERR)
         bad = 1;
      aimee_pg_finalize(st);
      if (bad)
      {
         free(pend);
         return DB2_WITNESS_EMIT_ERROR;
      }
   }

   db2_witness_emit_result_t rc = DB2_WITNESS_EMIT_OK;
   for (size_t i = 0; i < n && rc == DB2_WITNESS_EMIT_OK; i++)
   {
      if (pend[i].head_seq <= pend[i].last_emitted)
         continue;
      rc = emit_shard(conn, &pend[i], EMIT_BATCH_ROWS, (uint64_t)max_per_stream, sink, ctx, &stats);
   }
   /* Checkpoints arrive at cadence (roughly one per tick), so a single batch keeps
    * up in steady state. After a long outage the stream drains over several ticks
    * instead of one; the checkpoint backlog gauge shows that rather than it being
    * invisible. */
   if (rc == DB2_WITNESS_EMIT_OK && have_cp && cp_head > cp_after)
      rc = emit_checkpoints(conn, cp_after,
                            max_per_stream < EMIT_BATCH_ROWS ? max_per_stream : EMIT_BATCH_ROWS,
                            sink, ctx, &stats);

   /* Backlog is reported from the pre-run positions minus what this run emitted, so
    * it reflects what is still outstanding without a second round trip. */
   for (size_t i = 0; i < n; i++)
      if (pend[i].head_seq > pend[i].last_emitted)
         stats.backlog_records += (uint64_t)(pend[i].head_seq - pend[i].last_emitted);
   stats.backlog_records -= (stats.backlog_records < stats.records_emitted) ? stats.backlog_records
                                                                            : stats.records_emitted;
   if (have_cp && cp_head > cp_after)
      stats.backlog_checkpoints = (uint64_t)(cp_head - cp_after);
   stats.backlog_checkpoints -= (stats.backlog_checkpoints < stats.checkpoints_emitted)
                                    ? stats.backlog_checkpoints
                                    : stats.checkpoints_emitted;

   free(pend);
   if (out)
      *out = stats;
   return rc;
}
