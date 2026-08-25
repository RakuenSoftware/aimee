/* td_search_render.c: see td_search_render.h. */
#include "td_search_render.h"
#include "dstr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *td_retrieval_outcome_name(td_retrieval_outcome_t outcome)
{
   switch (outcome)
   {
   case TD_RETRIEVAL_FOUND:
      return "found";
   case TD_RETRIEVAL_EMPTY:
      return "empty";
   case TD_RETRIEVAL_DEGRADED:
      return "degraded";
   default:
      return "failed";
   }
}

char *td_render_retrieval_continuation(td_retrieval_outcome_t outcome, const char *source,
                                       const char *query, const char *message)
{
   cJSON *root = cJSON_CreateObject();
   cJSON *offers = NULL;
   if (!root)
      return NULL;
   cJSON_AddStringToObject(root, "status", td_retrieval_outcome_name(outcome));
   cJSON_AddStringToObject(root, "source", source ? source : "knowledge_base");
   cJSON_AddStringToObject(root, "message", message ? message : "");
   offers = cJSON_AddArrayToObject(root, "continuations");

   /* An external search is a useful alternative for an empty or degraded local
    * corpus.  It remains inert data: dispatch must re-enter the ordinary tool,
    * capability, execution-policy, and effect-authorization path. */
   if (offers && (outcome == TD_RETRIEVAL_EMPTY || outcome == TD_RETRIEVAL_DEGRADED) && query &&
       query[0])
   {
      cJSON *offer = cJSON_CreateObject();
      cJSON *args = cJSON_CreateObject();
      if (offer && args)
      {
         cJSON_AddStringToObject(offer, "action", "web_search");
         cJSON_AddStringToObject(args, "query", query);
         cJSON_AddItemToObject(offer, "arguments", args);
         args = NULL;
         cJSON_AddStringToObject(offer, "required_capability", "external_read");
         cJSON_AddNumberToObject(offer, "remaining_budget", 1);
         cJSON_AddBoolToObject(offer, "policy_recheck", 1);
         cJSON_AddBoolToObject(offer, "authorized", 0);
         cJSON_AddItemToArray(offers, offer);
         offer = NULL;
      }
      cJSON_Delete(args);
      cJSON_Delete(offer);
   }

   char *wire = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!wire)
      return NULL;
   dstr_t d;
   dstr_init(&d);
   dstr_append_str(&d, "<aimee_retrieval_outcome>");
   dstr_append_str(&d, wire);
   dstr_append_str(&d, "</aimee_retrieval_outcome>");
   free(wire);
   return dstr_steal(&d);
}

char *td_render_search_hits(const cJSON *hits, const char *query)
{
   dstr_t d;
   dstr_init(&d);

   int n = cJSON_IsArray(hits) ? cJSON_GetArraySize(hits) : 0;
   if (n <= 0)
   {
      char message[384];
      snprintf(message, sizeof(message), "No knowledge-base results for \"%.300s\".",
               query ? query : "");
      char *contract = td_render_retrieval_continuation(
          TD_RETRIEVAL_EMPTY, "knowledge_base", query, message);
      dstr_append_str(&d, message);
      if (contract)
      {
         dstr_append_str(&d, "\n");
         dstr_append_str(&d, contract);
         free(contract);
      }
      return dstr_steal(&d);
   }

   dstr_appendf(&d, "Knowledge-base results for \"%s\" (%d):\n", query ? query : "", n);
   int i = 0;
   const cJSON *h;
   cJSON_ArrayForEach(h, hits)
   {
      i++;
      const cJSON *aid = cJSON_GetObjectItemCaseSensitive(h, "artifact_id");
      const cJSON *sc = cJSON_GetObjectItemCaseSensitive(h, "score");
      const cJSON *ex = cJSON_GetObjectItemCaseSensitive(h, "excerpt");
      dstr_appendf(&d, "%d. %s", i,
                   (cJSON_IsString(aid) && aid->valuestring[0]) ? aid->valuestring : "(unknown)");
      if (cJSON_IsNumber(sc))
         dstr_appendf(&d, " (score %.3f)", sc->valuedouble);
      dstr_append_str(&d, "\n");
      if (cJSON_IsString(ex) && ex->valuestring[0])
      {
         char buf[321];
         snprintf(buf, sizeof(buf), "%.320s", ex->valuestring);
         dstr_append_str(&d, "   ");
         dstr_append_str(&d, buf);
         dstr_append_str(&d, "\n");
      }
   }
   return dstr_steal(&d);
}

int td_extract_hit_docs(const cJSON *hits, int64_t *ids, const char **snips, int max)
{
   if (!cJSON_IsArray(hits) || !ids || !snips || max <= 0)
      return 0;
   int cn = 0;
   const cJSON *h;
   cJSON_ArrayForEach(h, hits)
   {
      if (cn >= max)
         break;
      const cJSON *did = cJSON_GetObjectItemCaseSensitive(h, "doc_id");
      const cJSON *ex = cJSON_GetObjectItemCaseSensitive(h, "excerpt");
      if (cJSON_IsNumber(did) && did->valuedouble > 0)
      {
         ids[cn] = (int64_t)did->valuedouble;
         snips[cn] = cJSON_IsString(ex) ? ex->valuestring : "";
         cn++;
      }
   }
   return cn;
}

char *td_search_result_from_response(const cJSON *resp, const char *query)
{
   /* /v1/search returns {"hits":[{artifact_id,score,doc_id,excerpt,...}]}. A
    * refactor that moved kb_search() onto this http path left the tool
    * unwrapping a legacy {"result":"<text>"} that the endpoint no longer sends —
    * so it errored on every call. Honour {result} if some variant still returns
    * it, else render the hits; only a response with neither is a real error. */
   const cJSON *legacy = resp ? cJSON_GetObjectItemCaseSensitive(resp, "result") : NULL;
   const cJSON *hits = resp ? cJSON_GetObjectItemCaseSensitive(resp, "hits") : NULL;
   if (cJSON_IsString(legacy) && legacy->valuestring[0])
   {
      size_t n = strlen(legacy->valuestring) + 1;
      char *out = malloc(n);
      if (out)
         memcpy(out, legacy->valuestring, n);
      return out;
   }
   if (cJSON_IsArray(hits))
      return td_render_search_hits(hits, query);

   char *contract = td_render_retrieval_continuation(
       TD_RETRIEVAL_FAILED, "knowledge_base", query, "knowledge search unavailable");
   dstr_t d;
   dstr_init(&d);
   dstr_append_str(&d, "error: knowledge search unavailable");
   if (contract)
   {
      dstr_append_str(&d, "\n");
      dstr_append_str(&d, contract);
      free(contract);
   }
   return dstr_steal(&d);
}
