/* kb_scope.c: bearer-token scope parsing + per-request scope authorization.
 *
 * See src/headers/kb_scope.h. Pure string logic — no DB, no network. */

#include "kb_scope.h"

#include <stdio.h>
#include <string.h>

/* Copy at most cap-1 bytes of [src, src+n) into dst, NUL-terminating. */
static void copy_n(char *dst, size_t cap, const char *src, size_t n)
{
   if (!dst || cap == 0)
      return;
   if (n >= cap)
      n = cap - 1;
   if (src && n)
      memcpy(dst, src, n);
   dst[n] = '\0';
}

int kb_scope_token_parse(const char *token, char *scope_kind, size_t kind_len, char *scope_id,
                         size_t id_len, char *secret, size_t secret_len)
{
   if (scope_kind && kind_len)
      scope_kind[0] = '\0';
   if (scope_id && id_len)
      scope_id[0] = '\0';
   if (secret && secret_len)
      secret[0] = '\0';
   if (!token)
      return 0;

   /* Scoped form: "scope:<kind>:<id>:<secret>". Anything else is the secret. */
   if (strncmp(token, "scope:", 6) == 0)
   {
      const char *k = token + 6;
      const char *c1 = strchr(k, ':');
      if (c1)
      {
         const char *idp = c1 + 1;
         const char *c2 = strchr(idp, ':');
         if (c2 && c2 > idp)
         {
            const char *sec = c2 + 1;
            if (sec[0]) /* require a non-empty secret to treat as scoped */
            {
               copy_n(scope_kind, kind_len, k, (size_t)(c1 - k));
               copy_n(scope_id, id_len, idp, (size_t)(c2 - idp));
               copy_n(secret, secret_len, sec, strlen(sec));
               return 0;
            }
         }
      }
   }

   /* Unscoped / admin: the whole token is the secret. */
   copy_n(secret, secret_len, token, strlen(token));
   return 0;
}

int kb_scope_authorized(const char *token_kind, const char *token_id, const char *req_kind,
                        const char *req_id)
{
   /* Unscoped token = admin: full access. */
   if (!token_kind || !token_kind[0])
      return 1;
   if (!req_kind || !req_kind[0])
      return 1; /* request names no scope — nothing to deny against */
   /* A service token spans the data plane: any project, any workspace. It is
    * still scoped, so the administrative gates that refuse scoped credentials
    * continue to refuse it — this widens data access, never privilege. Other
    * kinds (user, console-admin, curator) are NOT reachable this way. */
   if (strcmp(token_kind, KB_SCOPE_KIND_SERVICE) == 0)
      return strcmp(req_kind, "project") == 0 || strcmp(req_kind, "workspace") == 0;
   if (strcmp(token_kind, req_kind) != 0)
      return 0;
   if (strcmp(token_id ? token_id : "", req_id ? req_id : "") != 0)
      return 0;
   return 1;
}

/* Extract the value of key=… from a urlencoded-ish query string (no percent
 * decoding needed for our scope ids). Returns 1 if found. */
static int qval(const char *qs, const char *key, char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   if (!qs || !key)
      return 0;
   size_t klen = strlen(key);
   const char *p = qs;
   while (p && *p)
   {
      const char *eq = strchr(p, '=');
      if (!eq)
         break;
      size_t nlen = (size_t)(eq - p);
      const char *val = eq + 1;
      const char *amp = strchr(val, '&');
      size_t vlen = amp ? (size_t)(amp - val) : strlen(val);
      if (nlen == klen && strncmp(p, key, klen) == 0)
      {
         copy_n(out, cap, val, vlen);
         return 1;
      }
      if (!amp)
         break;
      p = amp + 1;
   }
   return 0;
}

/* Extract a JSON string value for "key" (flat object, no escapes in value). */
static int jval(const char *body, const char *key, char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   if (!body || !key)
      return 0;
   char needle[128];
   snprintf(needle, sizeof(needle), "\"%s\"", key);
   const char *p = strstr(body, needle);
   if (!p)
      return 0;
   p = strchr(p + strlen(needle), ':');
   if (!p)
      return 0;
   p++;
   while (*p == ' ' || *p == '\t')
      p++;
   if (*p != '"')
      return 0;
   p++;
   const char *end = strchr(p, '"');
   if (!end)
      return 0;
   copy_n(out, cap, p, (size_t)(end - p));
   return 1;
}

int kb_scope_request_target(const char *query_string, const char *body, char *kind, size_t kind_len,
                            char *id, size_t id_len)
{
   if (kind && kind_len)
      kind[0] = '\0';
   if (id && id_len)
      id[0] = '\0';

   char tmp[256];

   /* 1. query: scope=<kind>:<id> */
   if (qval(query_string, "scope", tmp, sizeof(tmp)) && tmp[0])
   {
      const char *c = strchr(tmp, ':');
      if (c)
      {
         copy_n(kind, kind_len, tmp, (size_t)(c - tmp));
         copy_n(id, id_len, c + 1, strlen(c + 1));
         return (kind[0] != '\0');
      }
   }
   /* 2. query: project=<id> / workspace=<id> */
   if (qval(query_string, "project", tmp, sizeof(tmp)) && tmp[0])
   {
      copy_n(kind, kind_len, "project", 7);
      copy_n(id, id_len, tmp, strlen(tmp));
      return 1;
   }
   if (qval(query_string, "workspace", tmp, sizeof(tmp)) && tmp[0])
   {
      copy_n(kind, kind_len, "workspace", 9);
      copy_n(id, id_len, tmp, strlen(tmp));
      return 1;
   }
   /* 3. body: scope_kind + scope_id */
   char bkind[64] = "", bid[192] = "";
   if (jval(body, "scope_kind", bkind, sizeof(bkind)) && bkind[0])
   {
      jval(body, "scope_id", bid, sizeof(bid));
      copy_n(kind, kind_len, bkind, strlen(bkind));
      copy_n(id, id_len, bid, strlen(bid));
      return 1;
   }
   /* 4. body: scope_user → kind=user */
   if (jval(body, "scope_user", bid, sizeof(bid)) && bid[0])
   {
      copy_n(kind, kind_len, "user", 4);
      copy_n(id, id_len, bid, strlen(bid));
      return 1;
   }
   /* 5. body: project → kind=project. The POST routes (build, update, scan,
    * ingest, maintenance) name their target project in the body, not the query
    * string, so without this a scoped token reached any project simply by
    * omitting a query it never had to send: the target resolved to "no scope
    * named" and kb_scope_authorized allowed it. Checked last so an explicit
    * scope_kind/scope_user still wins. */
   if (jval(body, "project", bid, sizeof(bid)) && bid[0])
   {
      copy_n(kind, kind_len, "project", 7);
      copy_n(id, id_len, bid, strlen(bid));
      return 1;
   }
   /* 6. body: workspace → kind=workspace (POST /v1/ingest), same reasoning.
    * NOTE this resolves a NAMED workspace only. Ingest treats an absent or
    * "all" workspace as every discovered project, and that still names no
    * scope here, so it is not denied by this layer — see the route. */
   if (jval(body, "workspace", bid, sizeof(bid)) && bid[0] && strcmp(bid, "all") != 0)
   {
      copy_n(kind, kind_len, "workspace", 9);
      copy_n(id, id_len, bid, strlen(bid));
      return 1;
   }
   return 0;
}
