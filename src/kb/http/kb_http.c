/* kb_http.c: aimee-kb public HTTP/1.1 API server (Phase 1).
 *
 * Serves /v1/health, /v1/version, /v1/capabilities on a plain TCP port.
 * Runs in a background pthread; disabled when port == 0. */
#include "aimee.h"
#include "config.h"
#include "config_database.h" /* §2c: config_resolve_embedding_dim / is_pinned */
#include "lifecycle.h"       /* §2c: db2_dim_change_reset / db2_probe_embedder_dim */
#include "kb_curator_queue.h"
#include "kb_http.h"
#include "kb_http_code.h"
#include "kb_http_ingest.h"
#include "kb_http_jobs.h"
#include "kb_http_reflections.h"
#include "kb_http_releases.h"
#include "kb_http_search.h"
#include "kb_curator_serve.h"
#include "kb_service.h"
#include "kb_service_kb.h"
#include "db2/kb_service_backend.h"
#include "db2/canonical_index.h"
#include "kb_enroll.h"
#include "kb_pki.h"
#include "kb_paths.h"
#include "kb_scope.h"
#include "kb_verifier.h"
#include "kb/http/openapi_data.h"
#include "db2/lifecycle.h"
#include "db2/pgvec_kb_service.h"
#include "db2/kb_vectors.h"
#include "db2/kb_payload.h"
#include "db2/vector_index_ops.h"
#include "db2/kb_runtime_state.h"
#include "db2/corpus_jobs.h"
#include "kb.h"
#include "kb_intel_payload.h"
#include "log.h"
#include "workspace.h"
#include "cJSON.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

#define KB_HTTP_BACKLOG  16
#define KB_HTTP_READ_MAX 4096
#define KB_HTTP_RESP_MAX (1024 * 1024)

extern kb_service_ctx_t *g_kb_ctx;
int kb_dispatch_action_json(const char *action, const char *body, int body_len, char *out_buf,
                            int out_cap);

static pthread_t g_thread;
static int g_listen_fd = -1;
static volatile int g_running = 0;
char g_bearer_token[256];

/* ── response helpers ───────────────────────────────────────────────────── */

void write_all(int fd, const char *buf, int len)
{
   int sent = 0;
   while (sent < len)
   {
      int n = (int)write(fd, buf + sent, (size_t)(len - sent));
      if (n <= 0)
         break;
      sent += n;
   }
}

void send_response_ex(int fd, int status, const char *body, const char *request_id,
                      const char *content_type)
{
   const char *reason = status == 200   ? "OK"
                        : status == 201 ? "Created"
                        : status == 202 ? "Accepted"
                        : status == 401 ? "Unauthorized"
                        : status == 404 ? "Not Found"
                        : status == 405 ? "Method Not Allowed"
                        : status == 500 ? "Internal Server Error"
                        : status == 503 ? "Service Unavailable"
                                        : "Bad Request";
   if (!content_type || !content_type[0])
      content_type = "application/json";
   char hdr[512];
   int hlen;
   if (request_id && request_id[0])
      hlen = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %d\r\n"
                      "X-Request-ID: %s\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      status, reason, content_type, (int)strlen(body), request_id);
   else
      hlen = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %d\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      status, reason, content_type, (int)strlen(body));
   write_all(fd, hdr, hlen);
   write_all(fd, body, (int)strlen(body));
}

void send_response(int fd, int status, const char *body)
{
   send_response_ex(fd, status, body, NULL, NULL);
}

/* ── public route logic (also called by unit tests) ─────────────────────── */

int kb_http_route(const char *method, const char *path, const char *auth_header,
                  const char *bearer_token, char *out_buf, int out_cap)
{
   /* Auth check — routed through the pluggable Verifier seam (kb_verifier.h).
    * The built-in kb-token verifier reproduces the v1 opaque-bearer check. */
   if (bearer_token && bearer_token[0])
   {
      const char *presented =
          (auth_header && strncmp(auth_header, "Bearer ", 7) == 0) ? auth_header + 7 : "";
      kb_verify_result_t vr;
      if (!kb_verifier_authenticate(presented, bearer_token, &vr, NULL, 0))
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"unauthorized\"}");
         return 401;
      }
   }

   if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
      return 405;
   }

   if (strcmp(path, "/v1/health") == 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"status\":\"ok\"}");
      return 200;
   }

   if (strcmp(path, "/v1/version") == 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"version\":\"%s\",\"service\":\"aimee-kb\"}",
               AIMEE_VERSION);
      return 200;
   }

   if (strcmp(path, "/v1/capabilities") == 0)
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"capabilities\":[\"memory\",\"search\",\"index\"],"
               "\"version\":\"%s\"}",
               AIMEE_VERSION);
      return 200;
   }

   snprintf(out_buf, (size_t)out_cap, "{\"error\":\"not found\"}");
   return 404;
}

/* ── per-connection handler (implementation in kb_http_conn.c) ──────────── */
void handle_connection(int fd);

/* ── listener thread ─────────────────────────────────────────────────────── */

static void *listener_thread(void *arg)
{
   (void)arg;
   while (g_running)
   {
      struct sockaddr_in addr;
      socklen_t addrlen = sizeof(addr);
      int fd = accept(g_listen_fd, (struct sockaddr *)&addr, &addrlen);
      if (fd < 0)
      {
         if (g_running)
            LOG_WARN("kb_http", "accept failed: %s", strerror(errno));
         break;
      }
      /* CLOEXEC: ingest spawns converter subprocesses (pandoc/pdftotext) that
       * must not inherit the client connection — otherwise a slow/hung
       * converter keeps the socket open after we close it. */
      fcntl(fd, F_SETFD, FD_CLOEXEC);
      handle_connection(fd);
      close(fd);
   }
   return NULL;
}

/* ── public start/stop ───────────────────────────────────────────────────── */

