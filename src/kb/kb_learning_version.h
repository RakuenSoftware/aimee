/* kb_learning_version.h: version-bump replay for the learning pipeline.
 *
 * Persists the active embedding model_version and synthesis prompt_version in
 * kb runtime_state. On a version change it re-enqueues the affected work — a
 * model_version bump re-embeds the evidence layer (without re-running
 * synthesis); a prompt_version bump replays the candidate-generation pass
 * (without re-embedding) — so the queues the embed/synth drains already process
 * pick the work back up. KB_STORE only; no DB1 access.
 *
 * See docs/proposals/pending/cross-source-learning-pipeline.md */
#ifndef KB_LEARNING_VERSION_H
#define KB_LEARNING_VERSION_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int model_bumped;    /* 1 if the embedding model_version changed */
      int prompt_bumped;   /* 1 if the synthesis prompt_version changed */
      int embed_ops_reset; /* evidence ops re-enqueued for embedding */
      int synth_ops_reset; /* synth ops re-enqueued for candidate generation */
   } learning_version_replay_t;

   /* Compare the supplied versions against the persisted baseline and replay any
    * that changed. The first call on a fresh DB just records the baseline (no
    * replay). Pass NULL versions to leave that dimension untouched. `out` may be
    * NULL. Returns 0 on success, -1 on error (KB_STORE unavailable). */
   int learning_version_replay(const char *embed_model_version, const char *synth_prompt_version,
                               learning_version_replay_t *out);

#ifdef __cplusplus
}
#endif
#endif
