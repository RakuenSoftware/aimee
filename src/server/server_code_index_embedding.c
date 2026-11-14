/* Generic embedding caller. The selected Native provider owns model execution
 * and identity verification; repository storage never becomes a memory owner. */
#include "server_code_index_embedding.h"
#include "config.h"
#include "headers/memory_stage_contract.h"
#include "module_json_call.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *embed_call(const char *endpoint, const char *operation, const char *text,
                         const char *input)
{
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "base_url", endpoint);
   cJSON_AddNumberToObject(request, "max_dim", 4000);
   if (operation)
      cJSON_AddStringToObject(request, "operation", operation);
   if (text)
   {
      cJSON_AddStringToObject(request, "text", text);
      cJSON_AddStringToObject(request, "input_type", input);
   }
   return aimee_module_json_call(AIMEE_MEMORY_EVENT_EMBED, AIMEE_MEMORY_STAGE_EMBED, request,
                                 262144, 30000, NULL);
}
static int valid_reply(const cJSON *reply)
{
   if (!cJSON_IsObject(reply) || cJSON_HasObjectItem(reply, "error"))
      return 0;
   const char *flags[] = {"unavailable", "unauthorized", "truncated"};
   for (unsigned i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i)
   {
      const cJSON *v = cJSON_GetObjectItemCaseSensitive(reply, flags[i]);
      if (v && !cJSON_IsFalse(v))
         return 0;
   }
   return 1;
}
int server_code_index_embedding_available(void)
{
   const char *endpoint = config_embedder_command_current(NULL);
   if (!endpoint || !endpoint[0])
      return 0;
   cJSON *reply = embed_call(endpoint, "serving-id", NULL, NULL);
   const cJSON *identity = cJSON_GetObjectItemCaseSensitive(reply, "serving_id");
   int available = valid_reply(reply) && cJSON_IsString(identity) && identity->valuestring[0];
   cJSON_Delete(reply);
   return available;
}
static void embed(const char *text, const char *input, cJSON *out)
{
   const char *configured = config_embedder_command_current(NULL);
   if (!configured || !configured[0])
      return;
   char endpoint[4096];
   if (strlen(configured) >= sizeof(endpoint))
      return;
   snprintf(endpoint, sizeof(endpoint), "%s", configured);
   cJSON *before = embed_call(endpoint, "serving-id", NULL, NULL);
   cJSON *value = NULL, *after = NULL;
   const cJSON *identity = cJSON_GetObjectItemCaseSensitive(before, "serving_id");
   if (!valid_reply(before) || !cJSON_IsString(identity) || !identity->valuestring[0] ||
       strlen(identity->valuestring) > 160)
      goto done;
   value = embed_call(endpoint, NULL, text, input);
   after = embed_call(endpoint, "serving-id", NULL, NULL);
   const cJSON *next = cJSON_GetObjectItemCaseSensitive(after, "serving_id");
   const cJSON *dim = cJSON_GetObjectItemCaseSensitive(value, "dim");
   const cJSON *vector = cJSON_GetObjectItemCaseSensitive(value, "vector");
   if (!valid_reply(value) || !valid_reply(after) || !cJSON_IsString(next) ||
       strcmp(identity->valuestring, next->valuestring) || !cJSON_IsNumber(dim) ||
       dim->valuedouble != dim->valueint || dim->valueint < 1 || dim->valueint > 4000 ||
       !cJSON_IsArray(vector) || cJSON_GetArraySize(vector) != dim->valueint)
      goto done;
   double norm = 0;
   const cJSON *component;
   cJSON_ArrayForEach(component, vector)
   {
      if (!cJSON_IsNumber(component) || !isfinite(component->valuedouble))
         goto done;
      norm += component->valuedouble * component->valuedouble;
   }
   if (norm <= 0 || !isfinite(norm))
      goto done;
   cJSON_AddItemToObject(out, "embedding", cJSON_Duplicate(vector, 1));
   cJSON_AddStringToObject(out, "embedding_serving", identity->valuestring);
   cJSON_AddStringToObject(out, "embedding_policy", "runtime-code-path-prefix8192-bytes-cosine/v1");
done:
   cJSON_Delete(before);
   cJSON_Delete(value);
   cJSON_Delete(after);
}
void server_code_index_embed_file(const char *path, const char *content, cJSON *file)
{
   size_t n = strlen(content);
   if (n > 8192)
   {
      n = 8192;
      while (n && ((unsigned char)content[n] & 0xc0) == 0x80)
         --n;
   }
   size_t cap = strlen(path) + n + 2;
   char *text = malloc(cap);
   if (!text)
      return;
   snprintf(text, cap, "%s\n%.*s", path, (int)n, content);
   embed(text, "document", file);
   free(text);
}
static int hex_digit(char c)
{
   if (c >= '0' && c <= '9')
      return c - '0';
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
   return -1;
}
void server_code_index_embed_query(const char *route, cJSON *request)
{
   if (strncmp(route, "/v1/code/hybrid?", 16) && strncmp(route, "/v1/code/search?", 16) &&
       strncmp(route, "/v1/code/context?", 17))
      return;
   const char *start = strstr(route, "?query=");
   if (!start)
      start = strstr(route, "&query=");
   if (!start)
      return;
   start += 7;
   size_t n = strcspn(start, "&");
   if (n == 0 || n > 65536)
      return;
   char *query = malloc(n + 1);
   if (!query)
      return;
   size_t used = 0;
   for (size_t i = 0; i < n; ++i)
   {
      if (start[i] == '%')
      {
         if (i + 2 >= n || hex_digit(start[i + 1]) < 0 || hex_digit(start[i + 2]) < 0)
            goto done;
         int c = hex_digit(start[i + 1]) * 16 + hex_digit(start[i + 2]);
         if (c == 0)
            goto done;
         query[used++] = (char)c;
         i += 2;
      }
      else
         query[used++] = start[i] == '+' ? ' ' : start[i];
   }
   query[used] = 0;
   embed(query, "query", request);
done:
   free(query);
}
