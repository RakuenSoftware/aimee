/* approach_store.c: the storage half of approach-level negative knowledge (S3).
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */

#include "approach_store.h"

#include "approach_failures.h" /* db1_approach_failure_* */

#include <aimee/learning/policy_arms.h>

#include "cJSON.h"
#include "kb_client.h"

#include <stdlib.h>

#include <stdio.h>
#include <string.h>

/* Rows scored per recall. Bounds work, not correctness: the real filter is the
 * overlap score, and a deeper pool can only add lower-ranked matches. */
#define APPROACH_STORE_POOL 64

int approach_store_record(const char *goal, const char *approach, const char *failure_mode,
                          const char *source, const char *source_ref)
{
   if (!goal || !goal[0] || !approach || !approach[0])
      return -1;

   char goal_sig[APPROACH_MEM_SIGNATURE_LEN];
   char approach_sig[APPROACH_MEM_SIGNATURE_LEN];
   char tokens[APPROACH_MEM_TOKENS_LEN];
   if (learning_approach_signature(goal, goal_sig, sizeof(goal_sig)) != 0 ||
       learning_approach_signature(approach, approach_sig, sizeof(approach_sig)) != 0)
      return -1;
   learning_approach_tokens(goal, tokens, sizeof(tokens));
   if (!tokens[0])
      return -1; /* nothing to recall against: storing it would bury it */

   return db1_approach_failure_record(goal_sig, goal, tokens, approach_sig, approach, failure_mode,
                                      source, source_ref);
}

/* The rarest-looking token of a goal, used only to narrow the candidate pool
 * before scoring. Picking the LONGEST token is a cheap proxy for specificity;
 * getting it wrong costs recall breadth, never correctness, because the real
 * filter is the overlap score. */
static void narrowing_token(const char *tokens, char *out, size_t out_len)
{
   out[0] = '\0';
   size_t best = 0;
   const char *p = tokens;
   while (*p)
   {
      while (*p == ' ')
         p++;
      const char *start = p;
      while (*p && *p != ' ')
         p++;
      size_t len = (size_t)(p - start);
      if (len > best && len + 1 < out_len)
      {
         best = len;
         memcpy(out, start, len);
         out[len] = '\0';
      }
   }
}

int approach_store_recall(const char *goal, learning_approach_hit_t *out, int max)
{
   if (!goal || !goal[0] || !out || max <= 0)
      return -1;

   char tokens[APPROACH_MEM_TOKENS_LEN];
   learning_approach_tokens(goal, tokens, sizeof(tokens));
   if (!tokens[0])
      return 0;

   char narrow[64];
   narrowing_token(tokens, narrow, sizeof(narrow));

   static db1_approach_failure_t rows[APPROACH_STORE_POOL];
   int n = db1_approach_failure_candidates(narrow, rows, APPROACH_STORE_POOL);
   if (n < 0)
      return -1;

   /* Score, filter, then insertion-sort the survivors by similarity. The pool
    * is bounded and small, so a simple sort beats a heap here. */
   int kept = 0;
   for (int i = 0; i < n; i++)
   {
      double sim = learning_approach_overlap(tokens, rows[i].goal_tokens);
      if (sim < APPROACH_MEM_MIN_SIMILARITY)
         continue;
      learning_approach_hit_t hit;
      memset(&hit, 0, sizeof(hit));
      snprintf(hit.goal_text, sizeof(hit.goal_text), "%s", rows[i].goal_text);
      snprintf(hit.approach_text, sizeof(hit.approach_text), "%s", rows[i].approach_text);
      snprintf(hit.failure_mode, sizeof(hit.failure_mode), "%s", rows[i].failure_mode);
      snprintf(hit.source_ref, sizeof(hit.source_ref), "%s", rows[i].source_ref);
      hit.occurrences = rows[i].occurrences;
      hit.similarity = sim;

      if (kept >= max && out[max - 1].similarity >= sim)
         continue;
      int pos = kept < max ? kept : max - 1;
      while (pos > 0 && out[pos - 1].similarity < sim)
      {
         out[pos] = out[pos - 1];
         pos--;
      }
      out[pos] = hit;
      if (kept < max)
         kept++;
   }
   return kept;
}

int approach_store_render(const char *goal, char *out, size_t out_len, char *arm_out,
                          size_t arm_out_len)
{
   if (arm_out && arm_out_len)
      arm_out[0] = '\0';
   if (!out || out_len == 0)
      return 0;
   out[0] = '\0';

   /* Whether this block is worth its tokens is a question, not a constant —
    * and the answer comes from the service that has the bandit. This binary
    * renders the fragment; it cannot sample it. An unreachable service or an
    * arm this build does not declare falls back to the local default, so the
    * output is never something no registry knows about. */
   char arm[LEARNING_POLICY_ARM_LEN] = "";
   (void)learning_policy_select(LEARNING_POLICY_PLAN_ADVISORY, arm, sizeof(arm));

   char *sel = kb_client_learning_policy_select_json(LEARNING_POLICY_PLAN_ADVISORY);
   cJSON *doc = sel ? cJSON_Parse(sel) : NULL;
   free(sel);
   if (doc)
   {
      const cJSON *chosen = cJSON_GetObjectItemCaseSensitive(doc, "arm");
      if (cJSON_IsString(chosen) &&
          learning_policy_arm_is_valid(LEARNING_POLICY_PLAN_ADVISORY, chosen->valuestring))
         snprintf(arm, sizeof(arm), "%s", chosen->valuestring);
      cJSON_Delete(doc);
   }
   if (arm_out && arm_out_len)
      snprintf(arm_out, arm_out_len, "%s", arm);

   if (strcmp(arm, LEARNING_POLICY_ADVISORY_OFF) == 0)
      return 0; /* the control arm: say nothing, so the block can be measured */

   learning_approach_hit_t hits[APPROACH_MEM_MAX_RECALL];
   int n = approach_store_recall(goal, hits, APPROACH_MEM_MAX_RECALL);
   if (n <= 0)
      return 0;

   if (strcmp(arm, LEARNING_POLICY_ADVISORY_BRIEF) == 0)
   {
      snprintf(out, out_len, "%d approach%s to a goal like this one has already failed.", n,
               n == 1 ? "" : "es");
      return n;
   }

   /* Reports, does not instruct: the planner is told what was tried and what
    * happened, and decides for itself. An imperative here would turn advisory
    * recall into an unreviewed rule. */
   size_t o = (size_t)snprintf(out, out_len,
                               "Approaches already tried for a goal like this, and how they went:");
   for (int i = 0; i < n && o + 1 < out_len; i++)
      o += (size_t)snprintf(out + o, out_len - o, "\n- %s -> %s (seen %lld time%s)",
                            hits[i].approach_text[0] ? hits[i].approach_text : "(unrecorded)",
                            hits[i].failure_mode[0] ? hits[i].failure_mode : "failed",
                            hits[i].occurrences, hits[i].occurrences == 1 ? "" : "s");
   return n;
}
