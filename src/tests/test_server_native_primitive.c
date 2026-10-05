#include "server/server_native_primitive.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static cJSON *view(void)
{
   return cJSON_Parse("{\"identity\":[],\"preferences\":[],\"active_context\":[],"
                      "\"open_commitments\":[]}");
}

static void add(cJSON *recall, const char *group, const char *owner, const char *id,
                const char *revision, const char *text)
{
   cJSON *row = cJSON_CreateObject();
   cJSON *version = cJSON_AddObjectToObject(row, "version");
   cJSON_AddNumberToObject(version, "schema_version", 1);
   cJSON_AddStringToObject(version, "owner_id", owner);
   cJSON_AddStringToObject(version, "record_id", id);
   cJSON_AddStringToObject(version, "record_revision", revision);
   cJSON_AddStringToObject(row, "content", text);
   assert(cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(recall, group), row));
}

int main(void)
{
   cJSON *recall = view();
   cJSON *rows = server_native_primitive_rows(recall);
   assert(rows && cJSON_GetArraySize(rows) == 0);
   cJSON_Delete(rows);
   add(recall, "identity", "owner", "9007199254740993", "1", "first");
   add(recall, "active_context", "owner", "9007199254740993", "1", "first");
   add(recall, "preferences", "other-owner", "9007199254740993", "1", "second");
   cJSON *unchanged = cJSON_Duplicate(recall, 1);
   rows = server_native_primitive_rows(recall);
   assert(rows && cJSON_GetArraySize(rows) == 2);
   assert(cJSON_Compare(recall, unchanged, 1));
   assert(
       strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rows, 0), "content")->valuestring,
              "first") == 0);
   cJSON_Delete(unchanged);
   cJSON_Delete(rows);
   add(recall, "open_commitments", "owner", "9007199254740993", "2", "first");
   assert(server_native_primitive_rows(recall) == NULL);
   cJSON_DeleteItemFromArray(cJSON_GetObjectItemCaseSensitive(recall, "open_commitments"), 0);
   add(recall, "open_commitments", "owner", "9007199254740993", "1", "changed");
   assert(server_native_primitive_rows(recall) == NULL);
   cJSON_Delete(recall);

   recall = view();
   for (int i = 0; i < 40; ++i)
   {
      char id[16];
      snprintf(id, sizeof(id), "%d", i);
      add(recall, "identity", "owner", id, "1", "source");
   }
   rows = server_native_primitive_rows(recall);
   assert(rows && cJSON_GetArraySize(rows) == 32);
   const cJSON *version = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rows, 31), "version");
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(version, "record_id")->valuestring, "31") == 0);
   cJSON_Delete(rows);
   /* The omitted tail still participates in conflict validation. */
   add(recall, "active_context", "owner", "39", "2", "source");
   assert(server_native_primitive_rows(recall) == NULL);
   cJSON_Delete(recall);

   recall = view();
   cJSON_DeleteItemFromObjectCaseSensitive(recall, "preferences");
   assert(server_native_primitive_rows(recall) == NULL);
   cJSON_Delete(recall);
   recall = view();
   cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(recall, "identity"), cJSON_CreateObject());
   assert(server_native_primitive_rows(recall) == NULL);
   cJSON_Delete(recall);
   assert(server_native_primitive_rows(NULL) == NULL);
   puts("native primitive projection: overlap, exact IDs, conflicts, cap and malformed views pass");
   return 0;
}
