/* kb_surprising_judge.c: §4 surprising-links confirmation. See the header for the
 * contract. Resolves node->path via the embeddings table, reads each file's symbol
 * outline from the canonical index, computes a cheap shared-symbol cross-check, and
 * runs ONE batched Tier-B LLM judge (kb_curator_llm_run), mirroring kb_curator_judge.c. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_surprising_judge.h"
#include "kb_curator_llm.h"
#include "db2/pgvec_transport.h" /* pgvec_code_node_path */
#include "db2/canonical_index.h" /* canonical_index_structure */
#include "headers/index.h"       /* definition_t */
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SJ_MAX_SYMBOLS 40 /* symbols per file folded into the prompt (bounds size) */
#define SJ_PATH_MAX    512

#define SJ_SYSTEM_PROMPT                                                                           \
   "You are a code-architecture judge. Each input pair is two source files flagged because they "  \
   "are SEMANTICALLY similar (high embedding cosine) yet STRUCTURALLY far apart in the "           \
   "call/dependency graph (large hop distance, or disconnected). A GENUINE surprising link means " \
   "the two files implement PARALLEL or DUPLICATED logic that probably should share code but "     \
   "does "                                                                                         \
   "not. A FALSE positive is coincidental similarity (generic boilerplate, tests, or unrelated "   \
   "domains that merely share vocabulary). Judge each pair from its path + symbol outline. "       \
   "Respond "                                                                                      \
   "with ONLY one JSON object, no prose: {\"verdicts\":[{\"i\":<index>,\"surprising\":true|false," \
   "\"reason\":\"<=12 words\"}]}. Include one entry per input pair, echoing its \"i\". When "      \
   "unsure, "                                                                                      \
   "answer false."

/* Append the file's symbol names (capped) to `arr` and return the count. */
static int sj_add_symbols(cJSON *arr, const char *project, const char *path, char names[][128],
                          int names_cap)
{
   definition_t defs[SJ_MAX_SYMBOLS];
   int nd = canonical_index_structure(project, path, defs, SJ_MAX_SYMBOLS);
   if (nd < 0)
      nd = 0;
   int kept = 0;
   for (int i = 0; i < nd && kept < names_cap; i++)
   {
      if (!defs[i].name[0])
         continue;
      if (arr)
         cJSON_AddItemToArray(arr, cJSON_CreateString(defs[i].name));
      snprintf(names[kept], 128, "%s", defs[i].name);
      kept++;
   }
   return kept;
}

/* Symbol-name intersection size between two captured name sets (the cheap
 * cross-check: real parallel implementations tend to share function names). */
static int sj_shared_count(char a[][128], int na, char b[][128], int nb)
{
   int shared = 0;
   for (int i = 0; i < na; i++)
      for (int j = 0; j < nb; j++)
         if (a[i][0] && strcmp(a[i], b[j]) == 0)
         {
            shared++;
            break;
         }
   return shared;
}

/* Add one pair object to the request array; fills out->shared_symbols. Returns 1 if
 * both endpoints resolved to a path (pair is judgeable), 0 otherwise. */
static int sj_add_pair(cJSON *pairs, const char *project, int idx,
                       const kb_graph_surprising_t *link, kb_surprising_verdict_t *out)
{
   char path_a[SJ_PATH_MAX] = "", path_b[SJ_PATH_MAX] = "";
   if (pgvec_code_node_path(project, link->a, path_a, sizeof(path_a)) != 0 ||
       pgvec_code_node_path(project, link->b, path_b, sizeof(path_b)) != 0)
      return 0;

   cJSON *p = cJSON_CreateObject();
   if (!p)
      return 0;
   cJSON_AddNumberToObject(p, "i", idx);
   cJSON *ja = cJSON_AddObjectToObject(p, "a");
   cJSON *jb = cJSON_AddObjectToObject(p, "b");
   char names_a[SJ_MAX_SYMBOLS][128], names_b[SJ_MAX_SYMBOLS][128];
   int na = 0, nb = 0;
   if (ja)
   {
      cJSON_AddStringToObject(ja, "path", path_a);
      na = sj_add_symbols(cJSON_AddArrayToObject(ja, "symbols"), project, path_a, names_a,
                          SJ_MAX_SYMBOLS);
   }
   if (jb)
   {
      cJSON_AddStringToObject(jb, "path", path_b);
      nb = sj_add_symbols(cJSON_AddArrayToObject(jb, "symbols"), project, path_b, names_b,
                          SJ_MAX_SYMBOLS);
   }
   cJSON_AddNumberToObject(p, "cosine", link->cosine);
   cJSON_AddNumberToObject(p, "hops", link->hops);
   cJSON_AddItemToArray(pairs, p);
   out->shared_symbols = sj_shared_count(names_a, na, names_b, nb);
   out->sent = 1;
   return 1;
}

