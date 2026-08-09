/* kb_http.c: aimee-kb public HTTP/1.1 API server (Phase 1).
 *
 * Serves /v1/health, /v1/version, /v1/capabilities on a plain TCP port.
 * Runs in a background pthread; disabled when port == 0. */
#include "aimee.h"
#include "config.h"
#include "config_database.h" /* §2c: config_resolve_embedder_dims / is_pinned */
#include "db2_pool.h"        /* db2_pool_stats — health reports pool starvation */
#include "lifecycle.h"       /* §2c: db2_dim_change_reset / db2_probe_embedder_dim */
#include "kb_curator_queue.h"
#include "kb_http.h"
#include "kb_http_code.h"
#include "kb_http_pdf.h"
#include "kb_http_ingest.h"
#include "kb_http_jobs.h"
#include "kb_http_reflections.h"
#include "kb_http_releases.h"
#include "kb_http_search.h"
#include "kb_curator_serve.h"
#include "kb_service.h"
#include "kb/kb_service_code_embed.h"
#include "kb_service_kb.h"
#include "db2/kb_service_backend.h"
#include "db2/canonical_index.h"
#include "kb_enroll.h"
#include "kb_pki.h"
#include "kb_paths.h"
#include "kb_scope.h"
#include "kb_route_acl.h"
#include "kb_http_console.h"
#include "kb_http_accounts.h"
#include "kb_http_bootstrap.h"
#include <time.h>
#include "kb_http_governance.h"
#include "kb_http_insights.h"
#include "kb_http_budget.h"
#include "kb_http_rate.h"
#include "kb_http_servers.h"
#include "kb_http_telemetry.h"
#include "db2/enrollments.h"
#include "kb_verifier.h"
#include "kb_auth_oidc.h"
#include "kb_identity.h"
#include "kb_reqctx.h"
#include "kb_http_models.h"
#include "kb_http_grants.h"
#include "kb_http_team.h"
#include "kb/http/openapi_data.h"
#include "db2/lifecycle.h"
#include "db2/pgvec_kb_service.h"
#include "db2/kb_vectors.h"
#include "db2/kb_payload.h"
#include "db2/vector_index_ops.h"
#include "db2/kb_runtime_state.h"
#include "db2/corpus_jobs.h"
#include "db2/code_index.h"
#include "db2/pgvec_transport.h"
#include "db2/sketch.h"
#include "kb.h"
#include "kb_intel_payload.h"
#include "kb/kb_login_throttle.h"
#include "log.h"
#include <aimee/workspace/workspace.h>
#include "cJSON.h"
#include "kb_http_json.h"
#include <aimee/core/connection/auth.h>
#include <unistd.h>
#define KB_HTTP_READ_MAX 4096
#define KB_HTTP_RESP_MAX (1024 * 1024)
extern kb_service_ctx_t *g_kb_ctx;
int kb_dispatch_action_json(const char *action, const char *body, int body_len, char *out_buf,
                            int out_cap);

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
                        : status == 413 ? "Content Too Large"
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

/* Health has to be able to say "I am sick".
 *
 * This endpoint returned a bare {"status":"ok"} unconditionally, so a kb whose
 * connection pool had been leaking for three hours reported ok on the one port
 * that still answered — 1096 failed health checks later, the only symptom
 * visible from outside was a TIMEOUT, which reads as a hung box rather than a
 * diagnosed fault. Report the pool, and fail the check when it is starved: every
 * member stuck past its ceiling with callers queued is a lease leak this process
 * cannot fix in place (the pool refuses to reclaim a live lease, and rightly).
 *
 * Non-2xx is the point. The container HEALTHCHECK is `curl -fsS`, so degraded
 * here is what finally makes Docker's unhealthy state mean something. It fires
 * before the pool reaper gives up and exits, so the softer signal comes first.
 *
 * Writes the pool object into `buf`; returns 1 when starved. */
static int kb_health_pool_json(char *buf, size_t cap)
{
   int size = 0, in_use = 0, waiters = 0;
   long grants = 0, timeouts = 0, stuck = 0, poisoned = 0;
   db2_pool_stats(&size, &in_use, &waiters, &grants, &timeouts, &stuck, &poisoned);
   int starved = (size > 0 && in_use == size && waiters > 0);
   snprintf(buf, cap,
            "\"pool\":{\"size\":%d,\"in_use\":%d,\"waiters\":%d,\"lease_timeouts\":%ld,"
            "\"stuck\":%ld,\"poisoned\":%ld}",
            size, in_use, waiters, timeouts, stuck, poisoned);
   return starved;
}