int kb_http_start(int port, const char *bearer_token)
{
   if (port <= 0)
      return 0;

   g_bearer_token[0] = '\0';
   if (bearer_token && bearer_token[0])
      snprintf(g_bearer_token, sizeof(g_bearer_token), "%s", bearer_token);

   g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
   if (g_listen_fd < 0)
      return -1;
   /* CLOEXEC: the ingest path forks converter subprocesses (pandoc/pdftotext);
    * they must not inherit the listening socket, or a hung converter holds the
    * port open and blocks the service from rebinding on restart. */
   fcntl(g_listen_fd, F_SETFD, FD_CLOEXEC);
   int opt = 1;
   setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
   const char *b = getenv("AIMEE_KB_HTTP_BIND");
   in_addr_t baddr = (b && b[0]) ? INADDR_ANY : INADDR_LOOPBACK;
   struct sockaddr_in sa;
   memset(&sa, 0, sizeof(sa));
   sa.sin_family = AF_INET;
   sa.sin_addr.s_addr = htonl(baddr);
   sa.sin_port = htons((uint16_t)port);

   if (bind(g_listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
       listen(g_listen_fd, KB_HTTP_BACKLOG) < 0)
   {
      close(g_listen_fd);
      g_listen_fd = -1;
      return -1;
   }

   g_running = 1;
   if (pthread_create(&g_thread, NULL, listener_thread, NULL) != 0)
   {
      g_running = 0;
      close(g_listen_fd);
      g_listen_fd = -1;
      return -1;
   }

   LOG_INFO("kb_http", "listening on %s:%d", baddr == INADDR_ANY ? "0.0.0.0" : "127.0.0.1", port);
   return 0;
}

void kb_http_stop(void)
{
   if (!g_running)
      return;
   g_running = 0;
   if (g_listen_fd >= 0)
   {
      shutdown(g_listen_fd, SHUT_RDWR);
      close(g_listen_fd);
      g_listen_fd = -1;
   }
   pthread_join(g_thread, NULL);
}

/* ── Phase 5 backend declarations ────────────────────────────────────────── */
/* index.h and memory.h are already included via aimee.h; use their real types.
 * db2/artifacts.h is NOT in the aimee.h chain, so declare those types locally. */

extern char *kb_search_json_ex(const char *project, const char *query, const char *embedding_cmd,
                               int max_results, const char *fusion_mode_override);

/* Matches db2_artifact_row_t in db2/artifacts.h */
typedef struct
{
   char id[37];
   char kind[64];
   char state[32];
   char scope_kind[32];
   char scope_id[128];
   double confidence;
   char payload_json[4096];
   char updated_at[32];
} kbhttp_artifact_row_t;

/* Matches db2_artifact_citation_t in db2/artifacts.h */
typedef struct
{
   char source_kind[64];
   char source_id[256];
} kbhttp_artifact_citation_t;

/* Matches db2_artifact_link_row_t in db2/artifacts.h */
typedef struct
{
   char to_id[37];
   char link_kind[64];
} kbhttp_artifact_link_row_t;

extern int db2_artifact_read(const char *id, kbhttp_artifact_row_t *out,
                             kbhttp_artifact_citation_t *citations, int max_citations,
                             int *citation_count);
extern int db2_artifact_links_read(const char *id, kbhttp_artifact_link_row_t *out, int max);

/* ── Phase 5 helpers ─────────────────────────────────────────────────────── */

/* Append JSON-escaped src into buf at position pos; return new pos. */
static int json_escape(const char *src, char *buf, int pos, int cap)
{
   if (!src)
      return pos;
   for (; *src && pos + 6 < cap; src++)
   {
      unsigned char c = (unsigned char)*src;
      if (c == '"' || c == '\\')
      {
         buf[pos++] = '\\';
         buf[pos++] = (char)c;
      }
      else if (c < 0x20)
      {
         pos += snprintf(buf + pos, (size_t)(cap - pos), "\\u%04x", c);
      }
      else
      {
         buf[pos++] = (char)c;
      }
   }
   return pos;
}

/* printf-append into buf at pos, returning the new pos clamped to [0, cap].
 * snprintf returns the would-be length, so the raw `pos += snprintf(...)` idiom
 * can run pos PAST cap; a later (cap - pos) then wraps to a huge size_t and the
 * next write lands out of bounds. The per-record headroom guards in the JSON
 * builders below (pos + 64 < cap) are far smaller than a max escaped record, so
 * every builder routes its appends through this clamp. */
static int js_appendf(char *buf, int pos, int cap, const char *fmt, ...)
{
   if (pos < 0)
      return 0;
   if (pos >= cap)
      return cap;
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(buf + pos, (size_t)(cap - pos), fmt, ap);
   va_end(ap);
   if (n < 0)
      return pos;
   pos += n;
   return pos > cap ? cap : pos;
}

/* Extract a query parameter from "key=val&..." into out. */
static int qparam(const char *qs, const char *key, char *out, size_t out_cap)
{
   if (!qs || !out_cap)
      return 0;
   out[0] = '\0';
   size_t klen = strlen(key);
   const char *p = qs;
   while (*p)
   {
      if (strncmp(p, key, klen) == 0 && p[klen] == '=')
      {
         p += klen + 1;
         size_t i = 0;
         while (*p && *p != '&' && i + 1 < out_cap)
         {
            if (*p == '%' && p[1] && p[2])
            {
               int hi = (p[1] >= '0' && p[1] <= '9')   ? p[1] - '0'
                        : (p[1] >= 'A' && p[1] <= 'F') ? p[1] - 'A' + 10
                        : (p[1] >= 'a' && p[1] <= 'f') ? p[1] - 'a' + 10
                                                       : -1;
               int lo = (p[2] >= '0' && p[2] <= '9')   ? p[2] - '0'
                        : (p[2] >= 'A' && p[2] <= 'F') ? p[2] - 'A' + 10
                        : (p[2] >= 'a' && p[2] <= 'f') ? p[2] - 'a' + 10
                                                       : -1;
               if (hi >= 0 && lo >= 0)
               {
                  out[i++] = (char)((hi << 4) | lo);
                  p += 3;
                  continue;
               }
            }
            out[i++] = (*p == '+') ? ' ' : *p;
            p++;
         }
         out[i] = '\0';
         return 1;
      }
      while (*p && *p != '&')
         p++;
      if (*p == '&')
         p++;
   }
   return 0;
}

/* Extract a JSON string field value from body into out. */
static int json_str(const char *body, const char *key, char *out, size_t out_cap)
{
   if (!body || !out_cap)
      return 0;
   out[0] = '\0';
   char needle[128];
   snprintf(needle, sizeof(needle), "\"%s\"", key);
   const char *p = strstr(body, needle);
   if (!p)
      return 0;
   p += strlen(needle);
   while (*p == ' ' || *p == '\t')
      p++;
   if (*p != ':')
      return 0;
   p++;
   while (*p == ' ' || *p == '\t')
      p++;
   if (*p != '"')
      return 0;
   p++;
   size_t i = 0;
   while (*p && *p != '"' && i + 1 < out_cap)
   {
      if (*p == '\\' && *(p + 1))
         p++;
      out[i++] = *p++;
   }
   out[i] = '\0';
   return 1;
}

/* Extract a JSON integer field from body. Returns default_val if not found. */
static int json_int(const char *body, const char *key, int default_val)
{
   if (!body)
      return default_val;
   char needle[128];
   snprintf(needle, sizeof(needle), "\"%s\"", key);
   const char *p = strstr(body, needle);
   if (!p)
      return default_val;
   p += strlen(needle);
   while (*p == ' ' || *p == '\t')
      p++;
   if (*p != ':')
      return default_val;
   p++;
   while (*p == ' ' || *p == '\t')
      p++;
   if (*p < '0' || *p > '9')
      return default_val;
   return atoi(p);
}

static int json_bool(const char *body, const char *key, int default_val)
{
   if (!body)
      return default_val;
   char needle[128];
   snprintf(needle, sizeof(needle), "\"%s\"", key);
   const char *p = strstr(body, needle);
   if (!p)
      return default_val;
   p += strlen(needle);
   while (*p == ' ' || *p == '\t')
      p++;
   if (*p != ':')
      return default_val;
   p++;
   while (*p == ' ' || *p == '\t')
      p++;
   if (strncmp(p, "true", 4) == 0)
      return 1;
   if (strncmp(p, "false", 5) == 0)
      return 0;
   return default_val;
}
static int json_body_error(char *out_buf, int out_cap, int status, const char *message)
{
   snprintf(out_buf, (size_t)out_cap, "{\"error\":\"%s\"}", message);
   return status;
}
static int json_copy_response(char *out_buf, int out_cap, char *json)
{
   if (!json)
      return json_body_error(out_buf, out_cap, 500, "out of memory");
   size_t len = strlen(json);
   if (len >= (size_t)out_cap)
   {
      free(json);
      return json_body_error(out_buf, out_cap, 500, "response too large");
   }
   memcpy(out_buf, json, len + 1);
   free(json);
   return 200;
}
static void kb_http_write_build_stats(char *out_buf, int out_cap, const char *project,
                                      const kb_stats_t *stats)
{
   int pos = snprintf(out_buf, (size_t)out_cap, "{\"status\":\"ok\",\"project\":\"");
   pos = json_escape(project ? project : "", out_buf, pos, out_cap);
   if (pos >= out_cap)
      return;
   pos = js_appendf(out_buf, pos, out_cap,
                    "\",\"files_scanned\":%d,\"files_indexed\":%d,"
                    "\"files_skipped\":%d,\"files_removed\":%d,"
                    "\"chunks_added\":%d,\"chunks_removed\":%d,\"embeddings_added\":%d}",
                    stats ? stats->files_scanned : 0, stats ? stats->files_indexed : 0,
                    stats ? stats->files_skipped : 0, stats ? stats->files_removed : 0,
                    stats ? stats->chunks_added : 0, stats ? stats->chunks_removed : 0,
                    stats ? stats->embeddings_added : 0);
   (void)pos;
}
static int kb_http_vector_upsert_document(int64_t document_id, const float *vec, int dim,
                                          const char *payload_json, void *ctx)
{
   (void)ctx;
   return pgvec_kb_vector_upsert_document(document_id, vec, dim, payload_json);
}

static const char *kb_http_queue_state(const db2_kb_service_async_queue_stats_t *stats)
{
   if (stats->running > 0)
      return "running";
   if (stats->pending > 0)
      return "queued";
   if (stats->failed > 0)
      return "failed";
   return "idle";
}

static void kb_http_corpus_pipeline_json(char *out_buf, int out_cap,
                                         const db2_corpus_pipeline_stats_t *stats)
{
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(
       root, "state",
       stats->failed > 0 ? "failed" : (stats->pending + stats->running > 0 ? "active" : "idle"));
   if (stats->processed > 0)
      cJSON_AddNumberToObject(root, "processed", stats->processed);
   cJSON_AddNumberToObject(root, "pending", stats->pending);
   cJSON_AddNumberToObject(root, "running", stats->running);
   cJSON_AddNumberToObject(root, "done", stats->complete);
   cJSON_AddNumberToObject(root, "failed", stats->failed);
   cJSON_AddNumberToObject(root, "total", stats->total);
   cJSON *stages = cJSON_AddArrayToObject(root, "stages");
   db2_corpus_pipeline_stage_count_t rows[64];
   int n = db2_corpus_pipeline_stage_counts(rows, 64);
   for (int i = 0; i < n; i++)
   {
      cJSON *row = cJSON_CreateObject();
      cJSON_AddStringToObject(row, "stage", rows[i].stage);
      cJSON_AddStringToObject(row, "status", rows[i].stage_status);
      cJSON_AddNumberToObject(row, "count", rows[i].count);
      cJSON_AddItemToArray(stages, row);
   }
   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   snprintf(out_buf, (size_t)out_cap, "%s", json ? json : "{\"error\":\"out of memory\"}");
   free(json);
}

/* Extract segment N (0-based) from path like /v1/artifacts/UUID/links. */
static void path_seg(const char *path, int n, char *out, size_t out_cap)
{
   if (!path || !out_cap)
   {
      out[0] = '\0';
      return;
   }
   const char *p = path;
   int seg = 0;
   while (*p == '/')
      p++;
   while (seg < n && *p)
   {
      while (*p && *p != '/')
         p++;
      if (*p == '/')
         p++;
      seg++;
   }
   size_t i = 0;
   while (*p && *p != '/' && i + 1 < out_cap)
      out[i++] = *p++;
   out[i] = '\0';
}

/* ── Phase 5 extended routing ────────────────────────────────────────────── */

int kb_http_route_ex(const char *method, const char *path, const char *query_string,
                     const char *auth_header, const char *bearer_token, const char *body,
                     int body_len, char *out_buf, int out_cap)
{
   /* POST /v1/enroll/redeem — a client redeems its enrollment token for a client
    * certificate. The single-use token IS the credential, so this route is
    * intentionally reachable WITHOUT the owner bearer (it sits BEFORE the bearer
    * auth gate). Body: {"token":..,"csr":..}. The client supplies a CSR — its
    * private key never leaves it — and gets back the CA-signed client cert + the
    * granted scope. An invalid / replayed token or a bad CSR is a 401. */
   if (strcmp(path, "/v1/enroll/redeem") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      cJSON *req = body ? cJSON_Parse(body) : NULL;
      const cJSON *jtok = req ? cJSON_GetObjectItemCaseSensitive(req, "token") : NULL;
      const cJSON *jcsr = req ? cJSON_GetObjectItemCaseSensitive(req, "csr") : NULL;
      if (!cJSON_IsString(jtok) || !cJSON_IsString(jcsr))
      {
         cJSON_Delete(req);
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"bad request: token (string) and csr (PEM string) required\"}");
         return 400;
      }
      char scope[KB_ENROLL_SCOPE_MAX];
      char *cert = malloc(KB_PKI_CERT_PEM_MAX);
      int rc = cert ? kb_enroll_redeem_csr(kb_default_config_dir(), jtok->valuestring,
                                           jcsr->valuestring, 60L * 60 * 24 * 90, scope,
                                           sizeof(scope), cert, KB_PKI_CERT_PEM_MAX)
                    : -1;
      cJSON_Delete(req);
      if (rc != 0)
      {
         free(cert);
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"enrollment failed: invalid or already-used token, or bad CSR\"}");
         return 401;
      }
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "client_cert", cert);
      cJSON_AddStringToObject(resp, "scope", scope);
      char *out = cJSON_PrintUnformatted(resp);
      snprintf(out_buf, (size_t)out_cap, "%s", out ? out : "{}");
      free(out);
      cJSON_Delete(resp);
      free(cert);
      return 200;
   }

   /* Auth + scope authorization, routed through the pluggable Verifier seam
    * (kb_verifier.h). The built-in kb-token verifier validates the configured
    * bearer (which may be self-describing "scope:<kind>:<id>:<secret>") and
    * yields the verified scope. Per verify-then-trust, the scope used for the
    * cross-scope check comes from that verified result, never from the caller.
    * `vr` is function-scoped so owner-only routes (e.g. /v1/enroll) can tell an
    * unscoped owner credential (empty scope_kind) from a scoped one. */
   kb_verify_result_t vr;
   memset(&vr, 0, sizeof(vr));
   if (bearer_token && bearer_token[0])
   {
      const char *presented =
          (auth_header && strncmp(auth_header, "Bearer ", 7) == 0) ? auth_header + 7 : "";
      if (!kb_verifier_authenticate(presented, bearer_token, &vr, NULL, 0))
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"unauthorized\"}");
         return 401;
      }

      /* Scoped token: deny cross-scope access when the request names a scope. */
      if (vr.scope_kind[0])
      {
         char rkind[32] = "", rid[128] = "";
         if (kb_scope_request_target(query_string, body, rkind, sizeof(rkind), rid, sizeof(rid)) &&
             !kb_scope_authorized(vr.scope_kind, vr.scope_id, rkind, rid))
         {
            snprintf(out_buf, (size_t)out_cap,
                     "{\"error\":\"forbidden: token scoped %s:%s cannot access %s:%s\"}",
                     vr.scope_kind, vr.scope_id, rkind, rid);
            return 403;
         }
      }
   }

   /* POST /v1/enroll — the owner mints a one-time client enrollment (the HTTP
    * counterpart of `aimee-kb enroll`). Body: {"host":..,"port":N,"scope":..}.
    * Owner-only: a scoped bearer is rejected (verify-then-trust — scope comes
    * from the verified credential, never the body). Returns the single-use
    * aimee:// connection string. */
   if (strcmp(path, "/v1/enroll") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      if (vr.scope_kind[0])
      {
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"forbidden: enrollment minting requires the owner credential\"}");
         return 403;
      }
      cJSON *req = body ? cJSON_Parse(body) : NULL;
      const cJSON *jhost = req ? cJSON_GetObjectItemCaseSensitive(req, "host") : NULL;
      const cJSON *jport = req ? cJSON_GetObjectItemCaseSensitive(req, "port") : NULL;
      const cJSON *jscope = req ? cJSON_GetObjectItemCaseSensitive(req, "scope") : NULL;
      if (!cJSON_IsString(jhost) || !cJSON_IsNumber(jport))
      {
         cJSON_Delete(req);
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"bad request: host (string) and port (number) required\"}");
         return 400;
      }
      const char *scope = cJSON_IsString(jscope) ? jscope->valuestring : "global";
      char conn[1024];
      int rc = kb_enroll_mint(kb_default_config_dir(), jhost->valuestring, (int)jport->valuedouble,
                              scope, conn, sizeof(conn));
      cJSON_Delete(req);
      if (rc != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"failed to mint enrollment\"}");
         return 500;
      }
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "connection_string", conn);
      char *out = cJSON_PrintUnformatted(resp);
      snprintf(out_buf, (size_t)out_cap, "%s", out ? out : "{}");
      free(out);
      cJSON_Delete(resp);
      return 200;
   }

   /* GET /v1/openapi.json or /v1/openapi.yaml — serves the live OpenAPI spec.
    * The authoritative spec lives in api/openapi-v1.yaml and is embedded at
    * build time in kb/http/openapi_data.h by src/gen_openapi.py. */
   if (strcmp(path, "/v1/openapi.json") == 0 || strcmp(path, "/v1/openapi.yaml") == 0)
   {
      if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      /* Serve the raw YAML (Content-Type is set by send_response_ex for .yaml path).
       * For /v1/openapi.json the caller should expect YAML — the spec file is YAML
       * format regardless of the URL alias. */
      snprintf(out_buf, (size_t)out_cap, "%s", AIMEE_OPENAPI_YAML_STR);
      return 200;
   }

   if (strcmp(path, "/v1/health") == 0)
   {
      if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      char status_mode[16] = "";
      char project[256] = "";
      qparam(query_string, "status", status_mode, sizeof(status_mode));
      qparam(query_string, "project", project, sizeof(project));
      if ((status_mode[0] && strcmp(status_mode, "0") != 0) || project[0])
      {
         char *json = kb_service_status_json(project[0] ? project : NULL);
         if (!json)
         {
            snprintf(out_buf, (size_t)out_cap, "{\"error\":\"status unavailable\"}");
            return 500;
         }
         snprintf(out_buf, (size_t)out_cap, "%s", json);
         free(json);
         return 200;
      }
      char *json = kb_service_health_json();
      if (!json)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"health unavailable\"}");
         return 500;
      }
      snprintf(out_buf, (size_t)out_cap, "%s", json);
      free(json);
      return 200;
   }

   if (strcmp(path, "/v1/version") == 0 || strcmp(path, "/v1/capabilities") == 0)
      return kb_http_route(method, path, auth_header, bearer_token, out_buf, out_cap);

   if (strcmp(path, "/v1/intelligence/calibration/readiness") == 0)
   {
      if (strcmp(method, "GET") != 0)
         return json_body_error(out_buf, out_cap, 405, "method not allowed");
      cJSON *resp = kb_intel_calibrate_readiness_response();
      char *json = resp ? cJSON_PrintUnformatted(resp) : NULL;
      cJSON_Delete(resp);
      return json_copy_response(out_buf, out_cap, json);
   }

   if (strcmp(path, "/v1/intelligence/demotion/check") == 0)
   {
      if (strcmp(method, "GET") != 0)
         return json_body_error(out_buf, out_cap, 405, "method not allowed");
      cJSON *resp = kb_intel_demote_check_response();
      char *json = resp ? cJSON_PrintUnformatted(resp) : NULL;
      cJSON_Delete(resp);
      return json_copy_response(out_buf, out_cap, json);
   }

   if (strcmp(path, "/v1/intelligence/bandit/export") == 0)
   {
      if (strcmp(method, "GET") != 0)
         return json_body_error(out_buf, out_cap, 405, "method not allowed");
      cJSON *resp = kb_intel_bandit_export_response();
      char *json = resp ? cJSON_PrintUnformatted(resp) : NULL;
      cJSON_Delete(resp);
      return json_copy_response(out_buf, out_cap, json);
   }

   if (strcmp(path, "/v1/intelligence/bandit/replay-record") == 0)
      return strcmp(method, "POST") == 0
                 ? kb_intel_bandit_replay_record_http(body, body_len, out_buf, out_cap)
                 : json_body_error(out_buf, out_cap, 405, "method not allowed");
   if (strcmp(path, "/v1/intelligence/bandit/sample") == 0)
      return strcmp(method, "POST") == 0
                 ? kb_intel_bandit_sample_http(body, body_len, out_buf, out_cap)
                 : json_body_error(out_buf, out_cap, 405, "method not allowed");
   if (strcmp(path, "/v1/intelligence/bandit/close") == 0)
      return strcmp(method, "POST") == 0
                 ? kb_intel_bandit_close_http(body, body_len, out_buf, out_cap)
                 : json_body_error(out_buf, out_cap, 405, "method not allowed");
   if (strcmp(path, "/v1/intelligence/bandit/promote") == 0)
      return strcmp(method, "POST") == 0
                 ? kb_intel_bandit_promote_http(body, body_len, out_buf, out_cap)
                 : json_body_error(out_buf, out_cap, 405, "method not allowed");
   if (strncmp(path, "/v1/actions/", 12) == 0)
   {
      if (strcmp(method, "POST") != 0)
         return json_body_error(out_buf, out_cap, 405, "method not allowed");
      const char *action = path + 12;
      if (!action[0])
         return json_body_error(out_buf, out_cap, 404, "action not found");
      return kb_dispatch_action_json(action, body, body_len, out_buf, out_cap);
   }

   /* POST /v1/search */
   /* POST /v1/implements {"topic": "..."} — deep-curator doc<->code bridge. */
   if (strcmp(path, "/v1/implements") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      char topic[256] = "";
      if (!json_str(body, "topic", topic, sizeof(topic)) || !topic[0])
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing topic\"}");
         return 400;
      }
      if (kb_curator_implements_json(topic, out_buf, out_cap) < 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"implements unavailable\"}");
         return 503;
      }
      return 200;
   }

   /* POST /v1/synthesize {"topic": "..."} — latest cross-doc synthesis. */
   if (strcmp(path, "/v1/synthesize") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      char topic[256] = "";
      if (!json_str(body, "topic", topic, sizeof(topic)) || !topic[0])
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing topic\"}");
         return 400;
      }
      /* Returns -1 (and an empty-result object) when no synthesis exists yet. */
      kb_curator_synthesize_serve_json(topic, out_buf, out_cap);
      if (!out_buf[0])
         snprintf(out_buf, (size_t)out_cap,
                  "{\"topic\":\"%s\",\"synthesis_id\":\"\",\"text\":\"\",\"sources\":[]}", topic);
      return 200;
   }

   /* POST /v1/reembed {confirm, force, dry_run} — embedder-autodim §2c double-gated
    * dim-change reset. Server-gated by kb.reembed_on_dim_change; the destructive run
    * needs confirm=true (no confirm => a dry-run report). Resets to the
    * configured/derived dim (db2_embedding_dim()); a no-op when it already matches. */
   if (strcmp(path, "/v1/reembed") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      /* Escape hatch: force-clear a stuck reembed_in_progress marker. Non-destructive
       * (only clears the maintenance flag so search resumes), so it is available even
       * when kb.reembed_on_dim_change is off — an operator must be able to recover a
       * store wedged in maintenance regardless of the reset toggle's current state.
       * Refuses (409) when the recorded schema dim disagrees with the running dim
       * (clearing would resume search mid-transition) unless force makes it explicit. */
      if (json_bool(body, "clear_maintenance", 0))
      {
         int was = 0, recorded = 0, running = 0;
         int cforce = json_bool(body, "force", 0);
         int rc = db2_reembed_clear_maintenance(cforce, &was, &recorded, &running);
         char msg[160] = "";
         if (rc == -1)
            snprintf(msg, sizeof(msg),
                     "recorded dim %d != running dim %d; store is mid-transition — re-run "
                     "with force to clear anyway",
                     recorded, running);
         snprintf(out_buf, (size_t)out_cap,
                  "{\"cleared\":%s,\"was_in_progress\":%s,\"recorded_dim\":%d,\"running_dim\":%d,"
                  "\"dim_consistent\":%s,\"message\":\"%s\"}",
                  rc == 0 ? "true" : "false", was ? "true" : "false", recorded, running,
                  (recorded <= 0 || recorded == running) ? "true" : "false", msg);
         /* -1 = dim mismatch needs force (409); -2 = error (500). */
         return rc == 0 ? 200 : rc == -1 ? 409 : 500;
      }
      config_t rcfg;
      config_load(&rcfg);
      if (!rcfg.kb_reembed_on_dim_change)
      {
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"kb.reembed_on_dim_change is disabled; set it true to enable the "
                  "attended dim-change reset\"}");
         return 403;
      }
      int confirm = json_bool(body, "confirm", 0);
      int force = json_bool(body, "force", 0);
      int dry = json_bool(body, "dry_run", 0) || !confirm; /* no confirm => dry-run */
      /* Target precedence: explicit operator target_dim (authoritative, bypasses the
       * probe) > operator pin > the embedder's CURRENT dim via probe (after a model
       * swap, db2_embedding_dim() still reports the old/recorded value). */
      int target = json_int(body, "target_dim", 0);
      if (target <= 0)
      {
         if (config_embedding_dim_is_pinned(&rcfg))
            target = config_resolve_embedding_dim(&rcfg);
         else if (db2_probe_embedder_dim(8000, &target) != 0 || target <= 0)
         {
            snprintf(out_buf, (size_t)out_cap,
                     "{\"error\":\"could not determine the target dim from the embedder; pass "
                     "target_dim, pin AIMEE_EMBEDDING_DIM, or ensure the embedder /health is "
                     "reachable\"}");
            return 503;
         }
      }
      db2_reembed_plan_t plan;
      int rc = db2_dim_change_reset(target, force, dry, &plan);
      cJSON *o = cJSON_CreateObject();
      if (o)
      {
         cJSON_AddNumberToObject(o, "rc", rc);
         cJSON_AddNumberToObject(o, "recorded_dim", plan.recorded_dim);
         cJSON_AddNumberToObject(o, "target_dim", plan.target_dim);
         cJSON_AddBoolToObject(o, "dry_run", dry);
         cJSON_AddNumberToObject(o, "tables_dropped", plan.n_dropped);
         cJSON_AddNumberToObject(o, "rows_cleared", (double)plan.rows_cleared);
         cJSON_AddNumberToObject(o, "curator_requeued", plan.curator_requeued);
         cJSON_AddNumberToObject(o, "evidence_requeued", plan.evidence_requeued);
         cJSON_AddStringToObject(o, "detail", plan.detail);
         char *s = cJSON_PrintUnformatted(o);
         snprintf(out_buf, (size_t)out_cap, "%s", s ? s : "{}");
         free(s);
         cJSON_Delete(o);
      }
      /* -2 unknown halfvec table (409), -3 FK needs --force (412), other err 500. */
      return rc == 0 ? 200 : rc == -2 ? 409 : rc == -3 ? 412 : 500;
   }

   /* POST /v1/contradictions {"limit": N} — committed contradicting claim pairs. */
   if (strcmp(path, "/v1/contradictions") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      int limit = json_int(body, "limit", 20);
      if (kb_curator_contradictions_json(limit, out_buf, out_cap) < 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"contradictions unavailable\"}");
         return 503;
      }
      return 200;
   }

   if (strcmp(path, "/v1/search") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      /* §2c: while a dim-change re-embed is in flight the vector store is being
       * rebuilt at the new dim; serving against it would return partial/empty
       * results. Refuse explicitly (503) rather than mislead. */
      if (db2_reembed_in_progress_get(NULL, NULL) == 1)
      {
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"re-embedding in progress, retry shortly\",\"status\":"
                  "\"maintenance\"}");
         return 503;
      }
      char query[512] = "";
      if (!json_str(body, "query", query, sizeof(query)) || !query[0])
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing query\"}");
         return 400;
      }

      /* Typed-facet artifact path (deep-curator): when a `filters` object is
       * present, narrow over the artifact narrative payload and return only
       * matching artifacts (filter precision = 1.0). Returns -1 to fall through
       * to the default ranked search when no filters were supplied. */
      {
         int fr = kb_http_search_facets(body, out_buf, out_cap);
         if (fr >= 0)
            return fr;
      }

      char project[256] = "";
      char fusion_mode[64] = "";
      json_str(body, "project", project, sizeof(project));
      json_str(body, "fusion_mode", fusion_mode, sizeof(fusion_mode));
      int max_results = json_int(body, "max_results", 10);
      if (max_results < 1)
         max_results = 1;
      if (max_results > 100)
         max_results = 100;

      /* Embed the query with the SAME embedder as the corpus. Passing NULL
       * here falls back to "builtin", so a MiniLM-embedded corpus would be
       * queried with a hash vector — an embedding mismatch that turns the
       * fusion's vector arm into noise. Honour a body override, else the
       * configured embedding_command. */
      char embed_cmd[256] = "";
      json_str(body, "embedding_command", embed_cmd, sizeof(embed_cmd));
      if (!embed_cmd[0])
      {
         config_t scfg;
         if (config_load(&scfg) == 0 && scfg.embedding_command[0])
            snprintf(embed_cmd, sizeof(embed_cmd), "%s", scfg.embedding_command);
      }
      char *raw =
          kb_search_json_ex(project[0] ? project : NULL, query, embed_cmd[0] ? embed_cmd : NULL,
                            max_results, fusion_mode[0] ? fusion_mode : NULL);
      if (!raw)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"search unavailable\"}");
         return 503;
      }

      char used_mode[64] = "rrf";
      json_str(raw, "fusion_mode", used_mode, sizeof(used_mode));

      int pos = 0;
      pos = js_appendf(out_buf, pos, out_cap, "{\"hits\":[");
      int hit_count = 0;
      const char *rp = strstr(raw, "\"results\":[");
      if (rp)
      {
         rp += 11;
         while (*rp && pos + 64 < out_cap)
         {
            while (*rp == ' ' || *rp == ',' || *rp == '\n' || *rp == '\r')
               rp++;
            if (*rp != '{')
               break;

            /* Find end of this object */
            const char *obj_end = rp + 1;
            int depth = 1;
            while (*obj_end && depth > 0)
            {
               if (*obj_end == '{')
                  depth++;
               else if (*obj_end == '}')
                  depth--;
               obj_end++;
            }

            char fp[256] = "";
            char content[512] = "";
            char score_s[32] = "0.0";

            const char *f = strstr(rp, "\"file_path\":\"");
            if (f && f < obj_end)
            {
               f += 13;
               size_t i = 0;
               while (*f && *f != '"' && i < sizeof(fp) - 1)
               {
                  if (*f == '\\' && *(f + 1))
                     f++;
                  fp[i++] = *f++;
               }
               fp[i] = '\0';
            }

            const char *co = strstr(rp, "\"content\":\"");
            if (co && co < obj_end)
            {
               co += 11;
               size_t i = 0;
               while (*co && *co != '"' && i < sizeof(content) - 1)
               {
                  if (*co == '\\' && *(co + 1))
                     co++;
                  content[i++] = *co++;
               }
               content[i] = '\0';
            }

            const char *sc = strstr(rp, "\"score\":");
            if (sc && sc < obj_end)
            {
               sc += 8;
               while (*sc == ' ')
                  sc++;
               size_t i = 0;
               while (i < sizeof(score_s) - 1 &&
                      (*sc == '-' || (*sc >= '0' && *sc <= '9') || *sc == '.' || *sc == 'e' ||
                       *sc == 'E' || *sc == '+'))
                  score_s[i++] = *sc++;
               score_s[i] = '\0';
            }

            if (hit_count > 0)
               out_buf[pos++] = ',';
            pos = js_appendf(out_buf, pos, out_cap, "{\"artifact_id\":\"");
            pos = json_escape(fp, out_buf, pos, out_cap);
            pos = js_appendf(out_buf, pos, out_cap,
                             "\",\"score\":%s,\"kind\":\"doc_chunk\",\"excerpt\":\"",
                             score_s[0] ? score_s : "0.0");
            pos = json_escape(content, out_buf, pos, out_cap);
            pos = js_appendf(out_buf, pos, out_cap, "\",\"citations\":[]}");
            hit_count++;
            rp = obj_end;
         }
      }

      pos = js_appendf(out_buf, pos, out_cap,
                       "],\"next_cursor\":null,\"total_hits\":%d,"
                       "\"fusion_mode_used\":\"%s\"}",
                       hit_count, used_mode);
      free(raw);
      return 200;
   }

   /* GET /v1/artifacts/{id}/links */
   {
      char seg0[32] = "", seg1[64] = "", seg2[64] = "", seg3[32] = "";
      path_seg(path, 0, seg0, sizeof(seg0));
      path_seg(path, 1, seg1, sizeof(seg1));
      path_seg(path, 2, seg2, sizeof(seg2));
      path_seg(path, 3, seg3, sizeof(seg3));

      if (strcmp(seg0, "v1") == 0 && strcmp(seg1, "artifacts") == 0 && seg2[0] &&
          strcmp(seg3, "links") == 0)
      {
         if (strcmp(method, "GET") != 0)
         {
            snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
            return 405;
         }
         kbhttp_artifact_link_row_t links[64];
         int nlinks = db2_artifact_links_read(seg2, links, 64);
         if (nlinks < 0)
         {
            snprintf(out_buf, (size_t)out_cap, "{\"error\":\"not found\"}");
            return 404;
         }
         int pos = 0;
         pos = js_appendf(out_buf, pos, out_cap, "{\"artifact_id\":\"");
         pos = json_escape(seg2, out_buf, pos, out_cap);
         pos = js_appendf(out_buf, pos, out_cap, "\",\"links\":[");
         for (int i = 0; i < nlinks && pos + 64 < out_cap; i++)
         {
            if (i > 0)
               out_buf[pos++] = ',';
            pos = js_appendf(out_buf, pos, out_cap, "{\"to_id\":\"");
            pos = json_escape(links[i].to_id, out_buf, pos, out_cap);
            pos = js_appendf(out_buf, pos, out_cap, "\",\"link_kind\":\"");
            pos = json_escape(links[i].link_kind, out_buf, pos, out_cap);
            pos = js_appendf(out_buf, pos, out_cap, "\"}");
         }
         pos = js_appendf(out_buf, pos, out_cap, "]}");
         return 200;
      }

      /* GET /v1/artifacts/{id} */
      if (strcmp(seg0, "v1") == 0 && strcmp(seg1, "artifacts") == 0 && seg2[0] && !seg3[0])
      {
         if (strcmp(method, "GET") != 0)
         {
            snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
            return 405;
         }
         kbhttp_artifact_row_t row;
         kbhttp_artifact_citation_t citations[8];
         int ncitations = 0;
         if (db2_artifact_read(seg2, &row, citations, 8, &ncitations) != 0)
         {
            snprintf(out_buf, (size_t)out_cap, "{\"error\":\"not found\"}");
            return 404;
         }
         int pos = 0;
         pos = js_appendf(out_buf, pos, out_cap, "{\"id\":\"");
         pos = json_escape(row.id, out_buf, pos, out_cap);
         pos = js_appendf(out_buf, pos, out_cap, "\",\"kind\":\"");
         pos = json_escape(row.kind, out_buf, pos, out_cap);
         pos = js_appendf(out_buf, pos, out_cap, "\",\"state\":\"");
         pos = json_escape(row.state, out_buf, pos, out_cap);
         pos = js_appendf(out_buf, pos, out_cap, "\",\"scope_kind\":\"");
         pos = json_escape(row.scope_kind, out_buf, pos, out_cap);
         pos = js_appendf(out_buf, pos, out_cap, "\",\"scope_id\":\"");
         pos = json_escape(row.scope_id, out_buf, pos, out_cap);
         pos = js_appendf(out_buf, pos, out_cap,
                          "\",\"confidence\":%.6f,\"payload\":%s,\"citations\":[", row.confidence,
                          row.payload_json[0] ? row.payload_json : "{}");
         for (int i = 0; i < ncitations && pos + 64 < out_cap; i++)
         {
            if (i > 0)
               out_buf[pos++] = ',';
            pos = js_appendf(out_buf, pos, out_cap, "{\"source_kind\":\"");
            pos = json_escape(citations[i].source_kind, out_buf, pos, out_cap);
            pos = js_appendf(out_buf, pos, out_cap, "\",\"source_id\":\"");
            pos = json_escape(citations[i].source_id, out_buf, pos, out_cap);
            pos = js_appendf(out_buf, pos, out_cap, "\"}");
         }
         pos = js_appendf(out_buf, pos, out_cap, "],\"updated_at\":\"");
         pos = json_escape(row.updated_at, out_buf, pos, out_cap);
         pos = js_appendf(out_buf, pos, out_cap, "\"}");
         return 200;
      }
   }

   /* GET /v1/invalidations?since=<id> — deep-curator invalidation event feed.
    * Returns curator invalidation events (a source doc changed and its derived
    * artifacts were marked stale) with id > since, plus a next_cursor for the
    * caller to resume from. This is the pollable feed a future WebSocket push
    * would broadcast. */
   if (strcmp(path, "/v1/invalidations") == 0)
   {
      if (strcmp(method, "GET") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      char since_s[32] = "";
      long long since = 0;
      if (qparam(query_string, "since", since_s, sizeof(since_s)))
         since = strtoll(since_s, NULL, 10);

      db2_curator_invalidation_t evs[128];
      int n = db2_curator_invalidations_since((int64_t)since, evs, 128);

      cJSON *root = cJSON_CreateObject();
      cJSON *arr = cJSON_AddArrayToObject(root, "invalidations");
      long long cursor = since;
      for (int i = 0; i < n; i++)
      {
         cJSON *o = cJSON_CreateObject();
         cJSON_AddNumberToObject(o, "id", (double)evs[i].id);
         cJSON_AddStringToObject(o, "source_kind", evs[i].source_kind);
         cJSON_AddStringToObject(o, "source_id", evs[i].source_id);
         cJSON_AddNumberToObject(o, "artifacts_stale", evs[i].artifacts_stale);
         cJSON_AddStringToObject(o, "created_at", evs[i].created_at);
         cJSON_AddItemToArray(arr, o);
         if (evs[i].id > cursor)
            cursor = evs[i].id;
      }
      cJSON_AddNumberToObject(root, "next_cursor", (double)cursor);
      char *js = cJSON_PrintUnformatted(root);
      cJSON_Delete(root);
      if (js)
      {
         snprintf(out_buf, (size_t)out_cap, "%s", js);
         free(js);
      }
      return 200;
   }

   if (strcmp(path, "/v1/code/find") == 0)
      return handle_get_code_find_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/code/projects") == 0)
      return handle_get_code_projects_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/code/blast-radius") == 0)
      return handle_get_code_blast_radius_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/code/structure") == 0)
      return handle_get_code_structure_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/code/search") == 0)
      return handle_get_code_search_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/code/callers") == 0)
      return handle_get_code_callers_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/code/project-stats") == 0)
      return handle_get_code_project_stats_route(method, query_string, out_buf, out_cap);

   /* POST /v1/code/build */
   if (strcmp(path, "/v1/code/build") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      char kb_path[MAX_PATH_LEN] = "";
      char project[128] = "";
      char embed_cmd[256] = "";
      if (!json_str(body, "path", kb_path, sizeof(kb_path)) || !kb_path[0])
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing path\"}");
         return 400;
      }
      if (!json_str(body, "project", project, sizeof(project)) || !project[0])
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing project\"}");
         return 400;
      }
      (void)json_str(body, "embedding_command", embed_cmd, sizeof(embed_cmd));
      if (!embed_cmd[0])
      {
         config_t embed_cfg;
         config_load(&embed_cfg);
         snprintf(embed_cmd, sizeof(embed_cmd), "%s", config_embedding_command(&embed_cfg, NULL));
      }
      int force = json_bool(body, "force", 0);

      if (!db2_is_initialized())
      {
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"failed to open knowledge service store\"}");
         return 503;
      }
      if (pgvec_kb_service_ensure_kb_collection(384) != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"vector store unavailable\"}");
         return 503;
      }

      kb_stats_t stats;
      if (kb_build(kb_path, project, embed_cmd, force, &stats) != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"kb build failed\"}");
         return 500;
      }
      /* Canonical code index scan (symbols/definitions), mirroring the async
       * ingest worker so build and ingest produce the same index. */
      int inspected = 0;
      /* >= 0 is the scanned-file count (success); only a negative is an error. */
      if (canonical_index_scan_project(project, kb_path, force, &inspected) < 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"canonical index scan failed\"}");
         return 500;
      }
      db2_kb_runtime_state_set_now("last_ingest_at");
      kb_http_write_build_stats(out_buf, out_cap, project, &stats);
      return 200;
   }

   /* POST /v1/code/update */
   if (strcmp(path, "/v1/code/update") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      char kb_path[MAX_PATH_LEN] = "";
      char project[128] = "";
      char embed_cmd[256] = "";
      if (!json_str(body, "path", kb_path, sizeof(kb_path)) || !kb_path[0])
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing path\"}");
         return 400;
      }
      if (!json_str(body, "project", project, sizeof(project)) || !project[0])
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing project\"}");
         return 400;
      }
      (void)json_str(body, "embedding_command", embed_cmd, sizeof(embed_cmd));
      if (!embed_cmd[0])
      {
         config_t embed_cfg;
         config_load(&embed_cfg);
         snprintf(embed_cmd, sizeof(embed_cmd), "%s", config_embedding_command(&embed_cfg, NULL));
      }

      if (!db2_is_initialized())
      {
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"failed to open knowledge service store\"}");
         return 503;
      }

      kb_stats_t stats;
      if (kb_update(kb_path, project, embed_cmd, &stats) != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"kb update failed\"}");
         return 500;
      }
      int inspected = 0;
      /* >= 0 is the scanned-file count (success); only a negative is an error. */
      if (canonical_index_scan_project(project, kb_path, 0, &inspected) < 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"canonical index scan failed\"}");
         return 500;
      }
      db2_kb_runtime_state_set_now("last_ingest_at");
      {
         config_t cfg;
         config_load(&cfg);
         if (cfg.kb_curator_extract_docs_enabled)
            kb_curator_queue_docs_for_project(project);
      }
      kb_http_write_build_stats(out_buf, out_cap, project, &stats);
      return 200;
   }

   if (strcmp(path, "/v1/code/scan") == 0)
      return handle_post_code_scan_route(method, body, out_buf, out_cap);
   /* POST /v1/ingest */
   if (strcmp(path, "/v1/ingest") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      if (!db2_is_initialized())
      {
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"failed to open knowledge service store\"}");
         return 503;
      }

      config_t cfg;
      config_load(&cfg);

      char workspace[MAX_PATH_LEN] = "";
      (void)json_str(body, "workspace", workspace, sizeof(workspace));
      int force = json_bool(body, "force", 0);
      int use_all = !workspace[0] || strcmp(workspace, "all") == 0;

      char(*projects)[MAX_PATH_LEN] = calloc(MAX_DISCOVERED_PROJECTS, MAX_PATH_LEN);
      if (!projects)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"out of memory\"}");
         return 500;
      }

      int total_queued = 0;
      if (!use_all)
      {
         int n = workspace_discover_projects(workspace, 3, projects, MAX_DISCOVERED_PROJECTS);
         for (int i = 0; i < n; i++)
         {
            const char *pname = strrchr(projects[i], '/');
            pname = pname ? pname + 1 : projects[i];
            if (force)
            {
               db2_kb_service_clear_project(pname);
               pgvec_kb_vector_delete_project(pname);
               db2_kb_file_index_delete_project(pname);
            }
            db2_kb_ingest_queue_enqueue(pname, projects[i], workspace, force);
            total_queued++;
         }
      }
      else
      {
         for (int w = 0; w < cfg.workspace_count; w++)
         {
            int n = workspace_discover_projects(cfg.workspaces[w], 3, projects,
                                                MAX_DISCOVERED_PROJECTS);
            for (int i = 0; i < n; i++)
            {
               const char *pname = strrchr(projects[i], '/');
               pname = pname ? pname + 1 : projects[i];
               if (force)
               {
                  db2_kb_service_clear_project(pname);
                  pgvec_kb_vector_delete_project(pname);
                  db2_kb_file_index_delete_project(pname);
               }
               db2_kb_ingest_queue_enqueue(pname, projects[i], cfg.workspaces[w], force);
               total_queued++;
            }
         }
      }
      free(projects);

      if (g_kb_ctx)
         kb_worker_notify(g_kb_ctx);

      snprintf(out_buf, (size_t)out_cap,
               "{\"status\":\"ok\",\"projects_queued\":%d,\"message\":\"%s queued for %d "
               "project(s). Run `aimee kb ingest status` to monitor.\"}",
               total_queued, force ? "Force re-index" : "Incremental ingest", total_queued);
      return 202;
   }

   /* GET /v1/ingest/status */
   if (strcmp(path, "/v1/ingest/status") == 0)
   {
      if (strcmp(method, "GET") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      char *json = kb_service_ingest_status_json();
      if (!json)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"ingest status unavailable\"}");
         return 503;
      }
      snprintf(out_buf, (size_t)out_cap, "%s", json);
      free(json);
      return 200;
   }

   /* GET /v1/workers */
   if (strcmp(path, "/v1/workers") == 0)
   {
      if (strcmp(method, "GET") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      if (!g_kb_ctx)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"workers unavailable\"}");
         return 503;
      }
      char *json = kb_service_workers_json(g_kb_ctx);
      if (!json)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"workers unavailable\"}");
         return 503;
      }
      snprintf(out_buf, (size_t)out_cap, "%s", json);
      free(json);
      return 200;
   }

   /* GET /v1/pipeline/status */
   if (strcmp(path, "/v1/pipeline/status") == 0)
   {
      if (strcmp(method, "GET") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      db2_kb_service_async_queue_stats_t stats;
      if (db2_kb_service_async_queue_status(&stats) != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"queue unavailable\"}");
         return 503;
      }
      snprintf(out_buf, (size_t)out_cap,
               "{\"state\":\"%s\",\"queue_depth\":%d,\"active_jobs\":[],"
               "\"queue\":{\"pending\":%d,\"running\":%d,\"done\":%d,\"failed\":%d,"
               "\"total\":%d}}",
               kb_http_queue_state(&stats), stats.pending + stats.running, stats.pending,
               stats.running, stats.done, stats.failed, stats.total);
      return 200;
   }
   /* GET /v1/corpus/pipeline/status */
   if (strcmp(path, "/v1/corpus/pipeline/status") == 0)
   {
      if (strcmp(method, "GET") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      db2_corpus_pipeline_stats_t stats;
      if (db2_corpus_pipeline_status(&stats) != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"corpus pipeline unavailable\"}");
         return 503;
      }
      kb_http_corpus_pipeline_json(out_buf, out_cap, &stats);
      return 200;
   }
   /* POST /v1/corpus/pipeline/drain */
   if (strcmp(path, "/v1/corpus/pipeline/drain") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      int limit = json_int(body, "limit", 0);
      if (limit < 0)
         limit = 0;
      db2_corpus_pipeline_stats_t stats;
      if (db2_corpus_pipeline_drain(limit, &stats) != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"corpus pipeline drain failed\"}");
         return 500;
      }
      kb_http_corpus_pipeline_json(out_buf, out_cap, &stats);
      return 200;
   }
   /* POST /v1/drain */
   if (strcmp(path, "/v1/drain") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      char embed_cmd[256] = "";
      (void)json_str(body, "embedding_command", embed_cmd, sizeof(embed_cmd));
      if (!embed_cmd[0])
      {
         config_t embed_cfg;
         config_load(&embed_cfg);
         snprintf(embed_cmd, sizeof(embed_cmd), "%s", config_embedding_command(&embed_cfg, NULL));
      }
      int timeout = json_int(body, "timeout", 0);
      if (timeout < 0)
         timeout = 0;

      db2_kb_service_async_queue_stats_t stats;
      int rc =
          db2_kb_service_async_queue_drain(embed_cmd, timeout, pgvec_kb_vector_collection_name(),
                                           kb_http_vector_upsert_document, NULL, &stats);
      if (rc != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"queue drain failed\"}");
         return 500;
      }
      snprintf(out_buf, (size_t)out_cap,
               "{\"state\":\"%s\",\"processed\":%d,\"pending\":%d,\"running\":%d,"
               "\"done\":%d,\"failed\":%d,\"total\":%d}",
               kb_http_queue_state(&stats), stats.processed, stats.pending, stats.running,
               stats.done, stats.failed, stats.total);
      return 200;
   }
   /* POST /v1/maintenance/repair */
   if (strcmp(path, "/v1/maintenance/repair") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      char kb_path[4096] = "";
      char project[256] = "";
      char embed_cmd[256] = "";
      if (!json_str(body, "path", kb_path, sizeof(kb_path)) || !kb_path[0])
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing path\"}");
         return 400;
      }
      if (!json_str(body, "project", project, sizeof(project)) || !project[0])
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing project\"}");
         return 400;
      }
      (void)json_str(body, "embedding_command", embed_cmd, sizeof(embed_cmd));
      if (!embed_cmd[0])
      {
         config_t embed_cfg;
         config_load(&embed_cfg);
         snprintf(embed_cmd, sizeof(embed_cmd), "%s", config_embedding_command(&embed_cfg, NULL));
      }
      if (pgvec_kb_service_ensure_kb_collection(384) != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"vector store unavailable\"}");
         return 503;
      }
      if (!db2_is_initialized())
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"knowledge store unavailable\"}");
         return 503;
      }
      kb_stats_t stats;
      memset(&stats, 0, sizeof(stats));
      if (kb_build(kb_path, project, embed_cmd, 1, &stats) != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"knowledge repair failed\"}");
         return 500;
      }
      int pos = 0;
      pos = js_appendf(out_buf, pos, out_cap, "{\"status\":\"ok\",\"project\":\"");
      pos = json_escape(project, out_buf, pos, out_cap);
      snprintf(out_buf + pos, (size_t)(out_cap - pos),
               "\",\"files_scanned\":%d,\"files_indexed\":%d,\"files_skipped\":%d,"
               "\"files_removed\":%d,\"chunks_added\":%d,\"chunks_removed\":%d,"
               "\"embeddings_added\":%d}",
               stats.files_scanned, stats.files_indexed, stats.files_skipped, stats.files_removed,
               stats.chunks_added, stats.chunks_removed, stats.embeddings_added);
      return 200;
   }
   /* POST /v1/maintenance/reconcile */
   if (strcmp(path, "/v1/maintenance/reconcile") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      int dry_run = json_bool(body, "dry_run", 0);
      pgvec_kb_service_reconcile_result_t reconcile;
      memset(&reconcile, 0, sizeof(reconcile));
      if (pgvec_kb_service_reconcile_orphans(db2_kb_service_memory_record_exists,
                                             db2_kb_service_kb_document_exists, dry_run,
                                             &reconcile) != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"reconcile failed\"}");
         return 500;
      }
      snprintf(out_buf, (size_t)out_cap,
               "{\"status\":\"ok\",\"rc\":%d,\"dry_run\":%s,"
               "\"memory\":{\"kept\":%lld,\"pruned\":%lld},"
               "\"kb\":{\"kept\":%lld,\"pruned\":%lld}}",
               reconcile.rc, dry_run ? "true" : "false", (long long)reconcile.mem_kept,
               (long long)reconcile.mem_pruned, (long long)reconcile.kb_kept,
               (long long)reconcile.kb_pruned);
      return 200;
   }
   /* POST /v1/maintenance/clear */
   if (strcmp(path, "/v1/maintenance/clear") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      char project[256] = "";
      if (!json_str(body, "project", project, sizeof(project)) || !project[0])
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing project\"}");
         return 400;
      }
      int deleted = db2_kb_service_clear_project(project);
      if (deleted < 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"kb clear failed\"}");
         return 500;
      }
      int pos = 0;
      pos = js_appendf(out_buf, pos, out_cap, "{\"status\":\"ok\",\"project\":\"");
      pos = json_escape(project, out_buf, pos, out_cap);
      snprintf(out_buf + pos, (size_t)(out_cap - pos), "\",\"chunks_deleted\":%d}", deleted);
      return 200;
   }
   /* GET /v1/jobs/{id} */
   if (strncmp(path, "/v1/jobs/", 9) == 0 && path[9])
   {
      if (strcmp(method, "GET") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      return handle_get_job_status(path, out_buf, out_cap);
   }
   /* POST /v1/entities/search (must match before GET /v1/entities/{id}) */
   if (strcmp(path, "/v1/entities/search") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      char query[512] = "";
      if (!json_str(body, "query", query, sizeof(query)) || !query[0])
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing query\"}");
         return 400;
      }
      int limit = json_int(body, "limit", 10);
      if (limit < 1)
         limit = 1;
      if (limit > 50)
         limit = 50;

      memory_relation_t *rels = malloc((size_t)limit * sizeof(memory_relation_t));
      if (!rels)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
         return 500;
      }
      int nrels = memory_search_graph(query, limit, rels, limit);
      if (nrels < 0)
         nrels = 0;

      int pos = 0;
      pos = js_appendf(out_buf, pos, out_cap, "{\"entities\":[");
      for (int i = 0; i < nrels && pos + 64 < out_cap; i++)
      {
         if (i > 0)
            out_buf[pos++] = ',';
         pos = js_appendf(out_buf, pos, out_cap, "{\"entity\":\"");
         pos = json_escape(rels[i].src_entity, out_buf, pos, out_cap);
         pos = js_appendf(out_buf, pos, out_cap, "\",\"kind\":\"\",\"summary\":\"");
         pos = json_escape(rels[i].fact_text, out_buf, pos, out_cap);
         pos +=
             snprintf(out_buf + pos, (size_t)(out_cap - pos), "\",\"score\":%.6f}", rels[i].weight);
      }
      pos = js_appendf(out_buf, pos, out_cap, "],\"next_cursor\":null}");
      free(rels);
      return 200;
   }

   /* GET /v1/entities/{id} */
   {
      char seg0[32] = "", seg1[32] = "", seg2[256] = "", seg3[32] = "";
      path_seg(path, 0, seg0, sizeof(seg0));
      path_seg(path, 1, seg1, sizeof(seg1));
      path_seg(path, 2, seg2, sizeof(seg2));
      path_seg(path, 3, seg3, sizeof(seg3));

      if (strcmp(seg0, "v1") == 0 && strcmp(seg1, "entities") == 0 && seg2[0] && !seg3[0])
      {
         if (strcmp(method, "GET") != 0)
         {
            snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
            return 405;
         }
         memory_entity_profile_t profile;
         memset(&profile, 0, sizeof(profile));
         if (memory_get_entity_profile(seg2, &profile) != 0)
         {
            snprintf(out_buf, (size_t)out_cap, "{\"error\":\"not found\"}");
            return 404;
         }
         int pos = 0;
         pos = js_appendf(out_buf, pos, out_cap, "{\"entity\":\"");
         pos = json_escape(profile.entity, out_buf, pos, out_cap);
         pos = js_appendf(out_buf, pos, out_cap, "\",\"kind\":\"\",\"summary\":\"");
         pos = json_escape(profile.summary, out_buf, pos, out_cap);
         pos =
             js_appendf(out_buf, pos, out_cap, "\",\"facts\":[],\"tags\":[],\"updated_at\":\"\"}");
         return 200;
      }
   }
   /* ── Phase 3: Ingest API routes ──────────────────────────────────────── */
   /* POST /v1/docs */
   if (strcmp(path, "/v1/docs") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      return handle_post_docs(body, body_len, out_buf, out_cap);
   }
   /* POST /v1/docs/manifest */
   if (strcmp(path, "/v1/docs/manifest") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      return handle_post_docs_manifest(body, body_len, out_buf, out_cap);
   }
   /* GET /v1/review */
   if (strcmp(path, "/v1/review") == 0)
   {
      if (strcmp(method, "GET") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      return handle_get_review(query_string, out_buf, out_cap);
   }
   /* GET /v1/releases/active */
   if (strcmp(path, "/v1/releases/active") == 0)
   {
      if (strcmp(method, "GET") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      return handle_get_active_release(out_buf, out_cap);
   }
   /* POST /v1/releases */
   if (strcmp(path, "/v1/releases") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      return handle_post_releases(body, body_len, out_buf, out_cap);
   }

   {
      char seg0[32] = "", seg1[32] = "", seg2[256] = "", seg3[64] = "";
      path_seg(path, 0, seg0, sizeof(seg0));
      path_seg(path, 1, seg1, sizeof(seg1));
      path_seg(path, 2, seg2, sizeof(seg2));
      path_seg(path, 3, seg3, sizeof(seg3));
      /* GET /v1/docs/{id} or DELETE /v1/docs/{id} */
      if (strcmp(seg0, "v1") == 0 && strcmp(seg1, "docs") == 0 && seg2[0] && !seg3[0])
      {
         if (strcmp(method, "GET") == 0)
            return handle_get_doc(seg2, out_buf, out_cap);
         if (strcmp(method, "DELETE") == 0)
            return handle_delete_doc(seg2, out_buf, out_cap);
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      /* POST /v1/review/{id}/accept or /reject */
      if (strcmp(seg0, "v1") == 0 && strcmp(seg1, "review") == 0 && seg2[0] && seg3[0])
      {
         if (strcmp(method, "POST") != 0)
         {
            snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
            return 405;
         }
         if (strcmp(seg3, "accept") == 0)
            return handle_post_review_accept(seg2, body, body_len, out_buf, out_cap);
         if (strcmp(seg3, "reject") == 0)
            return handle_post_review_reject(seg2, body, body_len, out_buf, out_cap);
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"not found\"}");
         return 404;
      }
      /* POST /v1/releases/{id}/promote or /rollback */
      if (strcmp(seg0, "v1") == 0 && strcmp(seg1, "releases") == 0 && seg2[0] && seg3[0])
      {
         if (strcmp(method, "POST") != 0)
         {
            snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
            return 405;
         }
         if (strcmp(seg3, "promote") == 0)
            return handle_post_promote(seg2, out_buf, out_cap);
         if (strcmp(seg3, "rollback") == 0)
            return handle_post_rollback(seg2, body, body_len, out_buf, out_cap);
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"not found\"}");
         return 404;
      }
   }
   /* ── Phase 6: Reflection HTTP surface ───────────────────────────────────── */
   /* POST /v1/reflections */
   if (strcmp(path, "/v1/reflections") == 0 && strcmp(method, "POST") == 0)
      return handle_post_reflections(body, body_len, out_buf, out_cap);
   /* GET /v1/reflections */
   if (strcmp(path, "/v1/reflections") == 0 && strcmp(method, "GET") == 0)
      return handle_get_reflections(query_string, out_buf, out_cap);

   /* POST /v1/feedback/in-session */
   if (strcmp(path, "/v1/feedback/in-session") == 0 && strcmp(method, "POST") == 0)
      return handle_post_feedback_in_session(body, body_len, out_buf, out_cap);

   {
      char seg0[32] = "", seg1[32] = "", seg2[64] = "", seg3[32] = "";
      path_seg(path, 0, seg0, sizeof(seg0));
      path_seg(path, 1, seg1, sizeof(seg1));
      path_seg(path, 2, seg2, sizeof(seg2));
      path_seg(path, 3, seg3, sizeof(seg3));

      /* POST /v1/reflections/{id}/accept or /reject */
      if (strcmp(seg0, "v1") == 0 && strcmp(seg1, "reflections") == 0 && seg2[0] && seg3[0])
      {
         if (strcmp(method, "POST") != 0)
         {
            snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
            return 405;
         }
         if (strcmp(seg3, "accept") == 0)
            return handle_post_reflection_accept(seg2, body, body_len, out_buf, out_cap);
         if (strcmp(seg3, "reject") == 0)
            return handle_post_reflection_reject(seg2, body, body_len, out_buf, out_cap);
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"not found\"}");
         return 404;
      }
   }

   /* Fallthrough to Phase 1 handler */
   return kb_http_route(method, path, auth_header, bearer_token, out_buf, out_cap);
}
