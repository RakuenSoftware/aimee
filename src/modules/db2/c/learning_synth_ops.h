/* learning_synth_ops.h: the candidate-generation work queue.
 *
 * One row per captured evidence artifact awaiting a synthesis pass. Enqueued at
 * capture (off the interactive hot path), drained on the kb scheduler by the
 * candidate-generation worker. Mirrors the evidence embed-ops queue. */
#ifndef DEC_DB2_LEARNING_SYNTH_OPS_H
#define DEC_DB2_LEARNING_SYNTH_OPS_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Enqueue an evidence artifact for synthesis (idempotent — ON CONFLICT DO
    * NOTHING; status defaults to 'pending'). Returns 0 on success, -1 error. */
   int db2_synth_enqueue(const char *artifact_id);

   /* List up to `max` pending ops (status='pending'), ordered by artifact_id.
    * Writes artifact ids into out[][37]. Returns count (>=0), or -1 on error. */
   int db2_synth_list_pending(char out[][37], int max);

   /* Mark an op done (status='ok'). Returns 0/-1. */
   int db2_synth_mark_done(const char *artifact_id);

   /* Mark an op failed (status='failed', attempts+1, last_error). Returns 0/-1. */
   int db2_synth_mark_failed(const char *artifact_id, const char *error);

   /* Count ops with the given status (NULL = all). Returns count (>=0), or -1. */
   int db2_synth_ops_count(const char *status);

   /* Replay synthesis over the whole evidence layer: reset every op back to
    * 'pending' so the candidate-generation pass re-runs (used on a synthesis
    * prompt_version bump). Returns the number of ops reset (>=0). */
   int db2_synth_reenqueue_all(void);

#ifdef __cplusplus
}
#endif
#endif