int kb_http_route(const char *method, const char *path, const char *auth_header,
                  const char *bearer_token, char *out_buf, int out_cap)
{
   /* Auth check — routed through the pluggable Verifier seam (kb_verifier.h).
    * The built-in kb-token verifier reproduces the v1 opaque-bearer check. */
   if (bearer_token && bearer_token[0])
   {
      const char *presented = aimee_core_bearer_token(auth_header);
      if (!presented)
         presented = "";
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
      char pool[256] = "";
      int starved = kb_health_pool_json(pool, sizeof(pool));
      snprintf(out_buf, (size_t)out_cap, "{\"status\":\"%s\",%s}", starved ? "degraded" : "ok",
               pool);
      return starved ? 503 : 200;
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

/* ── Phase 5 backend declarations ────────────────────────────────────────── */
/* index.h and memory.h are already included via aimee.h; use their real types.
 * db2/artifacts.h is NOT in the aimee.h chain, so declare those types locally. */

extern char *kb_search_json_ex(const char *project, const char *query, const char *embedding_cmd,
                               int max_results, const char *fusion_mode_override);
extern char *kb_search_json_scoped_ex(const char *preferred_project, int all_projects,
                                      const char *query, const char *embedding_cmd, int max_results,
                                      const char *fusion_mode_override);

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

/* printf-append into buf at pos, returning the new pos clamped to [0, cap]. snprintf returns
 * the would-be length, so the raw `pos += snprintf(...)` idiom can run pos PAST cap; a later
 * (cap - pos) then wraps to a huge size_t and the next write lands out of bounds. The JSON
 * builders' per-record headroom guards (pos + 64 < cap) route every append through this clamp. */
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

static int json_body_error(char *out_buf, int out_cap, int status, const char *message);

/* Extract a JSON string field value from body into out. */

/* Resolve POST /v1/search scope before either the facet or ranked branch.
 * Omission is local/current, never all: a verified project credential can
 * supply current; otherwise the caller must provide a stable project id.
 * Cross-project search remains available only as explicit scope=all. */
static int kb_search_project_scope(const char *body, char *project, size_t project_cap,
                                   int *all_projects, char *out_buf, int out_cap)
{
   project[0] = '\0';
   *all_projects = 0;
   cJSON *root = cJSON_Parse(body ? body : "{}");
   if (!root)
      return json_body_error(out_buf, out_cap, 400, "invalid json");
   const cJSON *jp = cJSON_GetObjectItemCaseSensitive(root, "project");
   const cJSON *js = cJSON_GetObjectItemCaseSensitive(root, "scope");
   const char *scope = cJSON_IsString(js) ? js->valuestring : "current";
   if (js && !cJSON_IsString(js))
   {
      cJSON_Delete(root);
      return json_body_error(out_buf, out_cap, 400, "scope must be current or all");
   }
   if (strcmp(scope, "current") != 0 && strcmp(scope, "all") != 0)
   {
      cJSON_Delete(root);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":{\"type\":\"invalid_scope\",\"message\":\"scope must be current or "
               "all\"}}");
      return 400;
   }
   if (cJSON_IsString(jp) && jp->valuestring[0])
   {
      if (strlen(jp->valuestring) >= project_cap)
      {
         cJSON_Delete(root);
         return json_body_error(out_buf, out_cap, 400, "project id is too long");
      }
      snprintf(project, project_cap, "%s", jp->valuestring);
   }

   const char *verified_kind = NULL;
   const char *verified_id = NULL;
   int verified = kb_reqctx_verified_scope(&verified_kind, &verified_id);
   if (strcmp(scope, "all") == 0)
   {
      cJSON_Delete(root);
      if (verified)
      {
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":{\"type\":\"forbidden\",\"message\":\"a scoped credential "
                  "cannot search all projects\"}}");
         return 403;
      }
      *all_projects = 1;
      return 0;
   }

   if (!project[0] && verified && strcmp(verified_kind, "project") == 0)
      snprintf(project, project_cap, "%s", verified_id);
   if (project[0] && verified && strcmp(verified_kind, "project") == 0 &&
       strcmp(project, verified_id) != 0)
   {
      cJSON_Delete(root);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":{\"type\":\"forbidden\",\"message\":\"project is outside the "
               "verified credential scope\"}}");
      return 403;
   }
   cJSON_Delete(root);
   if (project[0])
      return 0;
   snprintf(out_buf, (size_t)out_cap,
            "{\"error\":{\"type\":\"scope_required\",\"message\":\"no active project is "
            "available; pass project or scope=all explicitly\"}}");
   return 409;
}

/* Extract a JSON integer field from body. Returns default_val if not found. */

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
   /* ALWAYS emitted, including zero. "processed: 14, failed: 0" read as a fully
    * processed document when eight of those fourteen transitions did nothing; an
    * absent field would leave the same impression for anyone who did not know to
    * look for it. See db2_corpus_pipeline_stats_t.skipped. */
   cJSON_AddNumberToObject(root, "skipped", stats->skipped);
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

/* Shared 403 for owner-credential-only mutations (a scoped token must not reach them). */
static int kb_http_owner_required(char *out, int cap, const char *what)
{
   snprintf(out, (size_t)cap, "{\"error\":\"forbidden: %s requires the owner credential\"}", what);
   return 403;
}

/* ── webchat-project-lifecycle slice 2: purge-route helpers ─────────────── */

/* Append one purge fan-out store outcome: a plain count on success (0 for the
 * primitives that report no count), {"error":...} on failure. A failing store
 * clears *all_ok but never stops the fan-out. */
static void purge_store_add(cJSON *stores, const char *name, int rc, int *all_ok)
{
   if (rc < 0)
   {
      cJSON *e = cJSON_CreateObject();
      if (e)
         cJSON_AddStringToObject(e, "error", "delete failed");
      cJSON_AddItemToObject(stores, name, e);
      *all_ok = 0;
      return;
   }
   cJSON_AddNumberToObject(stores, name, rc);
}

/* Serialize `resp` into out_buf and delete it. Returns `status`. */
static int purge_respond(cJSON *resp, char *out_buf, int out_cap, int status)
{
   char *out = resp ? cJSON_PrintUnformatted(resp) : NULL;
   snprintf(out_buf, (size_t)out_cap, "%s", out ? out : "{\"error\":\"out of memory\"}");
   free(out);
   cJSON_Delete(resp);
   return out ? status : 500;
}

/* Parse the shared purge-route body {project, generation, purge_id}. Returns 0
 * on success; otherwise writes a 400 body and returns -1. Generation/purge_id
 * become the space-separated fence value, so embedded spaces are rejected. */
static int purge_body_parse(const char *body, char *project, size_t project_cap, char *generation,
                            size_t generation_cap, char *purge_id, size_t purge_id_cap,
                            char *out_buf, int out_cap)
{
   project[0] = generation[0] = purge_id[0] = '\0';
   if (!kb_http_json_str(body, "project", project, project_cap) || !project[0])
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing project\"}");
      return -1;
   }
   if (!kb_http_json_str(body, "generation", generation, generation_cap) || !generation[0] ||
       strchr(generation, ' '))
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing or invalid generation\"}");
      return -1;
   }
   if (!kb_http_json_str(body, "purge_id", purge_id, purge_id_cap) || !purge_id[0] ||
       strchr(purge_id, ' '))
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing or invalid purge_id\"}");
      return -1;
   }
   return 0;
}

/* Current fence rendered as its stored "generation purge_id" value (empty when
 * no fence row exists) — echoed by heartbeat/finalize/cancel mismatch paths. */
static void purge_fence_current(const char *project, char *out, size_t out_cap)
{
   char gen[128] = "", pid[128] = "";
   out[0] = '\0';
   if (db2_kb_purge_fence_read(project, gen, sizeof(gen), pid, sizeof(pid), NULL) == 1)
      snprintf(out, out_cap, "%s %s", gen, pid);
}
/* ── Phase 5 extended routing ────────────────────────────────────────────── */

