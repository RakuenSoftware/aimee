/* server_http_routes_workspace.c: the /v1/workspaces routes.
 *
 * Split out of server_http_routes.c, which line-check caps at 2500 lines. These
 * three handlers and their percent-decoding helpers are self-contained and
 * nothing else in that file used them, so they were the group that could leave
 * without dragging anything with it.
 */
#include "server_http_routes_workspace.h"

#include "aimee.h"                /* MAX_PATH_LEN */
#include "server_http_internal.h" /* route_req_t */
#include "server_http.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

static int ws_hex(char c)
{
   if (c >= '0' && c <= '9')
      return c - '0';
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
   return -1;
}

/* Percent-decode `in` into `out` (cap bytes incl. NUL); a malformed %XX is
 * copied literally. Returns out. */
static char *ws_pct_decode(const char *in, char *out, size_t cap)
{
   size_t o = 0;
   for (size_t i = 0; in && in[i] && o + 1 < cap; i++)
   {
      int hi, lo;
      if (in[i] == '%' && (hi = ws_hex(in[i + 1])) >= 0 && (lo = ws_hex(in[i + 2])) >= 0)
      {
         out[o++] = (char)((hi << 4) | lo);
         i += 2;
      }
      else
      {
         out[o++] = in[i];
      }
   }
   if (cap)
      out[o] = '\0';
   return out;
}

/* Build {"method":m,"args":[arg0, extra...]} and run it through the loopback
 * bridge (same path + conn caps as rh_dispatch_op). */
static int ws_dispatch_args(const char *method, const char *arg0, const char *const *extra,
                            int extra_n, char *resp, int cap)
{
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return err_json(resp, cap, 500, "out of memory");
   cJSON_AddStringToObject(req, "method", method);
   cJSON *args = cJSON_AddArrayToObject(req, "args");
   if (arg0)
      cJSON_AddItemToArray(args, cJSON_CreateString(arg0));
   for (int i = 0; i < extra_n; i++)
      cJSON_AddItemToArray(args, cJSON_CreateString(extra[i]));
   char *line = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!line)
      return err_json(resp, cap, 500, "out of memory");
   int rc = loopback_rpc(line, (int)strlen(line), resp, cap, g_rpc_conn_caps);
   free(line);
   return rc;
}

/* POST /v1/workspaces — register {root_hint|root|path, provider?}. */
int rh_workspaces_register(const route_req_t *rq, char *resp, int cap)
{
   cJSON *body = (rq->body && rq->body[0]) ? cJSON_Parse(rq->body) : cJSON_CreateObject();
   if (!body || !cJSON_IsObject(body))
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "invalid JSON body");
   }
   const cJSON *jroot = cJSON_GetObjectItemCaseSensitive(body, "root_hint");
   if (!cJSON_IsString(jroot))
      jroot = cJSON_GetObjectItemCaseSensitive(body, "root");
   if (!cJSON_IsString(jroot))
      jroot = cJSON_GetObjectItemCaseSensitive(body, "path");
   const char *root = (cJSON_IsString(jroot) && jroot->valuestring) ? jroot->valuestring : "";
   const cJSON *jprov = cJSON_GetObjectItemCaseSensitive(body, "provider");
   const char *provider = (cJSON_IsString(jprov) && jprov->valuestring) ? jprov->valuestring : "";
   /* A `mirror` workspace is seeded by fetching the client's head from its
    * remote, so workspace.add requires both. Dropping them here (as this route
    * did) meant a mirror registration over REST was rejected for a missing
    * --remote, leaving the reverse channel no route to the sandboxed tier. */
   const cJSON *jremote = cJSON_GetObjectItemCaseSensitive(body, "remote");
   const char *remote =
       (cJSON_IsString(jremote) && jremote->valuestring) ? jremote->valuestring : "";
   const cJSON *jhead = cJSON_GetObjectItemCaseSensitive(body, "head");
   const char *head = (cJSON_IsString(jhead) && jhead->valuestring) ? jhead->valuestring : "";
   int rc;
   if (!root[0])
   {
      cJSON_Delete(body);
      return err_json(resp, cap, 400, "missing root_hint");
   }
   /* `aimee workspace prepare` marshals prepare:true onto the same method as
    * `workspace add`. Forward it, or the HTTP route silently performs a plain
    * add and the two commands become indistinguishable over the wire. */
   int prepare = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "prepare"));
   if (provider[0])
   {
      const char *extra[WS_ADD_FLAG_ARGS_MAX + 1];
      int extra_n = workspace_add_flag_args(provider, remote, head, extra, WS_ADD_FLAG_ARGS_MAX);
      if (prepare)
         extra[extra_n++] = "--prepare";
      rc = ws_dispatch_args("workspace.add", root, extra, extra_n, resp, cap);
   }
   else
   {
      const char *extra[] = {"--prepare"};
      rc = ws_dispatch_args("workspace.add", root, prepare ? extra : NULL, prepare ? 1 : 0, resp,
                            cap);
   }
   cJSON_Delete(body);
   return rc;
}

/* GET /v1/workspaces/{id} — manifest for the percent-encoded path id. */
int rh_workspace_get(const route_req_t *rq, char *resp, int cap)
{
   char path[MAX_PATH_LEN];
   ws_pct_decode(rq->id, path, sizeof(path));
   if (!path[0])
      return err_json(resp, cap, 400, "missing workspace id");
   return ws_dispatch_args("workspace.get", path, NULL, 0, resp, cap);
}

/* DELETE /v1/workspaces/{id} — deregister the percent-encoded path id. */
int rh_workspace_remove(const route_req_t *rq, char *resp, int cap)
{
   char path[MAX_PATH_LEN];
   ws_pct_decode(rq->id, path, sizeof(path));
   if (!path[0])
      return err_json(resp, cap, 400, "missing workspace id");
   return ws_dispatch_args("workspace.remove", path, NULL, 0, resp, cap);
}
