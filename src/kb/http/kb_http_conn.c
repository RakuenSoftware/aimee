/* kb_http_conn.c: per-connection handler for aimee-kb HTTP server.
 *
 * Extracted from kb_http.c to keep that file under the 2000-line limit.
 * Reads the HTTP request, extracts/generates X-Request-ID, routes via
 * kb_http_route_ex, logs the request, and writes the response. */

#include "aimee.h"
#include "kb_http.h"
#include "kb_http_ws.h"
#include "kb_verifier.h"
#include "log.h"
#include "kb/http/openapi_data.h"

/* Buffer sizes shared with kb_http.c — keep in sync. */
#ifndef KB_HTTP_READ_MAX
#define KB_HTTP_READ_MAX 4096
#endif
#ifndef KB_HTTP_RESP_MAX
#define KB_HTTP_RESP_MAX (1024 * 1024)
#endif
/* Protective request-body bound. Oversized bodies are REJECTED with 413 —
 * never silently truncated (truncation turned any large /v1/code/scan push
 * into a 400 "invalid json" with no hint of the real cause). Generous because
 * pushed-file scans of large repos are a legitimate workload; the bound only
 * guards the single-threaded listener against absurd Content-Length values. */
#ifndef KB_HTTP_BODY_MAX
#define KB_HTTP_BODY_MAX (256 * 1024 * 1024)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── externs from kb_http.c ─────────────────────────────────────────────── */

extern void write_all(int fd, const char *buf, int len);
extern void send_response(int fd, int status, const char *body);
extern void send_response_ex(int fd, int status, const char *body, const char *request_id,
                             const char *content_type);
extern char g_bearer_token[];

/* ── per-connection handler ──────────────────────────────────────────────── */

