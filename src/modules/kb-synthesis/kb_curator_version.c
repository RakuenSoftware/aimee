/* kb_curator_version.c: version-bump replay for the curator pipeline.
 * Mirrors kb_learning_version.c. No DB1 access. */

#include "kb_curator_version.h"

#include "aimee.h"
#include "db2/artifacts.h"
#include "db2/kb_payload.h"
#include "db2/kb_runtime_state.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

/* NEW KEY NAMES, and the rename is the migration.
 *
 * These used to be curator_extract_prompt_version and curator_embed_model_version, and
 * they held a hand-maintained label alone. They now hold a label composed with the
 * identity of the model actually in use, so the same key would carry a value that means
 * something different. Reusing it would make the first boot after this change look like
 * a bump on every existing deployment: a full LLM re-extraction of every document, and a
 * re-embed of every artifact, for no reason.
 *
 * A new key has no baseline, and version_changed() treats a first observation as "record
 * and do not replay". So the upgrade boot records silently and the next REAL change
 * fires. That is the whole reason these are not the old names. */
#define CV_EXTRACT_KEY "curator_extract_identity"
#define CV_EMBED_KEY   "curator_embed_identity"

/* Returns 1 if the persisted value under `key` differs from `current` (a real
 * bump, not the first-ever record). Always stores `current` as the new
 * baseline. */
static int version_changed(const char *key, const char *current)
{
   char prev[256] = "";
   int have = (db2_kb_runtime_state_get(key, prev, sizeof(prev)) == 0 && prev[0]);
   db2_kb_runtime_state_set(key, current);
   if (!have)
      return 0;
   return strcmp(prev, current) != 0;
}

/* Compose a declared label with the identity of the model actually serving.
 *
 * WHY BOTH. The label is an operator's deliberate "replay this pass now", and it has to
 * keep working: it is how a prompt edit that no identity can see gets replayed. The
 * identity is what the label cannot express, because nobody remembers to bump a version
 * string when they pick a different model in the wizard -- and that omission is silent,
 * which is the failure this addresses.
 *
 * An empty component contributes nothing but the separator, so a deployment that sets
 * neither keeps a stable value and never replays. */
static void compose_identity(char *out, size_t out_len, const char *label, const char *identity,
                             const char *identity2)
{
   snprintf(out, out_len, "%s|%s|%s", label ? label : "", identity ? identity : "",
            identity2 ? identity2 : "");
}

int kb_curator_version_replay(const char *extract_prompt_version, const char *embed_model_version,
                              const char *synthesis_model, const char *embedder_serving_id,
                              kb_curator_version_replay_t *out)
{
   kb_curator_version_replay_t r;
   memset(&r, 0, sizeof(r));

   /* Extraction: a prompt bump, a different synthesis model, OR a different embedder
    * vector space -> re-extract (re-arm extract_doc).
    *
    * The synthesis model belongs here because extraction output IS that model's output.
    * Swapping the model leaves every existing summary, claim and open question as the
    * previous model's work, indistinguishable from the new model's in the store and
    * cited as though current. Re-extracting is what "drop the old synthesis model and
    * re-synthesise" means in terms of this pipeline.
    *
    * THE EMBEDDER BELONGS HERE TOO, which is less obvious: extraction is an LLM pass, so
    * why should a vector space matter to it? Because what the pass reads is retrieved,
    * and retrieval is that vector space. Artifacts synthesised over neighbours the old
    * embedder selected are conclusions drawn from a different set of inputs than the same
    * query returns now. Re-embedding them preserves the old conclusions at new
    * coordinates, which is the worst of both. This is the operator-facing contract for an
    * embedder switch: drop, re-index, re-embed AND re-synthesise. */
   char extract_identity[256];
   compose_identity(extract_identity, sizeof(extract_identity), extract_prompt_version,
                    synthesis_model, embedder_serving_id);
   if (version_changed(CV_EXTRACT_KEY, extract_identity))
   {
      r.prompt_bumped = 1;
      r.docs_reextracted = db2_curator_reenqueue_extract_all();
      aimee_log(LOG_INFO, "kb.curator.version",
                "extraction identity -> %s; re-extracting %d document(s)", extract_identity,
                r.docs_reextracted);
   }

   /* Embedding: a model-version bump OR a different embedder vector space -> re-embed
    * (committed -> proposed); no re-extraction.
    *
    * The serving identity, not the embedder's NAME or its width: pooling and prefix
    * changes keep both and still produce a different space. This is the same identity
    * db2's drift guard records against the corpus, so the two agree on what a change is
    * rather than each deciding for itself.
    *
    * NOT keyed on the synthesis model: a different summariser produces different text,
    * and that text needs embedding, but re-extraction already leaves those artifacts
    * proposed for the index passes to pick up. Adding it here would re-embed every
    * artifact a second time for the same change. */
   char embed_identity[256];
   compose_identity(embed_identity, sizeof(embed_identity), embed_model_version,
                    embedder_serving_id, NULL);
   if (version_changed(CV_EMBED_KEY, embed_identity))
   {
      r.model_bumped = 1;
      r.artifacts_reembedded = db2_curator_reembed_all();
      aimee_log(LOG_INFO, "kb.curator.version",
                "embedding identity -> %s; re-embedding %d curator artifact(s)", embed_identity,
                r.artifacts_reembedded);
   }

   if (out)
      *out = r;
   return 0;
}
