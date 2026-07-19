/* learning_bundle.h: cross-source evidence neighbourhood builder.
 *
 * Given a query, embed it (via the configured embedding_command, builtin by
 * default), scan the stored evidence vectors (evidence_vectors, filled by the
 * kb_evidence_embed worker), rank by cosine similarity, and return the top-K
 * most-similar evidence artifacts. Because evidence artifacts carry their
 * source kind, a single bundle naturally spans multiple kinds — this is the
 * "retrieve from >= 3 kinds at once" neighbourhood the cross-source learning
 * pipeline judges over.
 *
 * The ranking is a C-side brute-force scan over the stored vectors (kept simple
 * and unit-testable against the db2 sqlite shim). At production scale the same
 * contract can be served by a pgvector-native top-K (ORDER BY embedding <=> q)
 * behind db2_evidence_vectors_list without changing this interface.
 *
 * See docs/proposals/pending/cross-source-learning-pipeline.md */
#ifndef DEC_LEARNING_BUNDLE_H
#define DEC_LEARNING_BUNDLE_H 1

#ifdef __cplusplus
extern "C"
{
#endif

#define LEARNING_BUNDLE_MAX 32

   typedef struct
   {
      char artifact_id[37];
      char kind[64];
      char collection[64];
      double score; /* cosine similarity to the query vector, [-1, 1] */
   } learning_bundle_item_t;

   typedef struct
   {
      learning_bundle_item_t items[LEARNING_BUNDLE_MAX];
      int count;          /* items returned (<= k) */
      int distinct_kinds; /* number of distinct kinds among the returned items */
      int scanned;        /* total stored vectors scanned */
   } learning_bundle_t;

   /* Build a cross-kind neighbourhood for `query`. `embed_cmd` is the embedding
    * command ("builtin"/NULL = hash embedder). `k` is the number of neighbours
    * to return (clamped to [1, LEARNING_BUNDLE_MAX]). Fills *out (sorted by
    * descending score). Returns 0 on success (including an empty corpus, count
    * 0), -1 on error (embedder failed or DB2 unavailable). */
   int learning_bundle_build(const char *query, const char *embed_cmd, int k,
                             learning_bundle_t *out);

#ifdef __cplusplus
}
#endif
#endif