void handle_connection(int fd)
{
   char buf[KB_HTTP_READ_MAX];
   int total = 0;

   /* Read until we have the full header (blank line) or buffer exhausted */
   while (total < KB_HTTP_READ_MAX - 1)
   {
      int n = (int)read(fd, buf + total, (size_t)(KB_HTTP_READ_MAX - 1 - total));
      if (n <= 0)
         break;
      total += n;
      buf[total] = '\0';
      if (strstr(buf, "\r\n\r\n") || strstr(buf, "\n\n"))
         break;
   }
   buf[total] = '\0';

   /* Parse request line: METHOD PATH HTTP/1.x */
   char method[16] = {0};
   char path[512] = {0};
   if (sscanf(buf, "%15s %511s", method, path) < 2)
   {
      send_response(fd, 400, "{\"error\":\"bad request\"}");
      return;
   }

   /* Extract X-Request-ID header (or generate one) for response echo + logging. */
   char request_id[64] = {0};
   {
      const char *hdr_start = strstr(buf, "\r\nX-Request-ID: ");
      if (!hdr_start)
         hdr_start = strstr(buf, "\r\nx-request-id: ");
      if (hdr_start)
      {
         hdr_start = strchr(hdr_start + 2, ' ');
         if (hdr_start)
         {
            hdr_start++; /* skip space */
            const char *end = strpbrk(hdr_start, "\r\n");
            int len = end ? (int)(end - hdr_start) : (int)strlen(hdr_start);
            if (len >= (int)sizeof(request_id))
               len = (int)sizeof(request_id) - 1;
            memcpy(request_id, hdr_start, (size_t)len);
            request_id[len] = '\0';
         }
      }
      if (!request_id[0])
      {
         /* Generate a simple monotonic request-id: <pid>-<counter> */
         static unsigned int s_counter = 0;
         unsigned int cnt = __sync_add_and_fetch(&s_counter, 1);
         snprintf(request_id, sizeof(request_id), "%d-%u", (int)getpid(), cnt);
      }
   }

   /* Extract Authorization header value */
   char auth_val[512] = {0};
   const char *p = buf;
   while ((p = strstr(p, "\r\n")) != NULL)
   {
      p += 2;
      if (strncasecmp(p, "Authorization: ", 15) == 0)
      {
         const char *v = p + 15;
         const char *end = strpbrk(v, "\r\n");
         int len = end ? (int)(end - v) : (int)strlen(v);
         if (len >= (int)sizeof(auth_val))
            len = (int)sizeof(auth_val) - 1;
         memcpy(auth_val, v, (size_t)len);
         auth_val[len] = '\0';
         break;
      }
   }

   char *resp_heap = malloc(KB_HTTP_RESP_MAX);
   if (!resp_heap)
   {
      send_response(fd, 500, "{\"error\":\"oom\"}");
      return;
   }

   /* Split query string from path */
   char qs[512] = "";
   char clean_path[512] = "";
   const char *qmark = strchr(path, '?');
   if (qmark)
   {
      int plen = (int)(qmark - path);
      if (plen >= (int)sizeof(clean_path))
         plen = (int)sizeof(clean_path) - 1;
      memcpy(clean_path, path, (size_t)plen);
      clean_path[plen] = '\0';
      snprintf(qs, sizeof(qs), "%s", qmark + 1);
   }
   else
   {
      snprintf(clean_path, sizeof(clean_path), "%s", path);
   }

   /* WebSocket upgrade (Phase-2 streams): /v1/jobs/{id}/stream, /v1/events.
    * Authenticate with the same bearer gate as REST routes, then hand the
    * connection to the WS server, which owns it until the stream ends. */
   if (kb_ws_is_upgrade(buf) && kb_ws_is_ws_path(clean_path))
   {
      if (g_bearer_token[0])
      {
         const char *presented =
             (auth_val[0] && strncmp(auth_val, "Bearer ", 7) == 0) ? auth_val + 7 : "";
         kb_verify_result_t vr;
         if (!kb_verifier_authenticate(presented, g_bearer_token, &vr, NULL, 0))
         {
            aimee_log(LOG_INFO, "kb.http", "request_id=%s method=%s path=%s status=401 (ws)",
                      request_id, method, clean_path);
            send_response(fd, 401, "{\"error\":\"unauthorized\"}");
            return;
         }
      }
      aimee_log(LOG_INFO, "kb.http", "request_id=%s method=%s path=%s status=101 (ws-upgrade)",
                request_id, method, clean_path);
      /* Hand off to a detached thread (dup'd fd): the single-threaded listener
       * must keep serving other requests while the stream is open. */
      kb_ws_spawn(fd, buf, clean_path);
      return;
   }

   /* Extract request body via Content-Length; use heap for large uploads */
   char *req_body_heap = NULL;
   int req_body_len = 0;
   const char *req_body_ptr = NULL;
   const char *cl_hdr = strstr(buf, "\r\nContent-Length: ");
   if (cl_hdr)
   {
      cl_hdr += 18;
      long cl = atol(cl_hdr);
      if (cl < 0)
         cl = 0;
      if (cl > KB_HTTP_BODY_MAX)
      {
         free(resp_heap);
         aimee_log(LOG_WARN, "kb.http",
                   "request_id=%s method=%s path=%s status=413 (body %ld > %d)", request_id, method,
                   clean_path, cl, KB_HTTP_BODY_MAX);
         send_response_ex(fd, 413, "{\"error\":\"request body too large\"}", request_id, NULL);
         return;
      }
      req_body_len = (int)cl;
      req_body_heap = malloc((size_t)req_body_len + 1);
      if (req_body_heap)
      {
         const char *body_start = strstr(buf, "\r\n\r\n");
         int already = 0;
         if (body_start)
         {
            body_start += 4;
            already = (int)(buf + total - body_start);
            if (already < 0)
               already = 0;
            if (already > req_body_len)
               already = req_body_len;
            if (already > 0)
               memcpy(req_body_heap, body_start, (size_t)already);
         }
         int remaining = req_body_len - already;
         while (remaining > 0)
         {
            int n = (int)read(fd, req_body_heap + already, (size_t)remaining);
            if (n <= 0)
               break;
            already += n;
            remaining -= n;
         }
         req_body_heap[already] = '\0';
         req_body_len = already;
         req_body_ptr = req_body_heap;
      }
   }

   int status =
       kb_http_route_ex(method, clean_path, qs[0] ? qs : NULL, auth_val[0] ? auth_val : NULL,
                        g_bearer_token[0] ? g_bearer_token : NULL, req_body_ptr, req_body_len,
                        resp_heap, KB_HTTP_RESP_MAX);

   aimee_log(LOG_INFO, "kb.http", "request_id=%s method=%s path=%s status=%d", request_id, method,
             clean_path, status);

   if (strcmp(method, "HEAD") == 0)
      resp_heap[0] = '\0';

   /* Serve OpenAPI spec with YAML content-type; all other routes use JSON. */
   const char *ct =
       (strcmp(clean_path, "/v1/openapi.yaml") == 0) ? "application/yaml" : "application/json";
   send_response_ex(fd, status, resp_heap, request_id, ct);
   free(req_body_heap);
   free(resp_heap);
}
