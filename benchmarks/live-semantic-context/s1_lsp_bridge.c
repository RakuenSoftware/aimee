/* Benchmark-only NDJSON bridge over the production LSP manager/context objects.
 *
 * This file deliberately lives outside src/: it is experiment instrumentation,
 * not another product surface. The build harness links it to the exact candidate
 * objects already used by unit-test-lsp and verifies the candidate src Git tree
 * before dispatching any model cell.
 */
#define _XOPEN_SOURCE 700

#include "aimee_sha256.h"
#include "cJSON.h"
#include "config_client.h"
#include "lsp.h"
#include "lsp_context.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct
{
   char root[PATH_MAX];
   char project[80];
   char worktree[80];
} bridge_context_t;

static int configure_provider(const char *command, const char *server_arg, const char *extension)
{
   cJSON *servers = cJSON_CreateArray();
   cJSON *server = cJSON_CreateObject();
   cJSON *args = cJSON_CreateArray();
   cJSON *extensions = cJSON_CreateArray();
   if (!servers || !server || !args || !extensions ||
       !cJSON_AddStringToObject(server, "name", "s1-pinned-provider") ||
       !cJSON_AddStringToObject(server, "command", command) ||
       (server_arg && !cJSON_AddItemToArray(args, cJSON_CreateString(server_arg))) ||
       !cJSON_AddItemToArray(extensions, cJSON_CreateString(extension)))
   {
      cJSON_Delete(servers);
      cJSON_Delete(server);
      cJSON_Delete(args);
      cJSON_Delete(extensions);
      return -1;
   }
   cJSON_AddItemToObject(server, "args", args);
   cJSON_AddItemToObject(server, "extensions", extensions);
   cJSON_AddItemToArray(servers, server);
   if (config_client_set_value("lsp_servers", servers) != 0)
      return -1;
   return config_client_set_number("lsp_server_count", 1.0);
}

static int contained(const char *root, const char *path)
{
   size_t n = strlen(root);
   return !strncmp(root, path, n) && (path[n] == '/' || path[n] == '\0');
}

static int read_file(const char *path, char **out, size_t *out_len)
{
   *out = NULL;
   *out_len = 0;
   FILE *fp = fopen(path, "rb");
   if (!fp || fseek(fp, 0, SEEK_END) != 0)
   {
      if (fp)
         fclose(fp);
      return -1;
   }
   long size = ftell(fp);
   if (size < 0 || size > 4 * 1024 * 1024 || fseek(fp, 0, SEEK_SET) != 0)
   {
      fclose(fp);
      return -1;
   }
   char *bytes = malloc((size_t)size + 1);
   if (!bytes || fread(bytes, 1, (size_t)size, fp) != (size_t)size)
   {
      free(bytes);
      fclose(fp);
      return -1;
   }
   fclose(fp);
   bytes[size] = '\0';
   *out = bytes;
   *out_len = (size_t)size;
   return 0;
}

static int context_authorize(void *opaque, const char *relative, char *resolved, size_t cap)
{
   bridge_context_t *ctx = opaque;
   char joined[PATH_MAX];
   if (!relative || relative[0] == '/' || strchr(relative, '\\') ||
       (size_t)snprintf(joined, sizeof(joined), "%s/%s", ctx->root, relative) >= sizeof(joined) ||
       !realpath(joined, resolved) || strlen(resolved) >= cap || !contained(ctx->root, resolved))
      return -1;
   return 0;
}

static int context_read(void *opaque, const char *path, char **out, size_t *out_len)
{
   bridge_context_t *ctx = opaque;
   return contained(ctx->root, path) ? read_file(path, out, out_len) : -1;
}

static int context_sync(void *opaque, const char *workspace, const char *file, const char *text,
                        int *version, unsigned long *generation, char *error, size_t error_size)
{
   (void)opaque;
   return lsp_manager_sync_document(workspace, file, text, version, generation, error, error_size);
}

