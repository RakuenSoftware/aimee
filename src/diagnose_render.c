/* diagnose_render.c: turning a diagnosis into a document.
 *
 * These three read nothing directly. They call db1_diagnose_get, _list_items,
 * _rank_hypotheses and _suggest_probes -- operations in their own right -- and
 * format what comes back as JSON or as a status block. Formatting is not
 * storage, so it belongs on this side of the module boundary, where the caller
 * already is.
 *
 * Keeping them in the module would have meant giving the module a growable
 * string buffer and a JSON writer to serve a rendering nobody stores. The
 * queries cross the bus; the rendering does not need to.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "diagnose.h"
#include "dstr.h"

static cJSON *item_to_json(const diagnosis_item_t *it)
{
   cJSON *obj = cJSON_CreateObject();
   cJSON_AddNumberToObject(obj, "id", it->id);
   cJSON_AddStringToObject(obj, "kind", it->kind);
   cJSON_AddNumberToObject(obj, "parent_id", it->parent_id);
   cJSON_AddStringToObject(obj, "content", it->content);
   cJSON_AddStringToObject(obj, "source", it->source);
   cJSON_AddNumberToObject(obj, "evidence_rank", it->evidence_rank);
   cJSON_AddStringToObject(obj, "created_at", it->created_at);
   return obj;
}

static cJSON *diagnosis_to_json_summary(const diagnosis_t *d)
{
   cJSON *obj = cJSON_CreateObject();
   cJSON_AddNumberToObject(obj, "id", d->id);
   cJSON_AddStringToObject(obj, "symptom", d->symptom);
   cJSON_AddStringToObject(obj, "status", d->status);
   cJSON_AddStringToObject(obj, "conclusion", d->conclusion);
   cJSON_AddNumberToObject(obj, "confidence", d->confidence);
   cJSON_AddStringToObject(obj, "created_at", d->created_at);
   cJSON_AddStringToObject(obj, "updated_at", d->updated_at);
   return obj;
}

char *db1_diagnose_json_full(int diag_id)
{
   diagnosis_t d;
   if (db1_diagnose_get(diag_id, &d) != 0)
      return NULL;

   cJSON *obj = diagnosis_to_json_summary(&d);

   diagnosis_item_t items[512];
   int n = db1_diagnose_list_items(diag_id, items, 512);
   cJSON *arr = cJSON_AddArrayToObject(obj, "items");
   for (int i = 0; i < n; i++)
      cJSON_AddItemToArray(arr, item_to_json(&items[i]));

   diagnosis_ranking_t rankings[64];
   int r = db1_diagnose_rank_hypotheses(diag_id, rankings, 64);
   cJSON *rarr = cJSON_AddArrayToObject(obj, "hypotheses_ranked");
   for (int i = 0; i < r; i++)
   {
      cJSON *h = cJSON_CreateObject();
      cJSON_AddNumberToObject(h, "id", rankings[i].hypothesis.id);
      cJSON_AddStringToObject(h, "content", rankings[i].hypothesis.content);
      cJSON_AddNumberToObject(h, "confidence", rankings[i].confidence);
      cJSON_AddNumberToObject(h, "evidence_for", rankings[i].evidence_for_count);
      cJSON_AddNumberToObject(h, "evidence_against", rankings[i].evidence_against_count);
      cJSON_AddNumberToObject(h, "strongest_for_rank", rankings[i].strongest_for_rank);
      cJSON_AddNumberToObject(h, "strongest_against_rank", rankings[i].strongest_against_rank);
      cJSON_AddItemToArray(rarr, h);
   }

   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json;
}
