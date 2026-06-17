/* kb_curator_judge.c: LLM judge sidecar for the resolve_entities ambiguous
 * band. See kb_curator_judge.h for the contract. No DB access. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_curator_judge.h"
#include "kb_curator_sidecar.h"
#include "kb_curator_llm.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* System prompt for the judge stage (Tier-B) when routed through a configured
 * provider (§2b). The legacy python sidecar carried its own prompt; the
 * in-process provider needs one. The request JSON (mention + candidate + score)
 * is the user turn; tune against the model. */
#define CJ_SYSTEM_PROMPT                                                                           \
   "You are an entity-resolution judge for a knowledge base. Given a candidate "                   \
   "mention and an existing entity as JSON, decide whether they are the same "                     \
   "canonical thing. Respond with a single JSON object: {\"same_entity\": "                        \
   "true|false}. When unsure, answer false."

/* Build the request JSON. Returns a malloc'd string (caller frees) or NULL. */
static char *cj_build_request(const char *mention_name, const char *mention_context,
                              const char *candidate_name, double score)
{
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return NULL;
   cJSON_AddStringToObject(root, "task", "same_entity");
   cJSON *mention = cJSON_AddObjectToObject(root, "mention");
   cJSON *candidate = cJSON_AddObjectToObject(root, "candidate");
   if (mention)
   {
      cJSON_AddStringToObject(mention, "name", mention_name ? mention_name : "");
      cJSON_AddStringToObject(mention, "context", mention_context ? mention_context : "");
   }
   if (candidate)
      cJSON_AddStringToObject(candidate, "name", candidate_name ? candidate_name : "");
   cJSON_AddNumberToObject(root, "score", score);

   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   return json;
}

int kb_curator_judge_same_entity(const config_t *cfg, const char *judge_cmd,
                                 const char *mention_name, const char *mention_context,
                                 const char *candidate_name, double score, int *out_same,
                                 char *errbuf, size_t errlen)
{
   if (errbuf && errlen)
      errbuf[0] = '\0';
   if (!out_same)
      return -1;

   char *request = cj_build_request(mention_name, mention_context, candidate_name, score);
   if (!request)
   {
      if (errbuf)
         snprintf(errbuf, errlen, "failed to build request json");
      return -1;
   }

   /* Tier-B dispatch: a configured tier_b.* provider runs in-process, else the
    * judge_cmd sidecar; "neither configured" surfaces as -1 (caller treats as
    * not-same / create). */
   char local_err[256];
   char *response = kb_curator_llm_run(cfg, KB_CURATOR_STAGE_JUDGE, CJ_SYSTEM_PROMPT, request,
                                       judge_cmd, 0, local_err, sizeof(local_err));
   free(request);
   if (!response)
   {
      if (errbuf)
         snprintf(errbuf, errlen, "%s", local_err);
      return -1;
   }

   /* The sidecar may wrap its JSON in prose or code fences; locate the first
    * object. cJSON_Parse tolerates leading whitespace but not leading prose, so
    * scan to the first '{'. */
   const char *start = strchr(response, '{');
   cJSON *root = start ? cJSON_Parse(start) : NULL;
   free(response);
   if (!root)
   {
      if (errbuf)
         snprintf(errbuf, errlen, "unparseable judge response");
      return -1;
   }

   const cJSON *same = cJSON_GetObjectItemCaseSensitive(root, "same_entity");
   int rc = -1;
   if (cJSON_IsBool(same))
   {
      *out_same = cJSON_IsTrue(same) ? 1 : 0;
      rc = 0;
   }
   else if (errbuf)
   {
      snprintf(errbuf, errlen, "judge response missing boolean 'same_entity'");
   }
   cJSON_Delete(root);
   return rc;
}
