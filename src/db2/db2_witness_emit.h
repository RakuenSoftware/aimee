#ifndef AIMEE_DB2_WITNESS_EMIT_H
#define AIMEE_DB2_WITNESS_EMIT_H

#include <stddef.h>
#include <stdint.h>

#include "modules/vault/vault_witness_export.h"

/* P7-witness-e2: evidence emission on the log/OTLP path.
 *
 * Reads COMMITTED witness state only and hands framed evidence bytes to a sink.
 * The durable store on aimee-kb is the system of record; emission publishes copies
 * outward so a retained copy on another host can later be compared against the
 * local store. Emission is therefore best-effort by design:
 *
 *   - it never blocks admission and never runs inside an append transaction;
 *   - it drops nothing from the durable log on failure, it only stops advancing;
 *   - a lost or reset cursor causes re-emission, which the offline verifier
 *     collapses (byte-identical repeats at one shard_seq are duplicates, only two
 *     DIFFERENT records at one position are a fork).
 *
 * The one thing emission is NOT permitted to do is publish evidence that does not
 * match what is stored. Every record is re-encoded from its stored columns and its
 * canonical digest is compared against the stored record_hash before framing; a
 * mismatch aborts the run with DB2_WITNESS_EMIT_PARITY_MISMATCH and emits nothing
 * further, because at that point either the store is corrupt or the encoder has
 * drifted, and both make the emitted stream useless for comparison.
 */

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum
{
   DB2_WITNESS_EMIT_OK = 0,          /* run completed; see stats for what moved */
   DB2_WITNESS_EMIT_TRANSIENT,       /* no connection or a retryable failure */
   DB2_WITNESS_EMIT_PARITY_MISMATCH, /* stored row and canonical encoding disagree */
   DB2_WITNESS_EMIT_SINK_FAILED,     /* the sink rejected a frame; cursor not advanced past it */
   DB2_WITNESS_EMIT_ERROR            /* any other failure */
} db2_witness_emit_result_t;

typedef struct
{
   uint64_t records_emitted;
   uint64_t checkpoints_emitted;
   uint64_t snapshots_emitted;
   uint64_t backlog_records;     /* unemitted records still pending across all shards */
   uint64_t backlog_checkpoints; /* unemitted checkpoints still pending */
   uint64_t sink_failures;
} db2_witness_emit_stats_t;

/* Receives one framed evidence blob. Returns 0 on success, non-zero to signal the
 * frame was not accepted (the cursor then stops before it and the run reports
 * DB2_WITNESS_EMIT_SINK_FAILED). */
typedef int (*db2_witness_emit_sink_fn)(void *ctx, vault_witness_export_kind_t kind,
                                        const uint8_t *frame, size_t frame_len);

/* Drain each shard, and the checkpoint stream, in one run.
 *
 * `max_per_stream` is a per-stream budget, not a hard cap: records are read in
 * fixed-size batches and the budget is checked between batches, so a run can
 * overshoot it by up to one batch. It exists to bound the tail — a normal burst
 * clears in a single run while a pathological backlog cannot monopolise the
 * caller's tick — and is not a precise quota. When it bites, the shard stays
 * behind and the backlog gauge reports what is outstanding; a partial drain is
 * never reported as a complete one.
 *
 * `out` may be NULL. */
db2_witness_emit_result_t db2_witness_emit_run(db2_witness_emit_sink_fn sink, void *ctx,
                                               int max_per_stream,
                                               db2_witness_emit_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_DB2_WITNESS_EMIT_H */
