/* server/harness_memory_routes.c — op handlers for the harness_memory.* family,
 * exposed at the /v1/harness_memory/ routes (see server_http_routes.inc). Thin
 * layer over the DB1 store (db1/harness_memory.h); server is the sole DB1 writer. */

#include "server.h"

#include "cJSON.h"
#include "db1/harness_memory.h"
#include "harness_memory_common.h"
#include "json_fluent.h"

#include <stdlib.h>
#include <string.h>

static int send_and_free(server_conn_t *conn, cJSON *resp)
{
   return server_send_ok(conn, resp);
}

/* Copy src into a fixed field, returning 0 on silent truncation (1 = fit). */
static int copy_field(char *dst, size_t cap, const char *src)
{
   int n = snprintf(dst, cap, "%s", src ? src : "");
   return (n >= 0 && (size_t)n < cap);
}

static cJSON *hmem_row_json(const hmem_row_t *r)
{
   cJSON *o = cJSON_CreateObject();
   if (!o)
      return NULL;
   cJSON_AddNumberToObject(o, "id", (double)r->id);
   cJSON_AddStringToObject(o, "project", r->project);
   cJSON_AddStringToObject(o, "name", r->name);
   cJSON_AddStringToObject(o, "type", r->type);
   cJSON_AddStringToObject(o, "description", r->description ? r->description : "");
   cJSON_AddStringToObject(o, "body", r->body ? r->body : "");
   cJSON_AddStringToObject(o, "meta_json", r->meta_json ? r->meta_json : "{}");
   cJSON_AddStringToObject(o, "content_hash", r->content_hash);
   cJSON_AddStringToObject(o, "last_client", r->last_client);
   cJSON_AddStringToObject(o, "deleted_at", r->deleted_at); /* "" when live */
   cJSON_AddStringToObject(o, "created_at", r->created_at);
   cJSON_AddStringToObject(o, "updated_at", r->updated_at);
   return o;
}

int handle_hmem_upsert(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *project, *name;
   if (jo_need_str(req, "project", &project) < 0 || jo_need_str(req, "name", &name) < 0)
      return server_send_error(conn, "missing project or name", NULL);

   hmem_row_t in;
   memset(&in, 0, sizeof(in));
   /* Reject silent truncation — a truncated project/name would alias distinct
    * memories and bypass P3's path-safety (which sees the original string). */
   if (!copy_field(in.project, sizeof(in.project), project) ||
       !copy_field(in.name, sizeof(in.name), name) ||
       !copy_field(in.type, sizeof(in.type), jo_str(req, "type", "fact")) ||
       !copy_field(in.last_client, sizeof(in.last_client), jo_str(req, "client", "")) ||
       !copy_field(in.source_session, sizeof(in.source_session), jo_str(req, "session_id", "")))
      return server_send_error(conn, "field too long", NULL);
   if (!hmem_type_valid(in.type))
      return server_send_error(conn, "invalid type", NULL);
   /* borrowed pointers — hmem_upsert only reads them */
   in.description = (char *)jo_str(req, "description", "");
   in.body = (char *)jo_str(req, "body", "");
   in.meta_json = (char *)jo_str(req, "meta_json", "{}");

   int64_t id = 0;
   int rc = hmem_upsert(&in, &id);
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", rc == 0 ? "ok" : "error");
   if (rc == 0)
      cJSON_AddNumberToObject(resp, "id", (double)id);
   return send_and_free(conn, resp);
}

int handle_hmem_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *project, *name;
   if (jo_need_str(req, "project", &project) < 0 || jo_need_str(req, "name", &name) < 0)
      return server_send_error(conn, "missing project or name", NULL);

   hmem_row_t row;
   cJSON *resp;
   if (hmem_get(project, name, &row) == 0)
   {
      resp = jo_ok();
      cJSON_AddItemToObject(resp, "memory", hmem_row_json(&row));
      hmem_row_free_fields(&row);
   }
   else
   {
      resp = jo_err("not found");
   }
   return send_and_free(conn, resp);
}