int kb_http_route_ex(const char *method, const char *path, const char *query_string,
                     const char *auth_header, const char *bearer_token, const char *body,
                     int body_len, char *out_buf, int out_cap)
{
   /* Credential bootstrap, both pre-auth: the login surface is how a caller with
    * no credential gets one (§3), and the enrollment token IS the credential.
    * See kb_http_bootstrap.h. */
   int br = kb_http_bootstrap_route(method, path, query_string, body, (int64_t)time(NULL), out_buf,
                                    out_cap);
   if (br >= 0)
      return br;

   /* P9a telemetry scrape/ingest TOKEN path: the dedicated token authorizes GET
    * /v1/metrics + POST /v1/telemetry/metrics WITHOUT the kb bearer, so it runs
    * BEFORE the bearer gate; a miss returns -1 and falls through to admin auth. */
   {
      const char *presented = aimee_core_bearer_token(auth_header);
      if (!presented)
         presented = "";
      int tk = kb_http_telemetry_token_route(method, path, query_string, body, presented, out_buf,
                                             out_cap);
      if (tk >= 0)
         return tk;
   }

   /* Auth + scope authorization via the pluggable Verifier seam (kb_verifier.h): the built-in
    * kb-token verifier validates the configured bearer (which may be self-describing
    * "scope:<kind>:<id>:<secret>") and yields the verified scope. Per verify-then-trust, the
    * cross-scope check uses that verified scope, never the caller's. `vr` is function-scoped so
    * owner-only routes (e.g. /v1/enroll) tell an unscoped owner credential from a scoped one. */
   /* The container healthcheck must keep working once a bearer is sealed. It is
    * a LOOPBACK GET of /v1/health with no query string, from inside the
    * container, and presents no credential — it cannot, since the bearer lives
    * in the Vault.
    *
    * The exemption is deliberately three conditions, not one. /v1/health also
    * answers ?status=1&project=<p>, whose scope is enforced against the CLIENT
    * CERTIFICATE, so a cross-scope query must still be refused (test_mtls_serve);
    * and a wrong bearer on /v1/health must still be 401 for any non-local caller
    * (test_scope_token_secret_auth). An unknown peer is not local, so a direct
    * router call — every unit test, and the mTLS listener — is never exempt. */
   int local_liveness_probe = (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) &&
                              strcmp(path, "/v1/health") == 0 &&
                              (!query_string || !query_string[0]) &&
                              kb_login_throttle_peer_is_loopback();

   kb_verify_result_t vr;
   memset(&vr, 0, sizeof(vr));
   /* Reset any prior request's actor on this worker thread; a request that fails
    * auth below leaves no actor (fail-closed) for tenant-aware handlers. */
   kb_reqctx_clear();
   char vr_which[32] = "";
   /* No owner actor is manufactured in auth-off mode: the tenancy mutation routes
    * require a real authenticated principal, so an auth-off deployment cannot make
    * anonymous admin writes. */
   if (!local_liveness_probe && bearer_token && bearer_token[0])
   {
      const char *presented = aimee_core_bearer_token(auth_header);
      if (!presented)
         presented = "";
      if (!kb_verifier_authenticate(presented, bearer_token, &vr, vr_which, sizeof(vr_which)))
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"unauthorized\"}");
         return 401;
      }
      kb_reqctx_set_verified_scope(vr.scope_kind, vr.scope_id);

      /* Build the request's authenticated ACTOR principal for tenant-aware handlers
       * (P1 slice 4). OIDC -> issuer-scoped (iss,sub); an UNSCOPED kb-token is the
       * install owner (the bootstrap operator); a scoped kb-token is a limited
       * service credential, not a tenancy actor, so no actor is set. */
      {
         kb_principal_t actor;
         memset(&actor, 0, sizeof(actor));
         if (strcmp(vr_which, "oidc") == 0)
         {
            /* OIDC: the issuer is required to form the issuer-scoped (iss,sub)
             * identity — if it isn't configured or resolution fails, set no actor
             * rather than a mis-scoped one. */
            char issuer[256] = "";
            if (kb_oidc_configured_issuer(issuer, sizeof(issuer)) == 0 && issuer[0])
               if (kb_principal_from_verify(&vr, issuer, &actor) != 0)
                  memset(&actor, 0, sizeof(actor));
         }
         else if (!vr.scope_kind[0])
         {
            /* Unscoped kb-token = the install owner (bootstrap operator). */
            if (kb_principal_from_verify(&vr, "", &actor) != 0)
               memset(&actor, 0, sizeof(actor));
         }
         if (actor.authenticated)
            kb_reqctx_set_actor(&actor);
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

      /* Event-bus control-web decisions are authoritative and fail closed. */
      if (vr.scope_kind[0] && strcmp(vr.scope_kind, KB_SCOPE_KIND_CONSOLE_ADMIN) == 0)
      {
         int allowed = 0;
         if (kb_route_acl_console_admin_authorize(method, path, &allowed) != 0)
            return json_body_error(out_buf, out_cap, 503, "control-web authorization unavailable");
         if (!allowed)
            return json_body_error(out_buf, out_cap, 403,
                                   "forbidden: console-admin credential not permitted");
      }
   }
   /* Tenancy routes (P1 slice 4): /v1/team*, /v1/project*. Reachable for any
    * authenticated caller; the org-admin capability for writes is enforced at the
    * DB layer (RLS write policies), and reads are RLS-scoped to the caller's teams.
    * The handler returns -1 for non-tenancy paths so the router falls through. */
   {
      int tr = kb_http_team_route(method, path, query_string, body, out_buf, out_cap);
      if (tr >= 0)
         return tr;
      tr = kb_http_servers_route_ex(method, path, query_string, body,
                                    body_len > 0 ? (size_t)body_len : 0, out_buf, out_cap);
      if (tr >= 0)
         return tr;
      /* Write-tier grant administration (increment 5). Authorization is the DB layer's
       * admin-or-team-lead check plus the server's UDS-only /v1 route; nothing is
       * enforced here. See kb_http_grants.h. */
      tr = kb_http_grants_route(method, path, query_string, body, out_buf, out_cap);
      if (tr >= 0)
         return tr;
   }
   /* Model catalog + entitlement routes (P2a): /v1/models/entitled (tenant read) +
    * the /v1/models/org/ admin CRUD (admin-gated at the DB layer, WORM-audited).
    * Returns -1 for non-models paths so the router falls through. */
   {
      int mr = kb_http_models_route(method, path, body, out_buf, out_cap);
      if (mr >= 0)
         return mr;
   }

   /* Org spend reporting (P3b): GET /v1/insights/spend. Any authenticated caller may
    * ask; the org-admin-OR-team-lead authorization is enforced at the DB layer inside
    * the SECURITY DEFINER org_spend_query() (a non-authorized caller surfaces as 403).
    * Returns -1 for non-insights paths so the router falls through. */
   {
      int ir = kb_http_insights_route(method, path, query_string, out_buf, out_cap);
      if (ir >= 0)
         return ir;
   }

   /* Budget admin routes (P4a): POST /v1/budget/set (org-admin) + GET /v1/budget/show
    * (org-admin OR team-lead). Authorization is enforced at the DB layer inside the
    * SECURITY DEFINER org_budget_set / org_budget_show (a non-authorized caller surfaces
    * as 403). Returns -1 for non-budget paths so the router falls through. */
   {
      int br = kb_http_budget_route(method, path, query_string, body, out_buf, out_cap);
      if (br >= 0)
         return br;
   }

   /* Rate-limit admin routes (P4b): POST /v1/rate/policy (org-admin) + GET /v1/rate/show
    * (org-admin OR team-lead). Authorization is enforced at the DB layer inside the
    * SECURITY DEFINER org_rate_policy_set / org_rate_policy_show (a non-authorized caller
    * surfaces as 403). org_rate_check (egress enforcement) is not routed here (P2b).
    * Returns -1 for non-rate paths so the router falls through. */
   {
      int rr = kb_http_rate_route(method, path, query_string, body, out_buf, out_cap);
      if (rr >= 0)
         return rr;
   }

   /* Telemetry export + ingest admin routes (P9a): the org-admin path for
    * /v1/metrics and /v1/telemetry (the token path ran pre-gate). DB-enforced. */
   {
      int tr = kb_http_telemetry_route(method, path, query_string, body, out_buf, out_cap);
      if (tr >= 0)
         return tr;
   }

   /* Console + accounts routes. Served only to the owner (unscoped credential) or
    * a console-admin bearer — never to some other scope kind, so a project-scoped
    * client cannot reach the console surface. (In an auth-off deployment vr is
    * zeroed = owner, consistent with everything open.) */
   if (!vr.scope_kind[0] || strcmp(vr.scope_kind, KB_SCOPE_KIND_CONSOLE_ADMIN) == 0)
   {
      int cr = kb_http_console_route(method, path, body, out_buf, out_cap);
      if (cr >= 0)
         return cr;
      int ar = kb_http_accounts_route(method, path, query_string, body, out_buf, out_cap);
      if (ar >= 0)
         return ar;
      int gr = kb_http_governance_route(method, path, query_string, body, out_buf, out_cap);
      if (gr >= 0)
         return gr;
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
      /* Owner mints, and the console-admin credential may also mint (enroll-a-
       * client is a console accounts action). Any other scope is rejected. */
      if (vr.scope_kind[0] && strcmp(vr.scope_kind, KB_SCOPE_KIND_CONSOLE_ADMIN) != 0)
         return kb_http_owner_required(out_buf, out_cap, "enrollment minting");
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
      /* A console-admin caller may mint only a properly-scoped CLIENT credential
       * — never an owner/full-access cert (a scope with no ':' gets full access
       * at the mTLS seam) and never a privileged kind (console-admin/curator/
       * owner). This bounds the console: it cannot escalate by minting. The owner
       * credential keeps unrestricted minting. */
      if (vr.scope_kind[0] && strcmp(vr.scope_kind, KB_SCOPE_KIND_CONSOLE_ADMIN) == 0)
      {
         const char *colon = strchr(scope, ':');
         size_t kindlen = colon ? (size_t)(colon - scope) : 0;
         int privileged = !colon /* owner / full-access */ ||
                          (kindlen == 13 && strncmp(scope, "console-admin", 13) == 0) ||
                          (kindlen == 7 && strncmp(scope, "curator", 7) == 0) ||
                          (kindlen == 7 && strncmp(scope, "service", 7) == 0) ||
                          (kindlen == 5 && strncmp(scope, "owner", 5) == 0);
         if (privileged)
         {
            cJSON_Delete(req);
            snprintf(
                out_buf, (size_t)out_cap,
                "{\"error\":\"forbidden: console-admin may not mint an owner or privileged scope; "
                "use a scoped '<kind>:<id>' value\"}");
            return 403;
         }
      }
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

   if (strcmp(path, "/v1/intelligence/ranker/export-view") == 0)
   {
      if (strcmp(method, "GET") != 0)
         return json_body_error(out_buf, out_cap, 405, "method not allowed");
      cJSON *resp = kb_intel_ranker_export_view_response();
      char *json = resp ? cJSON_PrintUnformatted(resp) : NULL;
      cJSON_Delete(resp);
      return json_copy_response(out_buf, out_cap, json);
   }

   if (strcmp(path, "/v1/intelligence/ranker/fit") == 0)
      return strcmp(method, "POST") == 0
                 ? kb_intel_ranker_fit_http(body, body_len, out_buf, out_cap)
                 : json_body_error(out_buf, out_cap, 405, "method not allowed");

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
      if (!kb_http_json_str(body, "topic", topic, sizeof(topic)) || !topic[0])
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
      if (!kb_http_json_str(body, "topic", topic, sizeof(topic)) || !topic[0])
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
      /* Escape hatch: force-clear a stuck reembed_in_progress marker (non-destructive, resumes
       * search; available even when reembed_on_dim_change is off). 409 on dim mismatch unless
       * force. */
      if (kb_http_json_bool(body, "clear_maintenance", 0))
      {
         int was = 0, recorded = 0, running = 0;
         int cforce = kb_http_json_bool(body, "force", 0);
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
      if (!config_kb_reembed_on_dim_change())
      {
         /* Name the file. This gate lives in aimee-kb's OWN config, not the
          * server's, and `aimee config set` targets the server -- so an operator
          * following the documented re-embed had no way to act on "set it true".
          * The path is what makes this message useful. */
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"kb.reembed_on_dim_change is disabled; set 'kb: "
                  "reembed_on_dim_change: true' in aimee-kb's own $AIMEE_HOME/aimee.yaml and "
                  "restart it (this gate is read by aimee-kb, not by aimee-server, so `aimee "
                  "config set` does not reach it)\"}");
         return 403;
      }
      int confirm = kb_http_json_bool(body, "confirm", 0);
      int force = kb_http_json_bool(body, "force", 0);
      int dry = kb_http_json_bool(body, "dry_run", 0) || !confirm; /* no confirm => dry-run */
      /* Target precedence: explicit operator target_dim (authoritative, bypasses the
       * probe) > operator pin > the embedder's CURRENT dim via probe (after a model
       * swap, db2_embedding_dim() still reports the old/recorded value). */
      int target = kb_http_json_int(body, "target_dim", 0);
      if (target <= 0)
      {
         if (config_embedder_dims_pinned_current())
            target = config_resolve_embedder_dims_current();
         else if (db2_probe_embedder_dim(8000, &target) != 0 || target <= 0)
         {
            snprintf(out_buf, (size_t)out_cap,
                     "{\"error\":\"could not determine the target dim from the embedder; pass "
                     "target_dim, pin EMBEDDER_DIMS, or ensure the embedder /health is "
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
      int limit = kb_http_json_int(body, "limit", 20);
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
      if (!kb_http_json_str(body, "query", query, sizeof(query)) || !query[0])
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing query\"}");
         return 400;
      }

      char project[256] = "";
      int all_projects = 0;
      int scope_status =
          kb_search_project_scope(body, project, sizeof(project), &all_projects, out_buf, out_cap);
      if (scope_status)
         return scope_status;

      /* Typed-facet artifact path (deep-curator): when a `filters` object is
       * present, narrow over the artifact narrative payload and return only
       * matching artifacts (filter precision = 1.0). Returns -1 to fall through
       * to the default ranked search when no filters were supplied. */
      {
         int fr = kb_http_search_facets(body, project, all_projects, out_buf, out_cap);
         if (fr >= 0)
            return fr;
      }

      char fusion_mode[64] = "";
      kb_http_json_str(body, "fusion_mode", fusion_mode, sizeof(fusion_mode));
      int max_results = kb_http_json_int(body, "max_results", 10);
      if (max_results < 1)
         max_results = 1;
      if (max_results > 100)
         max_results = 100;

      /* Embed the query with the SAME embedder as the corpus. Passing NULL
       * here falls back to "builtin", so a corpus produced by the managed LLM
       * would be queried with a 384-dimensional hash vector. Resolve the full
       * deployment default (explicit command, EMBEDDER_URL, or
       * SYNTHESIS_ENDPOINT), not just the raw config field. */
      char embed_cmd[256] = "";
      kb_http_json_str(body, "embedding_command", embed_cmd, sizeof(embed_cmd));
      if (!embed_cmd[0])
      {
         if (config_present())
            snprintf(embed_cmd, sizeof(embed_cmd), "%s", config_embedder_command_current(NULL));
      }
      char *raw =
          kb_search_json_scoped_ex(project, all_projects, query, embed_cmd[0] ? embed_cmd : NULL,
                                   max_results, fusion_mode[0] ? fusion_mode : NULL);
      if (!raw)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"search unavailable\"}");
         return 503;
      }

      char used_mode[64] = "rrf";
      kb_http_json_str(raw, "fusion_mode", used_mode, sizeof(used_mode));

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
            char hit_project[256] = "";
            char content[512] = "";
            char score_s[32] = "0.0";

            const char *pj = strstr(rp, "\"project\":\"");
            if (pj && pj < obj_end)
            {
               pj += 11;
               size_t i = 0;
               while (*pj && *pj != '"' && i < sizeof(hit_project) - 1)
               {
                  if (*pj == '\\' && *(pj + 1))
                     pj++;
                  hit_project[i++] = *pj++;
               }
               hit_project[i] = '\0';
            }

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

            /* doc_id keys the row to its feature_rows for the learning-to-rank
             * outcome capture; additive, existing consumers ignore it. */
            char doc_id_s[32] = "0";
            const char *di = strstr(rp, "\"doc_id\":");
            if (di && di < obj_end)
            {
               di += 9;
               while (*di == ' ')
                  di++;
               size_t i = 0;
               while (i < sizeof(doc_id_s) - 1 && ((*di >= '0' && *di <= '9') || *di == '-'))
                  doc_id_s[i++] = *di++;
               doc_id_s[i] = '\0';
               if (!doc_id_s[0])
                  doc_id_s[0] = '0', doc_id_s[1] = '\0';
            }

            if (hit_count > 0)
               out_buf[pos++] = ',';
            pos = js_appendf(out_buf, pos, out_cap, "{\"artifact_id\":\"");
            pos = json_escape(fp, out_buf, pos, out_cap);
            pos = js_appendf(out_buf, pos, out_cap, "\",\"project\":\"");
            pos = json_escape(hit_project, out_buf, pos, out_cap);
            pos = js_appendf(out_buf, pos, out_cap,
                             "\",\"score\":%s,\"doc_id\":%s,\"kind\":\"doc_chunk\",\"excerpt\":\"",
                             score_s[0] ? score_s : "0.0", doc_id_s);
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
   if (strcmp(path, "/v1/code/hybrid") == 0)
      return handle_get_code_hybrid_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/code/context") == 0)
      return handle_get_code_context_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/code/graph/hubs") == 0)
      return handle_get_code_graph_hubs_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/code/graph/surprising") == 0)
      return handle_get_code_graph_surprising_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/code/graph/audit") == 0)
      return handle_get_code_graph_audit_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/code/graph/diff") == 0)
      return handle_get_code_graph_diff_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/code/lessons") == 0)
      return handle_get_code_lessons_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/code/graph") == 0)
      return handle_get_code_graph_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/pdf/search") == 0)
      return handle_get_pdf_search_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/pdf/page") == 0)
      return handle_get_pdf_page_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/pdf/neighbors") == 0)
      return handle_get_pdf_neighbors_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/pdf/structure") == 0)
      return handle_get_pdf_structure_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/pdf/lookup_table") == 0)
      return handle_get_pdf_lookup_table_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/pdf/assets") == 0)
      return handle_get_pdf_assets_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/pdf/open_asset") == 0)
      return handle_get_pdf_open_asset_route(method, query_string, out_buf, out_cap);
   /* POST /v1/pdf/quarantine: owner-only release/purge; auth checked here (scope vr lives here). */
   if (strcmp(path, "/v1/pdf/quarantine") == 0)
   {
      if (vr.scope_kind[0])
         return kb_http_owner_required(out_buf, out_cap, "quarantine actions");
      return handle_post_pdf_quarantine_route(method, body, body_len, out_buf, out_cap);
   }
   if (strcmp(path, "/v1/code/callers") == 0)
      return handle_get_code_callers_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/code/project-stats") == 0)
      return handle_get_code_project_stats_route(method, query_string, out_buf, out_cap);
   if (strcmp(path, "/v1/code/cross-repo-deps") == 0)
      return handle_get_code_cross_repo_deps_route(method, query_string, out_buf, out_cap);
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
      if (!kb_http_json_str(body, "path", kb_path, sizeof(kb_path)) || !kb_path[0])
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing path\"}");
         return 400;
      }
      if (!kb_http_json_str(body, "project", project, sizeof(project)) || !project[0])
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing project\"}");
         return 400;
      }
      (void)kb_http_json_str(body, "embedding_command", embed_cmd, sizeof(embed_cmd));
      if (!embed_cmd[0])
      {
         snprintf(embed_cmd, sizeof(embed_cmd), "%s", config_embedder_command_current(NULL));
      }
      int force = kb_http_json_bool(body, "force", 0);

      if (!db2_is_initialized())
      {
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"failed to open knowledge service store\"}");
         return 503;
      }
      int kb_embed_dim = db2_embedding_dim();
      if (kb_embed_dim <= 0 || kb_embed_dim > EMBED_MAX_DIM)
         kb_embed_dim = 1024;
      if (pgvec_kb_service_ensure_kb_collection(kb_embed_dim) != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"vector store unavailable\"}");
         return 503;
      }

      /* Queue it. Do not do it here.
       *
       * A build is a scan plus doc embedding plus code embedding, and its cost
       * is a property of the corpus, not of the service being healthy: on a
       * 3825-file checkout that is minutes of embedder time. Doing it inline
       * made the caller hold an HTTP request open for the whole of it, and the
       * first bound to expire anywhere in that chain turned a build that was
       * progressing normally into a hard failure -- observed as
       * "knowledge service /v1/code/build did not respond" with the embedder
       * logging BrokenPipeError, after the kb dropped a connection whose embed
       * batch had simply taken longer than the client's patience.
       *
       * Embedding is asynchronous by design. It completes when it completes,
       * a second from now or a day from now, and nothing waits on it. The
       * queue worker performs the SAME build -- doc vectors, canonical index,
       * and code vectors -- so queueing loses no work.
       *
       * INTERACTIVE priority is what makes this safe to queue: an explicit
       * build request jumps the periodic sweep instead of sitting behind it.
       * Starvation behind the global backlog is what made someone inline this
       * work in the first place; priority is the fix for that, not blocking. */
      int queued =
          db2_kb_ingest_queue_enqueue(project, kb_path, "", force, DB2_KB_INGEST_PRIO_INTERACTIVE);
      if (queued < 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"could not queue build\"}");
         return 503;
      }
      /* No explicit wake: the workers park on a 2s timed wait and drain the
       * queue, which is how /v1/code/scan already hands off. */
      snprintf(out_buf, (size_t)out_cap,
               "{\"status\":\"ok\",\"queued\":true,\"project\":\"%s\","
               "\"files_indexed\":0,\"chunks_added\":0,\"embeddings_added\":0,"
               "\"reason\":\"queued\"}",
               project);
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
      if (!kb_http_json_str(body, "path", kb_path, sizeof(kb_path)) || !kb_path[0])
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing path\"}");
         return 400;
      }
      if (!kb_http_json_str(body, "project", project, sizeof(project)) || !project[0])
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing project\"}");
         return 400;
      }
      (void)kb_http_json_str(body, "embedding_command", embed_cmd, sizeof(embed_cmd));
      if (!embed_cmd[0])
      {
         snprintf(embed_cmd, sizeof(embed_cmd), "%s", config_embedder_command_current(NULL));
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
         if (config_kb_curator_extract_docs_enabled())
            kb_curator_queue_docs_for_project(project);
      }
      kb_http_write_build_stats(out_buf, out_cap, project, &stats);
      return 200;
   }

   if (strcmp(path, "/v1/code/scan") == 0)
      return handle_post_code_scan_route(method, body, out_buf, out_cap);
   if (strcmp(path, "/v1/code/project/detach") == 0)
      return handle_post_code_project_lifecycle_route(
          method, "detach", body, out_buf, out_cap, kb_reqctx_actor() != NULL && !vr.scope_kind[0]);
   if (strcmp(path, "/v1/code/project/purge") == 0)
      return handle_post_code_project_lifecycle_route(
          method, "purge", body, out_buf, out_cap, kb_reqctx_actor() != NULL && !vr.scope_kind[0]);
   if (strcmp(path, "/v1/code/project/gc") == 0)
      return handle_post_code_project_lifecycle_route(
          method, "gc", body, out_buf, out_cap, kb_reqctx_actor() != NULL && !vr.scope_kind[0]);
   if (strcmp(path, "/v1/code/lessons/observe") == 0)
      return handle_post_code_lessons_observe_route(method, body, out_buf, out_cap);
   if (strcmp(path, "/v1/code/repo-trust") == 0) /* S7: admin; owner gate in handler */
      return handle_post_code_repo_trust_route(method, body, out_buf, out_cap, !vr.scope_kind[0]);
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

      char workspace[MAX_PATH_LEN] = "";
      (void)kb_http_json_str(body, "workspace", workspace, sizeof(workspace));
      int force = kb_http_json_bool(body, "force", 0);
      int use_all = !workspace[0] || strcmp(workspace, "all") == 0;

      /* An absent or "all" workspace names NO scope, so the scope layer above
       * has nothing to deny against — and with force:true this clears the
       * vectors, kb rows and file index of every project in every configured
       * workspace. A scoped credential must not reach that: same rule as
       * "a scoped credential cannot search all projects" on /v1/search.
       * A `service` credential MAY: indexing the whole deployment is the job it
       * exists for, and it is still refused every administrative route. */
      if (use_all && vr.scope_kind[0] && strcmp(vr.scope_kind, KB_SCOPE_KIND_SERVICE) != 0)
      {
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"forbidden: a scoped credential cannot ingest all projects; "
                  "name a workspace\"}");
         return 403;
      }

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
            char pname[256];
            char pws[256];
            if (workspace_repo_index_keys(projects[i], workspace, pname, sizeof(pname), pws,
                                          sizeof(pws)) != 0)
               continue;
            if (force)
            {
               pgvec_kb_vector_delete_current_project(pname);
               db2_kb_service_clear_current_project(pname);
               db2_kb_file_index_delete_current_project(pname);
            }
            db2_kb_ingest_queue_enqueue(pname, projects[i], pws, force,
                                        DB2_KB_INGEST_PRIO_INTERACTIVE);
            total_queued++;
         }
      }
      else
      {
         for (int w = 0; w < config_workspace_count(); w++)
         {
            int n = workspace_discover_projects(config_workspaces(w), 3, projects,
                                                MAX_DISCOVERED_PROJECTS);
            for (int i = 0; i < n; i++)
            {
               char pname[256];
               char pws[256];
               if (workspace_repo_index_keys(projects[i], config_workspaces(w), pname,
                                             sizeof(pname), pws, sizeof(pws)) != 0)
                  continue;
               if (force)
               {
                  pgvec_kb_vector_delete_current_project(pname);
                  db2_kb_service_clear_current_project(pname);
                  db2_kb_file_index_delete_current_project(pname);
               }
               db2_kb_ingest_queue_enqueue(pname, projects[i], pws, force,
                                           DB2_KB_INGEST_PRIO_INTERACTIVE);
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
      int limit = kb_http_json_int(body, "limit", 0);
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
      (void)kb_http_json_str(body, "embedding_command", embed_cmd, sizeof(embed_cmd));
      if (!embed_cmd[0])
      {
         snprintf(embed_cmd, sizeof(embed_cmd), "%s", config_embedder_command_current(NULL));
      }
      int timeout = kb_http_json_int(body, "timeout", 0);
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
   /* Destructive as a family; see kb_route_acl.h for why this is by prefix. */
   if (kb_route_acl_is_maintenance(path) && !(kb_reqctx_actor() != NULL && !vr.scope_kind[0]))
      return kb_http_owner_required(out_buf, out_cap, "knowledge maintenance");

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
      if (!kb_http_json_str(body, "path", kb_path, sizeof(kb_path)) || !kb_path[0])
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing path\"}");
         return 400;
      }
      if (!kb_http_json_str(body, "project", project, sizeof(project)) || !project[0])
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing project\"}");
         return 400;
      }
      (void)kb_http_json_str(body, "embedding_command", embed_cmd, sizeof(embed_cmd));
      if (!embed_cmd[0])
      {
         snprintf(embed_cmd, sizeof(embed_cmd), "%s", config_embedder_command_current(NULL));
      }
      int kb_embed_dim = db2_embedding_dim();
      if (kb_embed_dim <= 0 || kb_embed_dim > EMBED_MAX_DIM)
         kb_embed_dim = 1024;
      if (pgvec_kb_service_ensure_kb_collection(kb_embed_dim) != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"vector store unavailable\"}");
         return 503;
      }
      if (!db2_is_initialized())
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"knowledge store unavailable\"}");
         return 503;
      }
      /* Repair queues for the same reason build does: it embeds, and embedding is
       * asynchronous, full stop. An operator asking for a repair gets a durable
       * commitment that it will happen, not an HTTP request held open across
       * minutes of embedder time that reports failure the moment any bound in
       * the chain expires. force=1 is preserved as the queued job's force flag. */
      kb_stats_t stats;
      memset(&stats, 0, sizeof(stats));
      if (db2_kb_ingest_queue_enqueue(project, kb_path, "", 1, DB2_KB_INGEST_PRIO_INTERACTIVE) < 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"could not queue knowledge repair\"}");
         return 503;
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
      int dry_run = kb_http_json_bool(body, "dry_run", 0);
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
      if (!kb_http_json_str(body, "project", project, sizeof(project)) || !project[0])
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
   /* POST /v1/maintenance/purge-project {project, generation, purge_id, takeover?}
    * (webchat-project-lifecycle slice 2). Writes the generation fence, then fans
    * out the writer→store→delete matrix IN ORDER, continuing past per-store
    * failures. The fence is NOT cleared here — /v1/maintenance/purge-finalize
    * (or purge-cancel) clears it after the server-side deletion completes.
    * Idempotent: a re-run on a purged key returns zeros. */
   if (strcmp(path, "/v1/maintenance/purge-project") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      char project[256], generation[128], purge_id[128];
      if (purge_body_parse(body, project, sizeof(project), generation, sizeof(generation), purge_id,
                           sizeof(purge_id), out_buf, out_cap) != 0)
         return 400;
      int takeover = kb_http_json_bool(body, "takeover", 0);

      /* Atomic read-decide-write: one transaction takes the project advisory
       * guard, inspects the identity row FOR UPDATE, refuses a LIVE foreign
       * fence (heartbeat younger than 2x the heartbeat interval, i.e. TTL/3)
       * without takeover:true, else publishes the new fence. */
      char cur_gen[128] = "", cur_pid[128] = "";
      int fence_replaced = 0;
      int arc =
          db2_kb_purge_fence_acquire(project, generation, purge_id, takeover, cur_gen,
                                     sizeof(cur_gen), cur_pid, sizeof(cur_pid), &fence_replaced);
      if (arc < 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"fence write failed\"}");
         return 500;
      }
      if (arc == 0)
      {
         cJSON *resp = cJSON_CreateObject();
         if (resp)
         {
            cJSON_AddStringToObject(resp, "error", "purge fence held");
            cJSON_AddStringToObject(resp, "generation", cur_gen);
            cJSON_AddStringToObject(resp, "purge_id", cur_pid);
         }
         return purge_respond(resp, out_buf, out_cap, 409);
      }

      /* Fan-out: every kb store the ingest path writes, in matrix order. The
       * OWN fence heartbeat is refreshed between stores so a slow delete (or
       * a fan-out longer than the TTL) cannot let the fence lapse mid-purge;
       * losing ownership (a takeover displaced us) aborts the remaining
       * stores fail-closed. */
      cJSON *stores = cJSON_CreateObject();
      if (!stores)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"out of memory\"}");
         return 500;
      }
      typedef int (*purge_store_fn)(const char *);
      static const struct
      {
         const char *name;
         purge_store_fn fn;
      } purge_matrix[] = {
          {"chunks", db2_kb_service_clear_project},
          {"file_index", db2_kb_file_index_delete_project},
          {"vectors", pgvec_kb_vector_delete_project},
          {"code_embeddings", pgvec_code_delete_project},
          {"curator_code_unit_vectors", pgvec_curator_code_unit_delete_project},
          {"canonical_index", db2_code_index_project_delete},
          {"code_unit_jobs", kb_curator_code_unit_jobs_delete_project},
          {"pdf_vectors", pgvec_kbpdf_delete_project},
          {"minhash", db2_sketch_minhash_signature_delete_project},
      };
      int all_ok = 1, fence_lost = 0;
      for (size_t si = 0; si < sizeof(purge_matrix) / sizeof(purge_matrix[0]); si++)
      {
         purge_store_add(stores, purge_matrix[si].name, purge_matrix[si].fn(project), &all_ok);
         if (si + 1 < sizeof(purge_matrix) / sizeof(purge_matrix[0]) &&
             db2_kb_purge_fence_heartbeat(project, generation, purge_id) != 1)
         {
            /* Ownership lost mid-fan-out: mark the remaining stores as
             * errored (fail closed) — the displaced owner's purge re-runs
             * them idempotently. */
            fence_lost = 1;
            all_ok = 0;
            for (size_t sj = si + 1; sj < sizeof(purge_matrix) / sizeof(purge_matrix[0]); sj++)
            {
               cJSON *e = cJSON_CreateObject();
               if (e)
               {
                  cJSON_AddStringToObject(e, "error", "fence lost");
                  cJSON_AddItemToObject(stores, purge_matrix[sj].name, e);
               }
            }
            aimee_log(LOG_WARN, "kb.purge",
                      "purge-project '%s': fence ownership lost after store '%s' — remaining "
                      "stores aborted",
                      project, purge_matrix[si].name);
            break;
         }
      }

      cJSON *resp = cJSON_CreateObject();
      if (!resp)
      {
         cJSON_Delete(stores);
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"out of memory\"}");
         return 500;
      }
      cJSON_AddStringToObject(resp, "status", "ok");
      cJSON_AddBoolToObject(resp, "ok", all_ok);
      cJSON_AddStringToObject(resp, "project", project);
      cJSON_AddStringToObject(resp, "generation", generation);
      cJSON_AddStringToObject(resp, "purge_id", purge_id);
      cJSON_AddBoolToObject(resp, "fence_replaced", fence_replaced);
      if (fence_lost)
         cJSON_AddBoolToObject(resp, "fence_lost", 1);
      if (fence_replaced)
      {
         cJSON *displaced = cJSON_CreateObject();
         if (displaced)
         {
            cJSON_AddStringToObject(displaced, "generation", cur_gen);
            cJSON_AddStringToObject(displaced, "purge_id", cur_pid);
         }
         cJSON_AddItemToObject(resp, "displaced", displaced);
      }
      cJSON_AddItemToObject(resp, "stores", stores);
      return purge_respond(resp, out_buf, out_cap, 200);
   }
   /* POST /v1/maintenance/purge-heartbeat {project, generation, purge_id}:
    * the owning delete op refreshes the fence heartbeat between phases. A
    * displaced owner mismatches and no-ops (refreshed:false + current fence). */
   if (strcmp(path, "/v1/maintenance/purge-heartbeat") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      char project[256], generation[128], purge_id[128];
      if (purge_body_parse(body, project, sizeof(project), generation, sizeof(generation), purge_id,
                           sizeof(purge_id), out_buf, out_cap) != 0)
         return 400;
      int rc = db2_kb_purge_fence_heartbeat(project, generation, purge_id);
      if (rc < 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"fence heartbeat failed\"}");
         return 500;
      }
      cJSON *resp = cJSON_CreateObject();
      if (resp)
      {
         cJSON_AddStringToObject(resp, "status", "ok");
         cJSON_AddBoolToObject(resp, "refreshed", rc == 1);
         if (rc != 1)
         {
            char fence[280] = "";
            purge_fence_current(project, fence, sizeof(fence));
            cJSON_AddStringToObject(resp, "fence", fence);
         }
      }
      return purge_respond(resp, out_buf, out_cap, 200);
   }
   /* POST /v1/maintenance/purge-finalize | purge-cancel {project, generation,
    * purge_id}: clear BOTH fence rows iff generation AND purge_id match. A
    * mismatch (displaced owner) is a logged no-op returning the current fence. */
   if (strcmp(path, "/v1/maintenance/purge-finalize") == 0 ||
       strcmp(path, "/v1/maintenance/purge-cancel") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      char project[256], generation[128], purge_id[128];
      if (purge_body_parse(body, project, sizeof(project), generation, sizeof(generation), purge_id,
                           sizeof(purge_id), out_buf, out_cap) != 0)
         return 400;
      int rc = db2_kb_purge_fence_clear(project, generation, purge_id);
      if (rc < 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"fence clear failed\"}");
         return 500;
      }
      cJSON *resp = cJSON_CreateObject();
      if (resp)
      {
         cJSON_AddStringToObject(resp, "status", "ok");
         cJSON_AddBoolToObject(resp, "cleared", rc == 1);
         if (rc != 1)
         {
            char fence[280] = "";
            purge_fence_current(project, fence, sizeof(fence));
            LOG_WARN("kb_http",
                     "%s: fence mismatch for project '%s' (presented %s %s, current '%s') — no-op",
                     path, project, generation, purge_id, fence);
            cJSON_AddStringToObject(resp, "fence", fence);
         }
      }
      return purge_respond(resp, out_buf, out_cap, 200);
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
      if (!kb_http_json_str(body, "query", query, sizeof(query)) || !query[0])
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing query\"}");
         return 400;
      }
      int limit = kb_http_json_int(body, "limit", 10);
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
