/* user_memory_merge.c: merge this user's DB1 recall rows into a KB recall array.
 *
 * This is array logic, not storage. Its only DB1 dependency is one read --
 * db1_user_memory_list_recall -- and everything else it does is cJSON surgery
 * on a document aimee-kb produced. It lived in db1/user_memory.c because that
 * is where the read lives, and moving it here is what let that source cross the
 * bus: a cJSON tree the callee MUTATES has no wire shape, while the read it
 * depends on is an ordinary list of rows.
 *
 * It sits in the core tree rather than beside its caller in modules/kb_client:
 * kb_client is a module, db1 is a module, and a module may only reach a peer
 * over the bus. This is daemon glue that links both, which is the ordinary
 * direction. Its own file rather than folded into a larger one so the property
 * its old comment claimed still holds: the merge is unit-testable without kb. */
#include "cJSON.h"
#include "db1_client/user_memory.h"

#include <string.h>

void db1_user_memory_merge_into_array(cJSON *arr, db1_user_recall_section_t section,
                                      const char *why)
{
   if (!cJSON_IsArray(arr))
      return;
   db1_user_memory_row_t rows[32];
   int n = db1_user_memory_list_recall(section, rows, 32);
   /* Insert in reverse so the highest-ranked db1 row ends up first. */
   for (int i = n - 1; i >= 0; i--)
   {
      /* db1 wins: drop any org row with the same key. Index is collected inside
       * the loop and the delete runs AFTER it exits — never during iteration. */
      int idx = 0, rm = -1;
      cJSON *it = NULL;
      cJSON_ArrayForEach(it, arr)
      {
         const char *k = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "key"));
         if (k && strcmp(k, rows[i].key) == 0)
         {
            rm = idx;
            break;
         }
         idx++;
      }
      if (rm >= 0)
         cJSON_DeleteItemFromArray(arr, rm);

      cJSON *row = cJSON_CreateObject();
      if (!row)
         break;
      cJSON_AddNumberToObject(row, "memory_id", (double)rows[i].id);
      cJSON_AddStringToObject(row, "tier", rows[i].tier);
      cJSON_AddStringToObject(row, "kind", rows[i].kind);
      cJSON_AddStringToObject(row, "key", rows[i].key);
      cJSON_AddStringToObject(row, "text", rows[i].content);
      cJSON_AddStringToObject(row, "why", why ? why : "");
      cJSON_AddStringToObject(row, "scope", "user");
      cJSON_InsertItemInArray(arr, 0, row);
   }
}
