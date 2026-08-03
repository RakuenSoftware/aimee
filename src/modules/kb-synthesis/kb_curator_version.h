/* kb_curator_version.h: version-bump replay for the curator pipeline.
 *
 * Charter versioning: bumping the extraction prompt_version re-extracts every
 * document (the doc passes), without dropping vectors; bumping the embedding
 * model_version re-embeds every committed curator artifact, without
 * re-extracting. Baselines persist in kb_runtime_state; the first observation of
 * a version records a baseline and does not replay. No DB1 access.
 *
 * The declared versions are not the only trigger. Each pass also replays when the model
 * it depends on changes -- the synthesis model for extraction, the embedder's serving
 * identity for embedding -- because those changes are the ones nobody remembers to
 * declare. A user picking a different synthesis model in the wizard does not bump a
 * prompt version, and without this the old model's summaries and claims stayed in the
 * store, cited as current. */
#ifndef DEC_KB_CURATOR_VERSION_H
#define DEC_KB_CURATOR_VERSION_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int prompt_bumped;
      int model_bumped;
      int docs_reextracted;     /* extract_doc jobs re-armed on a prompt bump */
      int artifacts_reembedded; /* curator artifacts re-queued on a model bump */
   } kb_curator_version_replay_t;

   /* Compare the given versions against the persisted baselines and replay the
    * affected pass on a real bump (not the first observation). Returns 0.
    *
    * |synthesis_model| and |embedder_serving_id| are the identities of what is actually
    * serving, folded into the compared value alongside the declared label. Pass empty or
    * NULL for either to key that pass on its label alone. */
   int kb_curator_version_replay(const char *extract_prompt_version,
                                 const char *embed_model_version, const char *synthesis_model,
                                 const char *embedder_serving_id,
                                 kb_curator_version_replay_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_CURATOR_VERSION_H */