static int context_query(void *opaque, const char *operation, const char *workspace,
                         const char *file, int line, int column, lsp_location_t *out, int max,
                         char *error, size_t error_size)
{
   (void)opaque;
   if (!strcmp(operation, "definition"))
      return lsp_manager_definition(workspace, file, line, column, out, max, error, error_size);
   return lsp_manager_references(workspace, file, line, column, out, max, error, error_size);
}

static cJSON *context_source(void *opaque, const char *relative, int start, int end, int max_lines)
{
   bridge_context_t *ctx = opaque;
   char path[PATH_MAX];
   if (context_authorize(ctx, relative, path, sizeof(path)) != 0)
      return NULL;
   char *whole = NULL;
   size_t whole_len = 0;
   if (read_file(path, &whole, &whole_len) != 0 || memchr(whole, '\0', whole_len))
   {
      free(whole);
      return NULL;
   }
   char digest[65];
   if (aimee_sha256_hex(whole, whole_len, digest) != 0)
   {
      free(whole);
      return NULL;
   }

   if (start < 1)
      start = 1;
   if (end < start)
      end = start;
   if (max_lines > 0 && end - start + 1 > max_lines)
      end = start + max_lines - 1;
   size_t begin = 0, finish = whole_len;
   int line = 1;
   for (size_t i = 0; i < whole_len; i++)
   {
      if (line == start)
      {
         begin = i;
         break;
      }
      if (whole[i] == '\n')
         line++;
   }
   line = start;
   for (size_t i = begin; i < whole_len; i++)
   {
      if (whole[i] == '\n' && line++ >= end)
      {
         finish = i + 1;
         break;
      }
   }
   char *span = malloc(finish - begin + 1);
   if (!span)
   {
      free(whole);
      return NULL;
   }
   memcpy(span, whole + begin, finish - begin);
   span[finish - begin] = '\0';
   free(whole);

   cJSON *result = cJSON_CreateObject();
   cJSON_AddStringToObject(result, "path", relative);
   cJSON_AddNumberToObject(result, "start_line", start);
   cJSON_AddNumberToObject(result, "end_line", line > end ? end : line);
   cJSON_AddStringToObject(result, "content", span);
   cJSON_AddStringToObject(result, "source_version", digest);
   free(span);
   return result;
}

static cJSON *error_result(const char *status, const char *reason)
{
   cJSON *result = cJSON_CreateObject();
   cJSON_AddStringToObject(result, "status", status);
   if (reason && reason[0])
      cJSON_AddStringToObject(result, "reason", reason);
   return result;
}

static cJSON *location_result(const char *operation, bridge_context_t *ctx, const cJSON *request)
{
   cJSON *anchors = cJSON_GetObjectItemCaseSensitive(request, "anchors");
   if (!cJSON_IsArray(anchors) || cJSON_GetArraySize(anchors) != 1)
      return error_result("abstained", "location-only requests require exactly one anchor");
   cJSON *anchor = cJSON_GetArrayItem(anchors, 0);
   cJSON *file = cJSON_GetObjectItemCaseSensitive(anchor, "file");
   cJSON *line = cJSON_GetObjectItemCaseSensitive(anchor, "line");
   cJSON *column = cJSON_GetObjectItemCaseSensitive(anchor, "column");
   if (!cJSON_IsString(file) || !cJSON_IsNumber(line) || !cJSON_IsNumber(column) ||
       line->valuedouble < 1 || column->valuedouble < 1)
      return error_result("abstained", "a one-based file, line, and column are required");
   char absolute[PATH_MAX];
   if (context_authorize(ctx, file->valuestring, absolute, sizeof(absolute)) != 0)
      return error_result("unauthorized", "anchor is outside the checked worktree");

   lsp_location_t locations[65];
   char error[256] = "";
   int count = !strcmp(operation, "definition")
                   ? lsp_manager_definition(ctx->root, absolute, (int)line->valuedouble - 1,
                                            (int)column->valuedouble - 1, locations, 65, error,
                                            sizeof(error))
                   : lsp_manager_references(ctx->root, absolute, (int)line->valuedouble - 1,
                                            (int)column->valuedouble - 1, locations, 65, error,
                                            sizeof(error));
   if (count < 0)
      return error_result("unavailable", error);
   cJSON *result = cJSON_CreateObject();
   cJSON_AddStringToObject(result, "status", count ? "ok" : "empty");
   cJSON *items = cJSON_AddArrayToObject(result, "locations");
   for (int i = 0; i < count && i < 65; i++)
   {
      char target[PATH_MAX];
      if (!realpath(locations[i].file, target) || !contained(ctx->root, target))
      {
         cJSON_Delete(result);
         return error_result("unauthorized", "provider returned a path outside the worktree");
      }
      const char *relative = target + strlen(ctx->root) + (target[strlen(ctx->root)] == '/');
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "file", relative);
      cJSON_AddNumberToObject(item, "line", locations[i].line + 1);
      cJSON_AddNumberToObject(item, "column", locations[i].col + 1);
      cJSON_AddItemToArray(items, item);
   }
   return result;
}

