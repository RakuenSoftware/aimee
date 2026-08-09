/* http_uds_client.c: minimal HTTP/1.1 client over aimee-server's /v1 UDS. */
#include "http_uds_client.h"
#include "aimee_home.h"
#include "cJSON.h"
#include "cli_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* The Windows client has no AF_UNIX local-socket transport: it reaches the
 * server over the TCP/HTTP transport instead (AIMEE_API_ENDPOINT=tcp:...). This
 * stub lets the shared CLI link; the UDS path is simply never taken on Windows. */
char *http_uds_request(const char *method, const char *path, const char *body, int *status_out)
{
   (void)method;
   (void)path;
   (void)body;
   if (status_out)
      *status_out = -1;
   return NULL;
}

/* tokenize_for_search lives in text.c, which the thin client does not link. Its
 * only client-side reference is the unreachable is_noise_utterance() in the
 * shared util.c — dead code the MinGW linker fails to drop even with
 * -ffunction-sections + --gc-sections. Provide a never-called stub here (this TU
 * is client-only, so it cannot collide with the real text.c in the server/kb). */
int tokenize_for_search(const char *text, char **out, int max_tokens)
{
   (void)text;
   (void)out;
   (void)max_tokens;
   return 0;
}
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

char *http_uds_request(const char *method, const char *path, const char *body, int *status_out)
{
   if (status_out)
      *status_out = 0;
   if (!method || !path)
      return NULL;

   char sock_path[512];
   snprintf(sock_path, sizeof(sock_path), "%s/aimee-http.sock", aimee_home());

   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (fd < 0)
      return NULL;
   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;
   snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
   if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
   {
      close(fd);
      return NULL;
   }

   int blen = body ? (int)strlen(body) : 0;
   char head[512];
   int hlen = snprintf(head, sizeof(head),
                       "%s %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
                       "Content-Length: %d\r\nConnection: close\r\n\r\n",
                       method, path, blen);
   /* write head + body */
   int off = 0;
   while (off < hlen)
   {
      int n = (int)write(fd, head + off, (size_t)(hlen - off));
      if (n <= 0)
      {
         close(fd);
         return NULL;
      }
      off += n;
   }
   off = 0;
   while (off < blen)
   {
      int n = (int)write(fd, body + off, (size_t)(blen - off));
      if (n <= 0)
      {
         close(fd);
         return NULL;
      }
      off += n;
   }

   /* read full response (Connection: close) */
   size_t cap = 8192, len = 0;
   char *resp = malloc(cap);
   if (!resp)
   {
      close(fd);
      return NULL;
   }
   for (;;)
   {
      if (len + 4096 > cap)
      {
         cap *= 2;
         char *grown = realloc(resp, cap);
         if (!grown)
         {
            free(resp);
            close(fd);
            return NULL;
         }
         resp = grown;
      }
      int n = (int)read(fd, resp + len, 4096);
      if (n <= 0)
         break;
      len += (size_t)n;
   }
   close(fd);
   resp[len] = '\0';

   /* parse status + body */
   int status = 0;
   if (sscanf(resp, "HTTP/1.%*d %d", &status) != 1)
   {
      free(resp);
      return NULL;
   }
   if (status_out)
      *status_out = status;
   char *bstart = strstr(resp, "\r\n\r\n");
   char *out = bstart ? strdup(bstart + 4) : strdup("");
   free(resp);
   return out;
}
#endif /* !_WIN32 */

/* Transport-selecting wrapper (see http_uds_client.h). Shared by both platforms:
 * on Windows the UDS branch is the stub above and only the remote branch can
 * succeed, which is exactly right -- that client always has a remote endpoint.
 *
 * The response is re-serialized from the parsed JSON rather than passed through
 * verbatim, because cli_http_request() hands back a cJSON tree, not the raw
 * body. Every /v1 path routed here answers JSON, and the callers either parse it
 * or print it, so compact re-serialization is equivalent. */
char *cli_v1_path_request(const char *method, const char *path, const char *body, int *status_out)
{
   if (status_out)
      *status_out = 0;
   if (!cli_v1_has_remote_endpoint())
      return http_uds_request(method, path, body, status_out);

   char *endpoint = cli_v1_client_endpoint();
   if (!endpoint)
      return NULL;
   char *bearer = cli_v1_client_bearer();
   int status = 0;
   cJSON *resp = cli_http_request(endpoint, method, path, body, bearer, 30000, &status);
   free(endpoint);
   free(bearer);
   if (status_out)
      *status_out = status;
   if (!resp)
      return NULL;
   char *out = cJSON_PrintUnformatted(resp);
   cJSON_Delete(resp);
   return out;
}
