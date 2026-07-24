/* kb_curator_version.h: version-bump replay for the curator pipeline.
 *
 * Charter versioning: bumping the extraction prompt_version re-extracts every
 * document (the doc passes), without dropping vectors; bumping the embedding
 * model_version re-embeds every committed curator artifact, without
 * re-extracting. Baselines persist in kb_runtime_state; the first observation of
 * a version records a baseline and does not replay. No DB1 access. */
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
    * affected pass on a real bump (not the first observation). Returns 0. */
   int kb_curator_version_replay(const char *extract_prompt_version,
                                 const char *embed_model_version, kb_curator_version_replay_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_CURATOR_VERSION_H */
