/* kb_learning_version.c: version-bump replay for the learning pipeline. */

#include "kb_learning_version.h"

#include "aimee.h"
#include "modules/db2/c/evidence_vectors.h"
#include "modules/db2/c/kb_runtime_state.h"
#include "modules/db2/c/learning_synth_ops.h"
#include "log.h"

#include <string.h>

#define LV_EMBED_KEY  "learning_embed_model_version"
#define LV_PROMPT_KEY "learning_synth_prompt_version"

/* Returns 1 if the persisted value under `key` differs from `current` and a
 * replay should fire (i.e. a real bump, not the first-ever record). Always
 * stores `current` as the new baseline. `first` is set when there was no prior
 * value (fresh DB — record baseline, do not replay). */
static int version_changed(const char *key, const char *current, int *first)
{
   char prev[64] = "";
   int have = (db2_kb_runtime_state_get(key, prev, sizeof(prev)) == 0 && prev[0]);
   *first = !have;
   db2_kb_runtime_state_set(key, current);
   if (!have)
      return 0;
   return strcmp(prev, current) != 0;
}

int learning_version_replay(const char *embed_model_version, const char *synth_prompt_version,
                            learning_version_replay_t *out)
{
   learning_version_replay_t r;
   memset(&r, 0, sizeof(r));

   if (embed_model_version && embed_model_version[0])
   {
      int first = 0;
      if (version_changed(LV_EMBED_KEY, embed_model_version, &first))
      {
         r.model_bumped = 1;
         r.embed_ops_reset = db2_evidence_reembed_all();
         aimee_log(LOG_INFO, "kb.learning.version",
                   "embedding model_version -> %s; re-embedding %d evidence op(s)",
                   embed_model_version, r.embed_ops_reset);
      }
   }

   if (synth_prompt_version && synth_prompt_version[0])
   {
      int first = 0;
      if (version_changed(LV_PROMPT_KEY, synth_prompt_version, &first))
      {
         r.prompt_bumped = 1;
         r.synth_ops_reset = db2_synth_reenqueue_all();
         aimee_log(LOG_INFO, "kb.learning.version",
                   "synthesis prompt_version -> %s; replaying %d synth op(s)", synth_prompt_version,
                   r.synth_ops_reset);
      }
   }

   if (out)
      *out = r;
   return 0;
}
