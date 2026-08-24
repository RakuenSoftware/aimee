/* kb_http_conn.c: per-connection handler for aimee-kb HTTP server.
 *
 * Extracted from kb_http.c to keep that file under the 2000-line limit.
 * Reads the HTTP request, extracts/generates X-Request-ID, routes via
 * kb_http_route_ex, logs the request, and writes the response. */

#include "aimee.h"
#include "kb_http.h"
#include "kb_http_ws.h"
#include "kb_verifier.h"
#include "kb_ingress.h"
#include "kb_reqctx.h"
#include "log.h"
#include "kb/http/openapi_data.h"
#include <aimee/core/connection/auth.h>

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

/* Copies the value of header `name` at `line` into `out`, if that is the header
 * this line carries. RFC 9110: the colon may be followed by zero or more spaces
 * or tabs, and trailing whitespace is not part of the value. Returns 1 on a
 * match (even for an empty value), 0 when the line is a different header. */
/* Returns 1 when the header matched and FITS, 0 when the line is a different
 * header, and -1 when it matched but was too long for `out`.
 *
 * The over-long case used to be indistinguishable from a clean parse: the value
 * was silently truncated to out_cap-1 and returned 1. For Authorization that
 * turned a valid credential into a mangled one, which then failed verification
 * and was reported as `unauthorized` -- a credential error for what is really a
 * request the server declined to read in full. The TLS front end already answers
 * 400 for this; this one could not, because it had no way to say so. */
static int header_value(const char *line, const char *name, char *out, size_t out_cap)
{
   size_t nlen = strlen(name);
   if (strncasecmp(line, name, nlen) != 0 || line[nlen] != ':')
      return 0;
   const char *v = line + nlen + 1;
   while (*v == ' ' || *v == '\t')
      v++;
   const char *end = strpbrk(v, "\r\n");
   if (!end)
      end = v + strlen(v);
   while (end > v && (end[-1] == ' ' || end[-1] == '\t'))
      end--;
   size_t len = (size_t)(end - v);
   if (len >= out_cap)
   {
      out[0] = '\0';
      return -1;
   }
   memcpy(out, v, len);
   out[len] = '\0';
   return 1;
}

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

   /* B5: kb never honors a client-supplied identity header; reject fail-closed. */
   if (kb_ingress_identity_header_present(buf))
   {
      LOG_WARN("kb.http",
               "kb ingress: rejected request bearing a spoofable X-Aimee-* identity header");
      send_response(fd, 400, "{\"error\":\"identity header not permitted\"}");
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

   /* Extract Authorization and Content-Type header values. Content-Type is
    * carried in the per-request context (kb_reqctx.h) rather than a new router
    * parameter: only the login route's security depends on it.
    *
    * RFC 9110 allows ZERO OR MORE spaces or tabs after the colon, so
    * "Content-Type:application/json" and a tab-separated form are both valid.
    * Matching only "Name: " with exactly one space treats those as ABSENT, which
    * for a route that requires JSON turns a legitimate request into a 415. The
    * TLS front end already parsed optional whitespace; this one did not, and the
    * inconsistency was invisible because the route-level tests bypass both. */
   /* Sized for a real OIDC bearer, not a hand-issued token. At 512 this silently
    * truncated every RS256 JWT an IdP actually mints: a 2048-bit signature is 342
    * base64url characters on its own, and ordinary claims (nbf, azp, scope,
    * email, name) push a routine token past 700 bytes. Measured on the box, same
    * key and same JWKS both times: a 338-byte token authenticated, a 509-byte one
    * did not.
    *
    * The consequence was not a slow path or a warning. OIDC bearer auth over this
    * listener was unusable for essentially every real token, and it failed as
    * `unauthorized` -- so it read as a rejected credential rather than a request
    * that was never read in full. aimee-server has always sized its own bearer
    * buffer for this (server_http_identity.c, tl_bearer[4097]); this side had
    * not. */
   char auth_val[4097] = {0};
   char ctype_val[128] = {0};
   int auth_too_long = 0;
   const char *p = buf;
   while ((p = strstr(p, "\r\n")) != NULL)
   {
      p += 2;
      if (!auth_val[0] && !auth_too_long &&
          header_value(p, "Authorization", auth_val, sizeof(auth_val)) < 0)
         auth_too_long = 1;
      if (!ctype_val[0])
         (void)header_value(p, "Content-Type", ctype_val, sizeof(ctype_val));
      if ((auth_val[0] || auth_too_long) && ctype_val[0])
         break;
   }
   /* Say so, rather than letting a truncated credential fail as `unauthorized`.
    * This is what the TLS front end already answers for the same condition. */
   if (auth_too_long)
   {
      aimee_log(LOG_WARN, "kb.http", "Authorization header too long for this listener");
      send_response(fd, 400, "{\"error\":\"authorization header too long\"}");
      return;
   }
   kb_reqctx_set_content_type(ctype_val);

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
         const char *presented = aimee_core_bearer_token(auth_val);
         if (!presented)
            presented = "";
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
   kb_reqctx_clear(); /* drop the request's actor before the worker handles the next */
   /* Same for the content type: a worker thread is reused, and a stale JSON
    * content type left behind would let the NEXT request past a check that its
    * own headers should have failed. */
   kb_reqctx_clear_content_type();

   aimee_log(LOG_INFO, "kb.http", "request_id=%s method=%s path=%s status=%d", request_id, method,
             clean_path, status);

   if (strcmp(method, "HEAD") == 0)
      resp_heap[0] = '\0';

   /* Serve OpenAPI spec with YAML content-type; the Prometheus export as its text
    * exposition format (0.0.4); all other routes use JSON. GET /v1/metrics is the
    * P9a scrape endpoint. On an error status it still carries a JSON error body, so
    * keep JSON unless the request succeeded. */
   const char *ct;
   if (strcmp(clean_path, "/v1/openapi.yaml") == 0)
      ct = "application/yaml";
   else if (strcmp(clean_path, "/v1/metrics") == 0 && status >= 200 && status < 300)
      ct = "text/plain; version=0.0.4; charset=utf-8";
   else
      ct = "application/json";
   send_response_ex(fd, status, resp_heap, request_id, ct);
   free(req_body_heap);
   free(resp_heap);
}
