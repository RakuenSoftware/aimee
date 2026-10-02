/* kb_evidence_embed.h: evidence-vector embed worker.
 *
 * Drains the evidence_index_ops queue (populated by
 * learning_evidence_write_event), embeds each evidence artifact's content via
 * the configured embedding_command (or the builtin embedder), and stores the
 * 384-dim vector into evidence_vectors. KB_STORE only; no DB1 access. */
#ifndef KB_EVIDENCE_EMBED_H
#define KB_EVIDENCE_EMBED_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Claim and process exactly one pending evidence op.
    *   embed_cmd: embedding_command to run ("builtin"/NULL = hash embedder).
    * Returns 1 if an op was processed (stored OR marked failed), 0 if none were
    * pending, -1 on a hard DB error. */
   int kb_evidence_embed_one(const char *embed_cmd);

   /* Drain up to `max` pending evidence ops (<=0 selects a default batch).
    * Returns the number of ops processed (>= 0). Stops early once the queue is
    * empty or a hard DB error is hit. */
   int kb_evidence_embed_drain(int max, const char *embed_cmd);

#ifdef __cplusplus
}
#endif
#endif
