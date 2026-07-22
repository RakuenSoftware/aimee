/* server_http_response.c: split from server_http.c into a real translation unit
 * (was server_http_response.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#ifndef _GNU_SOURCE /* strcasestr/memmem are GNU extensions (container gcc) */
#define _GNU_SOURCE
#endif
#include "server_http_internal.h"
#include "server_http.h"
#include "server.h"         /* CAP_* / CAPS_* capability bits, server_capability_for_method */
#include "server_conn_io.h" /* transport-aware fd I/O (native-TLS phase 1) */
#include "server_tls.h"     /* native TLS termination (phase 1b) */
#include "workspace_runner_registry.h" /* ws_runner_registry_poll/_respond for the /v1 reverse channel */
#include "forge_credentials.h"         /* forge_cred_install for the /v1 token-install route */
#include <time.h>
#include "persona.h"
#include "role_templates.h"
#include "util.h" /* safe_strdup, aimee_base64_* */
#include "cli_session_pty.h"
#include "config.h"
#include "prompts.h"
#include "delegate_role.h"
#include "log.h"
#include "aimee_version.h"
#include "openai_shape.h"
#include "ingress_preinject.h"
#include "openapi_server_data.h" /* AIMEE_OPENAPI_SERVER_YAML_STR (generated from api/openapi-server-v1.yaml) */
#include "openai_runs_store.h"
#include "roundtable_pipeline_capture.h" /* pipeline op-run capture seam (#18/#20) */
#include "presence.h"
#include "request_context.h"
#include "server_http_identity.h" /* WP-C.0 attested-identity capture/threading */
#include "http_content_encoding.h"
#include "server_workflow_api.h" /* W7: /v1/workflow read+author handlers */
#include "cJSON.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>

/* Build the optional `X-Request-ID: <id>\r\n` header line into dst (or ""). */
void request_id_header(char *dst, size_t n, const char *request_id)
{
   if (request_id && request_id[0])
      snprintf(dst, n, "X-Request-ID: %s\r\n", request_id);
   else if (n)
      dst[0] = '\0';
}

/* Auditable-correctness P1: build the optional `X-Aimee-Retrieval-Event:
 * <turn_id>\r\n` header line (or "") from the ingress thread-local. Non-empty
 * only when a turn id was minted for this request (OpenAI-family ingress with
 * kb_evidence_emit_enabled on), so it matches the id keyed onto the emitted
 * retrieval_event; clients use it to query /v1/audit/trace. */
void retrieval_event_header(char *dst, size_t n)
{
   const char *tid = ingress_preinject_turn_id();
   if (tid && tid[0])
      snprintf(dst, n, "X-Aimee-Retrieval-Event: %s\r\n", tid);
   else if (n)
      dst[0] = '\0';
}

/* Build the optional `Retry-After: <seconds>\r\n` header line (or "") from the
 * Anthropic ingress thread-local — non-empty only when the parity passthrough
 * relayed an upstream 429/529 that carried a Retry-After, so the client's SDK
 * backs off exactly as it would against api.anthropic.com. */
static void retry_after_header(char *dst, size_t n)
{
   int ra = anthropic_http_response_retry_after();
   if (ra > 0)
      snprintf(dst, n, "Retry-After: %d\r\n", ra);
   else if (n)
      dst[0] = '\0';
}

void send_response(int fd, int status, const char *body, const char *request_id)
{
   const char *reason = status == 200   ? "OK"
                        : status == 400 ? "Bad Request"
                        : status == 401 ? "Unauthorized"
                        : status == 403 ? "Forbidden"
                        : status == 404 ? "Not Found"
                        : status == 410 ? "Gone"
                        : status == 413 ? "Payload Too Large"
                        : status == 415 ? "Unsupported Media Type"
                        : status == 429 ? "Too Many Requests"
                        : status == 500 ? "Internal Server Error"
                        : status == 503 ? "Service Unavailable"
                                        : "OK";
   char rid[96];
   request_id_header(rid, sizeof(rid), request_id);
   char reh[80];
   retrieval_event_header(reh, sizeof(reh));
   char rah[48];
   retry_after_header(rah, sizeof(rah));
   char head[560];
   size_t blen = body ? strlen(body) : 0;
   unsigned char *compressed = NULL;
   size_t wire_len = blen;
   const void *wire_body = body;
   if (server_http_gzip_peek() && blen >= 4096 &&
       http_gzip_compress(body, blen, &compressed, &wire_len) == 0 && wire_len < blen &&
       (blen <= 1024 || (blen - 1024 + 49) / 50 <= wire_len))
      wire_body = compressed;
   else
   {
      free(compressed);
      compressed = NULL;
      wire_len = blen;
   }
   const char *encoding = compressed ? "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n" : "";
   const char *request_encoding =
       server_http_gzip_peek() ? "Accept-Request-Encoding: gzip\r\n" : "";
   int hlen = snprintf(head, sizeof(head),
                       "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                       "Content-Length: %zu\r\n%s%s%s%s%sConnection: %s\r\n\r\n",
                       status, reason, wire_len, rid, reh, rah, encoding, request_encoding,
                       server_http_keepalive_peek() ? "keep-alive" : "close");
   write_all_fd(fd, head, hlen);
   if (wire_len > 0)
      write_all_fd(fd, wire_body, (int)wire_len);
   free(compressed);
}

/* 429 Too Many Requests with a Retry-After header (seconds until the rate
 * window resets). */
void send_rate_limited(int fd, int retry_after, const char *request_id)
{
   const char *body =
       "{\"error\":{\"message\":\"rate limit exceeded\",\"type\":\"rate_limit_error\"}}";
   int blen = (int)strlen(body);
   char rid[96];
   request_id_header(rid, sizeof(rid), request_id);
   char head[320];
   int hlen = snprintf(head, sizeof(head),
                       "HTTP/1.1 429 Too Many Requests\r\nContent-Type: application/json\r\n"
                       "Retry-After: %d\r\nContent-Length: %d\r\n%sConnection: close\r\n\r\n",
                       retry_after, blen, rid);
   write_all_fd(fd, head, hlen);
   write_all_fd(fd, body, blen);
}
static _Thread_local int tl_keepalive = 0;
static _Thread_local int tl_gzip = 0;

void server_http_keepalive_set(int enabled)
{
   tl_keepalive = enabled ? 1 : 0;
}

int server_http_keepalive_peek(void)
{
   return tl_keepalive;
}

int server_http_keepalive_take(void)
{
   int v = tl_keepalive;
   tl_keepalive = 0;
   return v;
}

void server_http_gzip_set(int enabled)
{
   tl_gzip = enabled ? 1 : 0;
}

int server_http_gzip_peek(void)
{
   return tl_gzip;
}
