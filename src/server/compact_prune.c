/* compact_prune.c: pre-summarization tool-result content pruning.
 *
 * See headers/compact_prune.h for rationale and usage.
 */
#include "headers/compact_prune.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ id→name map */

#define MAX_TOOL_IDS 128

typedef struct
{
   char id[128];
   char name[64];
} tool_id_entry_t;

typedef struct
{
   tool_id_entry_t entries[MAX_TOOL_IDS];
   int count;
} tool_id_map_t;

static void map_insert(tool_id_map_t *m, const char *id, const char *name)
{
   if (!id || !name || !id[0] || !name[0] || m->count >= MAX_TOOL_IDS)
      return;
   tool_id_entry_t *e = &m->entries[m->count++];
   snprintf(e->id, sizeof(e->id), "%s", id);
   snprintf(e->name, sizeof(e->name), "%s", name);
}

static const char *map_lookup(const tool_id_map_t *m, const char *id)
{
   if (!id || !id[0])
      return NULL;
   for (int i = 0; i < m->count; i++)
      if (strcmp(m->entries[i].id, id) == 0)
         return m->entries[i].name;
   return NULL;
}

/* Scan a messages array and populate the id→name map from tool-call messages. */
static void build_id_map(const cJSON *messages, int end_idx, tool_id_map_t *m)
{
   if (!messages || !cJSON_IsArray(messages))
      return;

   int idx = 0;
   const cJSON *msg;
   cJSON_ArrayForEach(msg, messages)
   {
      if (idx >= end_idx)
         break;
      idx++;

      /* OpenAI: {role:"assistant", tool_calls:[{id, function:{name}}]} */
      cJSON *tc = cJSON_GetObjectItem(msg, "tool_calls");
      if (tc && cJSON_IsArray(tc))
      {
         cJSON *call;
         cJSON_ArrayForEach(call, tc)
         {
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(call, "id"));
            cJSON *fn = cJSON_GetObjectItem(call, "function");
            const char *nm = fn ? cJSON_GetStringValue(cJSON_GetObjectItem(fn, "name")) : NULL;
            map_insert(m, id, nm);
         }
      }

      /* Anthropic: {role:"assistant", content:[{type:"tool_use", id, name}]} */
      cJSON *content = cJSON_GetObjectItem(msg, "blocks");
      int canonical = content != NULL;
      if (!content)
         content = cJSON_GetObjectItem(msg, "content");
      if (content && cJSON_IsArray(content))
      {
         cJSON *block;
         cJSON_ArrayForEach(block, content)
         {
            const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
            if (!type || strcmp(type, "tool_use") != 0)
               continue;
            const char *id =
                cJSON_GetStringValue(cJSON_GetObjectItem(block, canonical ? "tool_id" : "id"));
            const char *nm = cJSON_GetStringValue(cJSON_GetObjectItem(block, "name"));
            map_insert(m, id, nm);
         }
      }
   }
}

/* ------------------------------------------------------------------ content measurement */

/* Return the serialised byte length of a content item (string or array). */
static size_t content_bytes(const cJSON *content)
{
   if (!content)
      return 0;
   if (cJSON_IsString(content))
      return strlen(content->valuestring);
   /* Array or object: serialise to measure */
   char *s = cJSON_PrintUnformatted((cJSON *)content);
   if (!s)
      return 0;
   size_t len = strlen(s);
   free(s);
   return len;
}

/* ------------------------------------------------------------------ stub builder */

static void make_stub(char *buf, size_t cap, const char *tool_name, size_t orig_bytes)
{
   snprintf(buf, cap, "[%s]   %zu chars truncated",
            tool_name && tool_name[0] ? tool_name : "tool_result", orig_bytes);
}

/* ------------------------------------------------------------------ public API */

