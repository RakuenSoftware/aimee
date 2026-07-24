/* kb_curator_version.c: version-bump replay for the curator pipeline.
 * Mirrors kb_learning_version.c. No DB1 access. */

#include "kb_curator_version.h"

#include "aimee.h"
#include "db2/artifacts.h"
#include "db2/kb_payload.h"
#include "db2/kb_runtime_state.h"
#include "log.h"

#include <string.h>

#define CV_PROMPT_KEY "curator_extract_prompt_version"
#define CV_MODEL_KEY  "curator_embed_model_version"

/* Returns 1 if the persisted value under `key` differs from `current` (a real
 * bump, not the first-ever record). Always stores `current` as the new
 * baseline. */
static int version_changed(const char *key, const char *current)
{
   char prev[64] = "";
   int have = (db2_kb_runtime_state_get(key, prev, sizeof(prev)) == 0 && prev[0]);
   db2_kb_runtime_state_set(key, current);
   if (!have)
      return 0;
   return strcmp(prev, current) != 0;
}

int kb_curator_version_replay(const char *extract_prompt_version, const char *embed_model_version,
                              kb_curator_version_replay_t *out)
{
   kb_curator_version_replay_t r;
   memset(&r, 0, sizeof(r));

   /* prompt bump -> re-extract (re-arm extract_doc); vectors untouched. */
   if (extract_prompt_version && extract_prompt_version[0] &&
       version_changed(CV_PROMPT_KEY, extract_prompt_version))
   {
      r.prompt_bumped = 1;
      r.docs_reextracted = db2_curator_reenqueue_extract_all();
      aimee_log(LOG_INFO, "kb.curator.version",
                "extract prompt_version -> %s; re-extracting %d document(s)",
                extract_prompt_version, r.docs_reextracted);
   }

   /* model bump -> re-embed (committed -> proposed); no re-extraction. */
   if (embed_model_version && embed_model_version[0] &&
       version_changed(CV_MODEL_KEY, embed_model_version))
   {
      r.model_bumped = 1;
      r.artifacts_reembedded = db2_curator_reembed_all();
      aimee_log(LOG_INFO, "kb.curator.version",
                "embed model_version -> %s; re-embedding %d curator artifact(s)",
                embed_model_version, r.artifacts_reembedded);
   }

   if (out)
      *out = r;
   return 0;
}