static cJSON *handle_request(const cJSON *request)
{
   cJSON *workspace = cJSON_GetObjectItemCaseSensitive(request, "workspace");
   cJSON *operation = cJSON_GetObjectItemCaseSensitive(request, "operation");
   if (!cJSON_IsString(workspace) || !cJSON_IsString(operation))
      return error_result("abstained", "workspace and operation are required");
   bridge_context_t ctx = {0};
   if (!realpath(workspace->valuestring, ctx.root))
      return error_result("unavailable", "workspace does not resolve");
   char digest[65];
   if (aimee_sha256_hex(ctx.root, strlen(ctx.root), digest) != 0)
      return error_result("unavailable", "could not identify worktree authority");
   snprintf(ctx.project, sizeof(ctx.project), "s1:%.*s", 16, digest);
   snprintf(ctx.worktree, sizeof(ctx.worktree), "local:%s", digest);

   if (!strcmp(operation->valuestring, "definition") ||
       !strcmp(operation->valuestring, "references"))
      return location_result(operation->valuestring, &ctx, request);
   if (strcmp(operation->valuestring, "context"))
      return error_result("abstained", "unknown operation");

   lsp_context_provider_t provider = {.provider = "local_lsp",
                                      .root = ctx.root,
                                      .project = ctx.project,
                                      .worktree = ctx.worktree,
                                      .ctx = &ctx,
                                      .authorize = context_authorize,
                                      .read_file = context_read,
                                      .sync = context_sync,
                                      .query = context_query,
                                      .source = context_source};
   cJSON *args = cJSON_Duplicate(request, 1);
   cJSON_ReplaceItemInObjectCaseSensitive(args, "operation", cJSON_CreateString("definition"));
   cJSON *semantic_operation = cJSON_GetObjectItemCaseSensitive(request, "semantic_operation");
   if (cJSON_IsString(semantic_operation))
      cJSON_ReplaceItemInObjectCaseSensitive(args, "operation",
                                             cJSON_Duplicate(semantic_operation, 1));
   cJSON *result = lsp_context_execute(&provider, args);
   cJSON_Delete(args);
   return result ? result : error_result("unavailable", "context allocation failed");
}

int main(int argc, char **argv)
{
   if (argc != 4)
   {
      fprintf(stderr, "usage: %s <provider-command> <arg-or-dash> <extension>\n", argv[0]);
      return 2;
   }
   if (configure_provider(argv[1], strcmp(argv[2], "-") ? argv[2] : NULL, argv[3]) != 0)
   {
      fprintf(stderr, "could not configure the pinned provider\n");
      return 2;
   }
   lsp_manager_init();
   char *line = NULL;
   size_t capacity = 0;
   while (getline(&line, &capacity, stdin) >= 0)
   {
      cJSON *request = cJSON_Parse(line);
      cJSON *result = request ? handle_request(request)
                              : error_result("abstained", "request is not valid JSON");
      char *rendered = cJSON_PrintUnformatted(result);
      if (rendered)
      {
         printf("%s\n", rendered);
         fflush(stdout);
         free(rendered);
      }
      cJSON_Delete(result);
      cJSON_Delete(request);
   }
   free(line);
   lsp_manager_shutdown_all();
   return 0;
}
