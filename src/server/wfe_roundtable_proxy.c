#include "wfe_roundtable_proxy.h"

#include "cJSON.h"
#include "json_fluent.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int handle_roundtable_review_proxy(server_ctx_t *ctx, server_conn_t *conn, cJSON *request)
{
   (void)ctx;
   return wfe_roundtable_proxy(conn, request);
}

#ifdef _WIN32
int wfe_roundtable_proxy(server_conn_t *conn, const cJSON *request)
{
   (void)request;
   return server_send_error(conn, "Go roundtable transport is unavailable on this platform", NULL);
}
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int write_all(int fd, const char *data, size_t size)
{
   while (size > 0)
   {
      ssize_t n = write(fd, data, size);
      if (n <= 0)
         return -1;
      data += n;
      size -= (size_t)n;
   }
   return 0;
}

static char *decode_chunked(const char *body)
{
   size_t cap = strlen(body) + 1, used = 0;
   char *out = malloc(cap);
   if (!out)
      return NULL;
   const char *p = body;
   for (;;)
   {
      char *end = NULL;
      unsigned long chunk = strtoul(p, &end, 16);
      if (!end || end == p || end[0] != '\r' || end[1] != '\n')
      {
         free(out);
         return NULL;
      }
      p = end + 2;
      if (chunk == 0)
         break;
      if (chunk > strlen(p) || used + chunk >= cap)
      {
         free(out);
         return NULL;
      }
      memcpy(out + used, p, chunk);
      used += chunk;
      p += chunk;
      if (p[0] != '\r' || p[1] != '\n')
      {
         free(out);
         return NULL;
      }
      p += 2;
   }
   out[used] = '\0';
   return out;
}

static char *post_go_roundtable(const char *body, int *status)
{
   if (status)
      *status = 0;
   const char *socket_path = getenv("AIMEE_WFE_HTTP_SOCKET");
   if (!socket_path || !socket_path[0])
      return NULL;
   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (fd < 0)
      return NULL;
   struct sockaddr_un addr;
   memset(&addr, 0, sizeof addr);
   addr.sun_family = AF_UNIX;
   if (snprintf(addr.sun_path, sizeof addr.sun_path, "%s", socket_path) >=
           (int)sizeof addr.sun_path ||
       connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0)
   {
      close(fd);
      return NULL;
   }
   char head[512];
   int head_n = snprintf(head, sizeof head,
                         "POST /v1/roundtable/review HTTP/1.1\r\nHost: aimee\r\n"
                         "Content-Type: application/json\r\nContent-Length: %zu\r\n"
                         "Connection: close\r\n\r\n",
                         strlen(body));
   if (head_n <= 0 || head_n >= (int)sizeof head || write_all(fd, head, (size_t)head_n) != 0 ||
       write_all(fd, body, strlen(body)) != 0)
   {
      close(fd);
      return NULL;
   }
   size_t cap = 8192, used = 0;
   char *raw = malloc(cap);
   if (!raw)
   {
      close(fd);
      return NULL;
   }
   for (;;)
   {
      if (used + 4097 > cap)
      {
         cap *= 2;
         char *grown = realloc(raw, cap);
         if (!grown)
         {
            free(raw);
            close(fd);
            return NULL;
         }
         raw = grown;
      }
      ssize_t n = read(fd, raw + used, cap - used - 1);
      if (n <= 0)
         break;
      used += (size_t)n;
   }
   close(fd);
   raw[used] = '\0';
   int http_status = 0;
   if (sscanf(raw, "HTTP/1.%*d %d", &http_status) != 1)
   {
      free(raw);
      return NULL;
   }
   if (status)
      *status = http_status;
   char *body_at = strstr(raw, "\r\n\r\n");
   if (!body_at)
   {
      free(raw);
      return NULL;
   }
   body_at += 4;
   char *out =
       strstr(raw, "Transfer-Encoding: chunked") ? decode_chunked(body_at) : strdup(body_at);
   free(raw);
   return out;
}

int wfe_roundtable_proxy(server_conn_t *conn, const cJSON *request)
{
   cJSON *payload = cJSON_CreateObject();
   cJSON *prompt = cJSON_GetObjectItemCaseSensitive(request, "prompt");
   cJSON *brief = cJSON_GetObjectItemCaseSensitive(request, "brief");
   cJSON *preset = cJSON_GetObjectItemCaseSensitive(request, "roundtable");
   cJSON *run_id = cJSON_GetObjectItemCaseSensitive(request, "__run_id");
   cJSON *original = cJSON_GetObjectItemCaseSensitive(request, "original_request");
   cJSON *stage = cJSON_GetObjectItemCaseSensitive(request, "artifact_stage");
   cJSON *requested_workdir = cJSON_GetObjectItemCaseSensitive(request, "workdir");
   if (!payload || !cJSON_IsString(prompt))
   {
      cJSON_Delete(payload);
      return server_send_error(conn, "roundtable artifact is required", NULL);
   }
   cJSON_AddStringToObject(payload, "artifact", prompt->valuestring);
   cJSON_AddStringToObject(payload, "artifact_stage",
                           cJSON_IsString(stage) ? stage->valuestring : "frozen_diff");
   if (cJSON_IsString(original))
      cJSON_AddStringToObject(payload, "original_request", original->valuestring);
   else if (cJSON_IsString(brief))
      cJSON_AddStringToObject(payload, "original_request", brief->valuestring);
   else if (cJSON_IsObject(brief))
   {
      char *text = cJSON_PrintUnformatted(brief);
      if (text)
      {
         cJSON_AddStringToObject(payload, "original_request", text);
         free(text);
      }
   }
   if (cJSON_IsString(preset))
      cJSON_AddStringToObject(payload, "roundtable", preset->valuestring);
   if (cJSON_IsString(run_id))
      cJSON_AddStringToObject(payload, "run_id", run_id->valuestring);
   const char *cwd =
       cJSON_IsString(requested_workdir) ? requested_workdir->valuestring : run_cmd_get_cwd();
   if (cwd && cwd[0])
      cJSON_AddStringToObject(payload, "workdir", cwd);

   char *wire = cJSON_PrintUnformatted(payload);
   cJSON_Delete(payload);
   if (!wire)
      return server_send_error(conn, "out of memory", NULL);
   int status = 0;
   char *body = post_go_roundtable(wire, &status);
   free(wire);
   if (!body)
      return server_send_error(conn, "Go roundtable service is unreachable", NULL);
   cJSON *response = cJSON_Parse(body);
   free(body);
   cJSON *error = response ? cJSON_GetObjectItemCaseSensitive(response, "error") : NULL;
   if (status < 200 || status >= 300 || !response)
   {
      int rc = server_send_error(
          conn, cJSON_IsString(error) ? error->valuestring : "Go roundtable request failed", NULL);
      cJSON_Delete(response);
      return rc;
   }
   cJSON *result = cJSON_DetachItemFromObject(response, "roundtable");
   cJSON_Delete(response);
   if (!result)
      return server_send_error(conn, "Go roundtable returned no result", NULL);
   cJSON_AddBoolToObject(result, "ok", 1);
   return server_send_ok(conn, result);
}
#endif
