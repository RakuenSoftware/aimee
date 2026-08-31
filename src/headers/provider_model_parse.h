/* provider_model_parse.h: shared parsing for provider model-list responses.
 *
 * Six provider profiles each had their own copy of the same loop over an
 * OpenAI-shaped {"data":[{"id":...}]} body. Header-only so adding it needs no
 * build wiring; the bodies are small and each profile calls one of them once.
 */
#ifndef DEC_PROVIDER_MODEL_PARSE_H
#define DEC_PROVIDER_MODEL_PARSE_H 1

#include "cJSON.h"
#include "provider_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Parse an OpenAI-shaped model list into provider_model_t rows.
 *
 * Accepts either {"data":[...]} (OpenAI, MiniMax, OpenRouter, Mistral) or
 * {"models":[...]} (Ollama), and array entries that are either bare strings or
 * objects carrying "id" (or "name", which Ollama uses).
 *
 * ONLY the id is populated. These endpoints publish no limits, so every other
 * field stays 0 = "not published" and the operator's declared value survives.
 * A provider that does publish limits should parse them itself rather than
 * extend this helper with fields most of its callers cannot supply.
 *
 * Returns 0 and sets *out / *n_out on success (caller free()s *out), -1
 * otherwise. Returns -1 for an empty list: a provider that reports no models is
 * indistinguishable here from one whose response we failed to understand, and
 * the caller's fallback (cached rows, then operator config) is right for both. */
static inline int provider_models_from_list_json(cJSON *root, provider_model_t **out, int *n_out)
{
   if (!root || !out || !n_out)
      return -1;
   *out = NULL;
   *n_out = 0;

   cJSON *arr = cJSON_GetObjectItem(root, "data");
   if (!arr || !cJSON_IsArray(arr))
      arr = cJSON_GetObjectItem(root, "models");
   if (!arr || !cJSON_IsArray(arr))
      return -1;

   int n = cJSON_GetArraySize(arr);
   if (n <= 0)
      return -1;
   provider_model_t *rows = calloc((size_t)n, sizeof(*rows));
   if (!rows)
      return -1;

   int count = 0;
   cJSON *item;
   cJSON_ArrayForEach(item, arr)
   {
      const char *id = NULL;
      if (cJSON_IsString(item))
         id = item->valuestring;
      else if (cJSON_IsObject(item))
      {
         cJSON *j = cJSON_GetObjectItem(item, "id");
         if (!j || !cJSON_IsString(j))
            j = cJSON_GetObjectItem(item, "name");
         if (j && cJSON_IsString(j))
            id = j->valuestring;
      }
      if (!id || !id[0])
         continue;
      snprintf(rows[count].id, sizeof(rows[count].id), "%s", id);
      count++;
   }

   if (count == 0)
   {
      free(rows);
      return -1;
   }
   *out = rows;
   *n_out = count;
   return 0;
}

#endif /* DEC_PROVIDER_MODEL_PARSE_H */