int kb_surprising_judge(const config_t *cfg, const char *judge_cmd, const char *project,
                        const kb_graph_surprising_t *links, int n, kb_surprising_verdict_t *out,
                        char *errbuf, size_t errlen)
{
   if (errbuf && errlen)
      errbuf[0] = '\0';
   if (!project || !*project || !links || !out || n <= 0)
      return -1;
   for (int i = 0; i < n; i++)
      memset(&out[i], 0, sizeof(out[i]));

   cJSON *root = cJSON_CreateObject();
   if (!root)
   {
      if (errbuf)
         snprintf(errbuf, errlen, "failed to build request json");
      return -1;
   }
   cJSON_AddStringToObject(root, "task", "surprising_links");
   cJSON *pairs = cJSON_AddArrayToObject(root, "pairs");
   int judgeable = 0;
   for (int i = 0; i < n; i++)
      if (pairs && sj_add_pair(pairs, project, i, &links[i], &out[i]))
         judgeable++;
   if (judgeable == 0)
   {
      cJSON_Delete(root); /* nothing resolvable -> no LLM call, all left judged=0 */
      return 0;
   }

   char *request = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!request)
   {
      if (errbuf)
         snprintf(errbuf, errlen, "failed to serialize request json");
      return -1;
   }

   char local_err[256];
   char *response = kb_curator_llm_run(cfg, KB_CURATOR_STAGE_JUDGE, SJ_SYSTEM_PROMPT, request, NULL,
                                       judge_cmd, 0, local_err, sizeof(local_err));
   free(request);
   if (!response)
   {
      /* No Tier-B provider/sidecar configured (or it failed): not an error — the
       * route still returns the structural candidates, just unconfirmed. */
      if (errbuf)
         snprintf(errbuf, errlen, "%s", local_err);
      return 0;
   }

   const char *start = strchr(response, '{');
   cJSON *resp = start ? cJSON_Parse(start) : NULL;
   free(response);
   if (!resp)
   {
      if (errbuf)
         snprintf(errbuf, errlen, "unparseable judge response");
      return -1;
   }

   const cJSON *verdicts = cJSON_GetObjectItemCaseSensitive(resp, "verdicts");
   int judged = 0;
   if (cJSON_IsArray(verdicts))
   {
      const cJSON *v = NULL;
      cJSON_ArrayForEach(v, verdicts)
      {
         const cJSON *ji = cJSON_GetObjectItemCaseSensitive(v, "i");
         const cJSON *js = cJSON_GetObjectItemCaseSensitive(v, "surprising");
         if (!cJSON_IsNumber(ji) || !cJSON_IsBool(js))
            continue;
         int idx = (int)ji->valuedouble;
         /* Reject verdicts for an index that was never sent (unresolved pair, or a
          * hallucinated/duplicate echo): only pairs in the request can be judged. */
         if (idx < 0 || idx >= n || !out[idx].sent || out[idx].judged)
            continue;
         out[idx].judged = 1;
         out[idx].confirmed = cJSON_IsTrue(js) ? 1 : 0;
         const cJSON *jr = cJSON_GetObjectItemCaseSensitive(v, "reason");
         if (cJSON_IsString(jr) && jr->valuestring)
            snprintf(out[idx].reason, sizeof(out[idx].reason), "%s", jr->valuestring);
         judged++;
      }
   }
   cJSON_Delete(resp);
   if (judged == 0 && errbuf)
      snprintf(errbuf, errlen, "judge response had no usable verdicts");
   return judged;
}
