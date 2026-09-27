/* kb_learning_synth.h: cross-source learning candidate-generation pass.
 *
 * Builds an evidence neighbourhood for a seed query (learning_bundle), hands it
 * to the configured model sidecar (learning_synthesize_command), and writes the
 * returned candidates as proposed charter artifacts — each citing every
 * neighbourhood evidence artifact as a corroborating source, so the existing
 * judge -> promote machinery can act on them unchanged.
 *
 * Heavy work (the LLM call) runs here on the kb scheduler, never on the
 * interactive capture hot path. KB_STORE only; no DB1 access.
 *
 * See docs/proposals/pending/cross-source-learning-pipeline.md */
#ifndef KB_LEARNING_SYNTH_H
#define KB_LEARNING_SYNTH_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Generate candidates from the neighbourhood around `query`.
    *   synth_cmd:   sidecar command (required; NULL/"" -> error).
    *   embed_cmd:   embedding command for the bundle ("builtin"/NULL ok).
    *   k:           neighbourhood size.
    *   max_tokens:  per-call token budget passed to the sidecar.
    *   scope_kind/scope_id/operator_id: scope stamped on the proposed candidates.
    *   proposed_ids/max_ids: optional out-array of written candidate ids (each
    *                 >= 37 bytes); may be NULL.
    * Returns the number of candidates written (>= 0), or -1 on error (no
    * sidecar, embedder/DB failure, or sidecar failure). An empty neighbourhood
    * is a valid result of 0. */
   int kb_learning_synth_generate(const char *query, const char *synth_cmd, const char *embed_cmd,
                                  int k, int max_tokens, const char *scope_kind,
                                  const char *scope_id, const char *operator_id,
                                  char proposed_ids[][37], int max_ids);

   /* Drain up to `max` pending learning_synth_ops: for each queued evidence
    * artifact, build the neighbourhood around its own content and synthesise
    * candidates, then mark the op done (or failed). Each op's scope is taken
    * from its evidence artifact. Returns the number of ops processed (>= 0).
    * This is the scheduler entry point — it does the heavy LLM work off the
    * interactive capture hot path. */
   int kb_learning_synth_drain(int max, const char *synth_cmd, const char *embed_cmd, int k,
                               int max_tokens);

#ifdef __cplusplus
}
#endif
#endif
