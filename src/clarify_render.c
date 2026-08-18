/* clarify_render.c: a clarify session as JSON.
 *
 * Serialisation of a struct the caller already holds. It reads no database and
 * answers no query, so it is not storage, and it lived in a DB1 source only
 * because that is where the struct is defined.
 *
 * The module has no use for it: nothing it stores is this document.
 */
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "clarify.h"

char *db1_clarify_to_json(const clarify_session_t *s)
{
   if (!s)
      return NULL;

   const char *status_str = (s->status == CLARIFY_READY)       ? "ready"
                            : (s->status == CLARIFY_CANCELLED) ? "cancelled"
                                                               : "open";
   cJSON *obj = cJSON_CreateObject();
   cJSON_AddNumberToObject(obj, "id", s->id);
   cJSON_AddStringToObject(obj, "description", s->description);
   cJSON_AddStringToObject(obj, "status", status_str);
   cJSON_AddNumberToObject(obj, "score", (double)s->score);
   cJSON_AddStringToObject(obj, "spec", s->spec);
   cJSON_AddStringToObject(obj, "created_at", s->created_at);
   cJSON_AddStringToObject(obj, "updated_at", s->updated_at);

   cJSON *qa_arr = cJSON_AddArrayToObject(obj, "qa");
   for (int i = 0; i < s->qa_count; i++)
   {
      cJSON *qa = cJSON_CreateObject();
      cJSON_AddStringToObject(qa, "dimension", s->qa[i].dimension);
      cJSON_AddStringToObject(qa, "question", s->qa[i].question);
      cJSON_AddStringToObject(qa, "answer", s->qa[i].answer);
      cJSON_AddBoolToObject(qa, "answered", s->qa[i].answered);
      cJSON_AddItemToArray(qa_arr, qa);
   }

   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json;
}