/* The whole result set (bodies included) can exceed the fixed RPC response
 * buffer (SHTTP_RESP_MAX, 256 KiB) once a project accumulates enough memories —
 * the loopback RPC then truncates and the JSON fails to parse (a 502 that, for
 * `list`, aborts the session-start hydrate before it can import disk-only files).
 * So serialize a size-bounded page: rows from `offset` until the accumulated
 * payload approaches the budget, always emitting at least one row so a single
 * large memory still makes progress (it stays well under 256 KiB in practice).
 * The caller pages via {offset -> next_offset} until has_more is false. */
#define HMEM_PAGE_BUDGET (192 * 1024)

static int respond_rows_paged(server_conn_t *conn, hmem_row_t *rows, int n, int offset)
{
   if (offset < 0)
      offset = 0;
   if (offset > n)
      offset = n;
   int end = hmem_page_end(rows, n, offset, HMEM_PAGE_BUDGET);
   cJSON *resp = jo_ok();
   cJSON *arr = cJSON_CreateArray();
   for (int i = offset; i < end; i++)
      cJSON_AddItemToArray(arr, hmem_row_json(&rows[i]));
   cJSON_AddItemToObject(resp, "memories", arr);
   cJSON_AddNumberToObject(resp, "total", n);
   cJSON_AddNumberToObject(resp, "offset", offset);
   cJSON_AddNumberToObject(resp, "next_offset", end);
   cJSON_AddBoolToObject(resp, "has_more", end < n);
   hmem_rows_free(rows, n);
   return send_and_free(conn, resp);
}

int handle_hmem_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *project;
   if (jo_need_str(req, "project", &project) < 0)
      return server_send_error(conn, "missing project", NULL);
   int include_deleted = jo_int(req, "include_deleted", 0);
   int offset = jo_int(req, "offset", 0);
   hmem_row_t *rows = NULL;
   int n = 0;
   if (hmem_list(project, &rows, &n, include_deleted) != 0)
      return server_send_error(conn, "list failed", NULL);
   return respond_rows_paged(conn, rows, n, offset);
}

int handle_hmem_search(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *project, *query;
   if (jo_need_str(req, "project", &project) < 0 || jo_need_str(req, "query", &query) < 0)
      return server_send_error(conn, "missing project or query", NULL);
   int offset = jo_int(req, "offset", 0);
   hmem_row_t *rows = NULL;
   int n = 0;
   if (hmem_search(project, query, &rows, &n) != 0)
      return server_send_error(conn, "search failed", NULL);
   return respond_rows_paged(conn, rows, n, offset);
}

int handle_hmem_tombstone(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *project, *name;
   if (jo_need_str(req, "project", &project) < 0 || jo_need_str(req, "name", &name) < 0)
      return server_send_error(conn, "missing project or name", NULL);
   int rc = hmem_tombstone(project, name);
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", rc == 0 ? "ok" : "error");
   return send_and_free(conn, resp);
}

int handle_hmem_tombstone_prefix(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *project;
   if (jo_need_str(req, "project", &project) < 0)
      return server_send_error(conn, "missing project", NULL);
   const char *dir = jo_str(req, "dir", "");
   /* Empty dir tombstones the WHOLE project — never the default for a missing
    * key; require an explicit tombstone_all:true to wipe everything. */
   if (!dir[0] && !jo_int(req, "tombstone_all", 0))
      return server_send_error(conn, "missing dir (set tombstone_all:true to wipe the project)",
                               NULL);
   int rc = hmem_tombstone_prefix(project, dir);
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", rc >= 0 ? "ok" : "error");
   if (rc >= 0)
      cJSON_AddNumberToObject(resp, "tombstoned", (double)rc);
   return send_and_free(conn, resp);
}