int compact_prune_tool_results(cJSON *messages, int start_idx, int end_idx, int threshold)
{
   if (!messages || !cJSON_IsArray(messages) || end_idx <= start_idx)
      return 0;

   int eff_thresh = threshold > 0 ? threshold : COMPACT_PRUNE_THRESHOLD_BYTES;

   /* Build tool id→name map from messages up to end_idx */
   tool_id_map_t id_map;
   memset(&id_map, 0, sizeof(id_map));
   build_id_map(messages, end_idx, &id_map);

   int saved = 0;
   int idx = 0;
   cJSON *msg;
   cJSON_ArrayForEach(msg, messages)
   {
      if (idx < start_idx)
      {
         idx++;
         continue;
      }
      if (idx >= end_idx)
         break;
      idx++;

      const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));

      /* OpenAI: role=tool */
      if (role && strcmp(role, "tool") == 0 && !cJSON_GetObjectItem(msg, "blocks"))
      {
         cJSON *content = cJSON_GetObjectItem(msg, "blocks");
         if (!content)
            content = cJSON_GetObjectItem(msg, "content");
         if (!content || !cJSON_IsString(content))
            continue;
         size_t orig = strlen(content->valuestring);
         if ((int)orig <= eff_thresh)
            continue;
         const char *tid = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "tool_call_id"));
         const char *name = map_lookup(&id_map, tid);
         char stub[192];
         make_stub(stub, sizeof(stub), name, orig);
         cJSON_SetValuestring(content, stub);
         saved += (int)(orig - strlen(stub));
         continue;
      }

      /* Anthropic: role=user with tool_result blocks */
      if (cJSON_GetObjectItem(msg, "blocks") || (role && strcmp(role, "user") == 0))
      {
         cJSON *content = cJSON_GetObjectItem(msg, "blocks");
         int canonical = content != NULL;
         if (!content)
            content = cJSON_GetObjectItem(msg, "content");
         if (!content || !cJSON_IsArray(content))
            continue;
         cJSON *block;
         cJSON_ArrayForEach(block, content)
         {
            const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
            if (!type || strcmp(type, "tool_result") != 0)
               continue;

            cJSON *rc = cJSON_GetObjectItem(block, "content");
            if (!rc)
               continue;
            size_t orig = content_bytes(rc);
            if ((int)orig <= eff_thresh)
               continue;

            const char *tid = cJSON_GetStringValue(
                cJSON_GetObjectItem(block, canonical ? "tool_id" : "tool_use_id"));
            const char *name = map_lookup(&id_map, tid);
            char stub[192];
            make_stub(stub, sizeof(stub), name, orig);

            /* Replace content with the stub string */
            if (cJSON_IsString(rc))
            {
               cJSON_SetValuestring(rc, stub);
            }
            else
            {
               cJSON_DeleteItemFromObject(block, "content");
               cJSON_AddStringToObject(block, "content", stub);
            }
            saved += (int)(orig - strlen(stub));
         }
      }
   }
   return saved;
}

double compact_prune_estimate_savings(cJSON *messages, int start_idx, int end_idx, int threshold)
{
   if (!messages || !cJSON_IsArray(messages) || end_idx <= start_idx)
      return 0.0;

   /* Measure range as-is */
   int idx = 0;
   int before = 0;
   cJSON *msg;
   cJSON_ArrayForEach(msg, messages)
   {
      if (idx >= start_idx && idx < end_idx)
      {
         char *s = cJSON_PrintUnformatted(msg);
         if (s)
         {
            before += (int)strlen(s);
            free(s);
         }
      }
      idx++;
      if (idx >= end_idx)
         break;
   }
   if (before == 0)
      return 0.0;

   /* Deep-copy range, prune it, measure again */
   cJSON *copy = cJSON_CreateArray();
   if (!copy)
      return 0.0;
   idx = 0;
   cJSON_ArrayForEach(msg, messages)
   {
      if (idx >= start_idx && idx < end_idx)
      {
         cJSON *dup = cJSON_Duplicate(msg, 1);
         if (dup)
            cJSON_AddItemToArray(copy, dup);
      }
      idx++;
      if (idx >= end_idx)
         break;
   }
   compact_prune_tool_results(copy, 0, cJSON_GetArraySize(copy), threshold);

   int after = 0;
   cJSON_ArrayForEach(msg, copy)
   {
      char *s = cJSON_PrintUnformatted(msg);
      if (s)
      {
         after += (int)strlen(s);
         free(s);
      }
   }
   cJSON_Delete(copy);

   if (after >= before)
      return 0.0;
   return (double)(before - after) / (double)before;
}
