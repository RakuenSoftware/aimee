#include "server_native_primitive.h"

cJSON *server_native_primitive_rows(const cJSON *recall)
{
   const char *groups[] = {"identity", "preferences", "active_context", "open_commitments"};
   cJSON *rows = cJSON_CreateArray();
   if (!cJSON_IsObject(recall) || !rows)
      goto invalid;
   for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); ++g)
   {
      const cJSON *group = cJSON_GetObjectItemCaseSensitive(recall, groups[g]);
      if (!cJSON_IsArray(group))
         goto invalid;
      const cJSON *row = NULL;
      cJSON_ArrayForEach(row, group)
      {
         const cJSON *version = cJSON_GetObjectItemCaseSensitive(row, "version");
         const cJSON *content = cJSON_GetObjectItemCaseSensitive(row, "content");
         if (!cJSON_IsObject(version) || !cJSON_IsString(content))
            goto invalid;
         const cJSON *owner = cJSON_GetObjectItemCaseSensitive(version, "owner_id");
         const cJSON *record = cJSON_GetObjectItemCaseSensitive(version, "record_id");
         const cJSON *existing = NULL;
         int duplicate = 0;
         cJSON_ArrayForEach(existing, rows)
         {
            const cJSON *prior = cJSON_GetObjectItemCaseSensitive(existing, "version");
            if (cJSON_Compare(owner, cJSON_GetObjectItemCaseSensitive(prior, "owner_id"), 1) &&
                cJSON_Compare(record, cJSON_GetObjectItemCaseSensitive(prior, "record_id"), 1))
            {
               if (!cJSON_Compare(version, prior, 1) ||
                   !cJSON_Compare(content, cJSON_GetObjectItemCaseSensitive(existing, "content"),
                                  1))
                  goto invalid;
               duplicate = 1;
               break;
            }
         }
         if (duplicate)
            continue;
         cJSON *copy = cJSON_Duplicate(row, 1);
         if (!copy || !cJSON_AddItemToArray(rows, copy))
         {
            cJSON_Delete(copy);
            goto invalid;
         }
      }
   }
   /* Validate the complete authorized view before truncating. Conflicts in a
    * later section must refuse export even when the selected bank is full. */
   while (cJSON_GetArraySize(rows) > 32)
      cJSON_DeleteItemFromArray(rows, cJSON_GetArraySize(rows) - 1);
   return rows;

invalid:
   cJSON_Delete(rows);
   return NULL;
}
