#include "lsp_context.h"
#include "aimee.h"
#include "aimee_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int contained(const char *root, const char *path)
{
   size_t n = strlen(root);
   return !strncmp(root, path, n) && (path[n] == '/' || path[n] == '\0');
}

static int relative_path(const char *path)
{
   if (!path || !path[0] || path[0] == '/' || strchr(path, '\\'))
      return 0;
   const char *component = path;
   for (const char *p = path;; p++)
   {
      if (*p != '/' && *p != '\0')
         continue;
      size_t n = (size_t)(p - component);
      if (n == 0 || (n == 1 && component[0] == '.') ||
          (n == 2 && component[0] == '.' && component[1] == '.'))
         return 0;
      if (*p == '\0')
         return 1;
      component = p + 1;
   }
}

static void status(cJSON *item, const char *value, const char *reason)
{
   cJSON_AddStringToObject(item, "status", value);
   if (reason && reason[0])
      cJSON_AddStringToObject(item, "reason", reason);
}

cJSON *lsp_context_execute(const lsp_context_provider_t *provider, const cJSON *args)
{
   cJSON *response = cJSON_CreateObject();
   if (!response)
      return NULL;
   cJSON_AddStringToObject(response, "provider",
                           provider && provider->provider ? provider->provider : "local_lsp");
   if (!provider || !provider->root || !provider->project || !provider->worktree ||
       !provider->authorize || !provider->read_file || !provider->sync || !provider->query ||
       !provider->source)
   {
      cJSON_AddStringToObject(response, "status", "unavailable");
      cJSON_AddStringToObject(response, "reason", "semantic provider is incomplete");
      return response;
   }
   cJSON_AddStringToObject(response, "project", provider->project);
   cJSON_AddStringToObject(response, "worktree", provider->worktree);

   cJSON *operation = cJSON_GetObjectItemCaseSensitive(args, "operation");
   cJSON *anchors = cJSON_GetObjectItemCaseSensitive(args, "anchors");
   int count = cJSON_IsArray(anchors) ? cJSON_GetArraySize(anchors) : 0;
   int is_definition = cJSON_IsString(operation) && !strcmp(operation->valuestring, "definition");
   int is_references = cJSON_IsString(operation) && !strcmp(operation->valuestring, "references");
   if ((!is_definition && !is_references) || count < 1 || count > 16)
   {
      cJSON_AddStringToObject(response, "status", "abstained");
      cJSON_AddStringToObject(response, "reason",
                              "operation and one to sixteen anchors are required");
      return response;
   }

   int source_budget = 8192;
   cJSON *budget = cJSON_GetObjectItemCaseSensitive(args, "max_source_bytes");
   if (cJSON_IsNumber(budget) && budget->valuedouble >= 256 && budget->valuedouble <= 32768 &&
       budget->valuedouble == (int)budget->valuedouble)
      source_budget = (int)budget->valuedouble;
   int initial_source_budget = source_budget;
   cJSON *results = cJSON_AddArrayToObject(response, "results");
   int status_ok = 0, status_empty = 0, status_stale = 0, status_unavailable = 0;
   int status_unauthorized = 0, status_unsupported = 0, status_abstained = 0;
   int any_truncated = 0;
   unsigned long response_generation = 0;
   for (int ai = 0; ai < count; ai++)
   {
      cJSON *anchor = cJSON_GetArrayItem(anchors, ai);
      cJSON *file_j = cJSON_GetObjectItemCaseSensitive(anchor, "file");
      cJSON *line_j = cJSON_GetObjectItemCaseSensitive(anchor, "line");
      cJSON *col_j = cJSON_GetObjectItemCaseSensitive(anchor, "column");
      cJSON *item = cJSON_CreateObject();
      cJSON_AddItemToArray(results, item);
      if (!cJSON_IsString(file_j) || !relative_path(file_j->valuestring))
      {
         status(item, "unauthorized", NULL);
         status_unauthorized = 1;
         continue;
      }
      if (!cJSON_IsNumber(line_j) || !cJSON_IsNumber(col_j) || line_j->valuedouble < 1 ||
          col_j->valuedouble < 1 || line_j->valuedouble != (int)line_j->valuedouble ||
          col_j->valuedouble != (int)col_j->valuedouble)
      {
         status(item, "abstained", NULL);
         status_abstained = 1;
         continue;
      }

      char source_path[MAX_PATH_LEN];
      if (provider->authorize(provider->ctx, file_j->valuestring, source_path,
                              sizeof(source_path)) != 0 ||
          !contained(provider->root, source_path))
      {
         status(item, "unauthorized", NULL);
         status_unauthorized = 1;
         continue;
      }
      char *source = NULL;
      size_t source_len = 0;
      char before_hash[65] = "", after_hash[65] = "";
      if (provider->read_file(provider->ctx, source_path, &source, &source_len) != 0 ||
          aimee_sha256_hex(source, source_len, before_hash) != 0)
      {
         free(source);
         status(item, "unavailable", NULL);
         status_unavailable = 1;
         continue;
      }
      if (memchr(source, '\0', source_len))
      {
         free(source);
         status(item, "unsupported", "semantic context accepts text files only");
         status_unsupported = 1;
         continue;
      }

      int version = 0;
      unsigned long generation = 0;
      char error[256] = "";
      if (provider->sync(provider->ctx, provider->root, source_path, source, &version, &generation,
                         error, sizeof(error)) != 0)
      {
         free(source);
         int unavailable = strstr(error, "no LSP server configured") != NULL ||
                           strstr(error, "document limit") != NULL;
         status(item, unavailable ? "unavailable" : "stale", error);
         if (unavailable)
            status_unavailable = 1;
         else
            status_stale = 1;
         continue;
      }
      free(source);
      if (!generation || (response_generation && response_generation != generation))
      {
         status(item, "unsupported", "one context batch cannot span provider generations");
         status_unsupported = 1;
         continue;
      }
      response_generation = generation;

      lsp_location_t locations[65];
      int n = provider->query(provider->ctx, is_definition ? "definition" : "references",
                              provider->root, source_path, (int)line_j->valuedouble - 1,
                              (int)col_j->valuedouble - 1, locations, 65, error, sizeof(error));
      char *after = NULL;
      size_t after_len = 0;
      if (provider->read_file(provider->ctx, source_path, &after, &after_len) != 0 ||
          aimee_sha256_hex(after, after_len, after_hash) != 0 || strcmp(before_hash, after_hash))
      {
         cJSON *document = cJSON_AddObjectToObject(item, "document");
         cJSON_AddStringToObject(document, "path", file_j->valuestring);
         cJSON_AddNumberToObject(document, "version", version);
         cJSON_AddStringToObject(document, "content_sha256", before_hash);
         if (after && after_hash[0])
            cJSON_AddStringToObject(document, "current_content_sha256", after_hash);
         cJSON_AddStringToObject(document, "freshness", "stale");
         cJSON_AddNumberToObject(item, "provider_generation", (double)generation);
         free(after);
         status(item, "stale", NULL);
         status_stale = 1;
         continue;
      }
      free(after);

      cJSON *document = cJSON_AddObjectToObject(item, "document");
      cJSON_AddStringToObject(document, "path", file_j->valuestring);
      cJSON_AddNumberToObject(document, "version", version);
      cJSON_AddStringToObject(document, "content_sha256", before_hash);
      cJSON_AddStringToObject(document, "freshness", "current");
      cJSON_AddNumberToObject(item, "provider_generation", (double)generation);
      if (n < 0)
      {
         status(item, "unavailable", error);
         status_unavailable = 1;
         continue;
      }

      cJSON *location_array = cJSON_AddArrayToObject(item, "locations");
      int withheld = 0, source_failed = 0, source_stale = 0, truncated = n > 64;
      int emit_n = n > 64 ? 64 : n;
      for (int li = 0; li < emit_n; li++)
      {
         char target[MAX_PATH_LEN];
         if (!realpath(locations[li].file, target) || !contained(provider->root, target))
         {
            withheld = 1;
            continue;
         }
         const char *relative =
             target + strlen(provider->root) + (target[strlen(provider->root)] == '/');
         cJSON *location = cJSON_CreateObject();
         cJSON_AddStringToObject(location, "file", relative);
         cJSON_AddNumberToObject(location, "line", locations[li].line + 1);
         cJSON_AddNumberToObject(location, "column", locations[li].col + 1);
         if (source_budget > 0)
         {
            int start = locations[li].line > 4 ? locations[li].line - 3 : 1;
            cJSON *span =
                provider->source(provider->ctx, relative, start, locations[li].line + 5, 9);
            cJSON *content = span ? cJSON_GetObjectItemCaseSensitive(span, "content") : NULL;
            cJSON *source_version =
                span ? cJSON_GetObjectItemCaseSensitive(span, "source_version") : NULL;
            if (!cJSON_IsString(content))
               source_failed = 1;
            else if (!cJSON_IsString(source_version) ||
                     strcmp(source_version->valuestring, before_hash))
               source_stale = 1;
            else
            {
               size_t bytes = strlen(content->valuestring);
               if ((int)bytes > source_budget)
               {
                  content->valuestring[source_budget] = '\0';
                  truncated = 1;
               }
               source_budget -=
                   (int)(bytes > (size_t)source_budget ? (size_t)source_budget : bytes);
            }
            if (span)
               cJSON_AddItemToObject(location, "source", span);
         }
         else
            truncated = 1;
         cJSON_AddItemToArray(location_array, location);
      }

      char *final_bytes = NULL;
      size_t final_len = 0;
      char final_hash[65] = "";
      if (provider->read_file(provider->ctx, source_path, &final_bytes, &final_len) != 0 ||
          aimee_sha256_hex(final_bytes, final_len, final_hash) != 0 ||
          strcmp(before_hash, final_hash))
      {
         cJSON_DeleteItemFromObjectCaseSensitive(item, "locations");
         cJSON_ReplaceItemInObjectCaseSensitive(document, "freshness", cJSON_CreateString("stale"));
         if (final_bytes && final_hash[0])
            cJSON_AddStringToObject(document, "current_content_sha256", final_hash);
         free(final_bytes);
         status(item, "stale", NULL);
         status_stale = 1;
         continue;
      }
      free(final_bytes);
      cJSON_AddBoolToObject(item, "truncated", truncated);
      if (truncated)
         any_truncated = 1;
      if (withheld)
      {
         status(item, "unauthorized", NULL);
         status_unauthorized = 1;
      }
      else if (source_stale)
      {
         cJSON_DeleteItemFromObjectCaseSensitive(item, "locations");
         cJSON_ReplaceItemInObjectCaseSensitive(document, "freshness", cJSON_CreateString("stale"));
         status(item, "stale", "bounded source did not match the synchronized document");
         status_stale = 1;
      }
      else if (source_failed)
      {
         cJSON_DeleteItemFromObjectCaseSensitive(item, "locations");
         status(item, "unavailable", "bounded source could not be read");
         status_unavailable = 1;
      }
      else if (n == 0)
      {
         status(item, "empty", NULL);
         status_empty = 1;
      }
      else
      {
         status(item, "ok", NULL);
         status_ok = 1;
      }
   }

   const char *aggregate = status_unauthorized  ? "unauthorized"
                           : status_stale       ? "stale"
                           : status_unsupported ? "unsupported"
                           : status_unavailable ? "unavailable"
                           : status_abstained   ? "abstained"
                           : status_ok          ? "ok"
                           : status_empty       ? "empty"
                                                : "unavailable";
   cJSON_AddStringToObject(response, "status", aggregate);
   cJSON_AddBoolToObject(response, "truncated", any_truncated);
   cJSON_AddNumberToObject(response, "source_bytes", initial_source_budget - source_budget);
   if (response_generation)
      cJSON_AddNumberToObject(response, "provider_generation", (double)response_generation);
   return response;
}
