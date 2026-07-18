/* workspace_provider_detached.c — the `detached` resource provider.
 *
 * Each provider op is marshalled to a JSON request and handed to a pluggable
 * transport (the client-side runner / a test mock); the JSON response is parsed
 * back into the provider's contract. Binary payloads (file contents, exec
 * output) cross as base64 so embedded NULs survive. See
 * workspace_provider_detached.h. The transport and the request/response schema
 * are the seam the /v1 reverse channel plugs into; nothing here touches the
 * filesystem, so it is transport-agnostic and unit-tested with a mock. */
#include "workspace_provider_detached.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

/* run_cmd cwd control (util.c): exec_shell honors the thread-local working
 * directory the mcp_git tools set before shelling out. The runner end applies
 * the marshalled cwd around its local exec_shell call. Forward-declared to keep
 * this TU off the heavyweight aimee.h include. */
extern const char *run_cmd_get_cwd(void);
extern void run_cmd_set_cwd(const char *cwd);

/* --- RFC 4648 base64 (self-contained so the unit test links only this TU +
 * cJSON; the delegate backends keep their own copies). --- */

static const char B64_ALPHA[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Encode in_len bytes; returns a malloc'd NUL-terminated string (caller frees)
 * or NULL on OOM. */
static char *b64_encode(const char *in, size_t in_len)
{
   size_t out_cap = ((in_len + 2) / 3) * 4 + 1;
   char *out = malloc(out_cap);
   if (!out)
      return NULL;
   size_t o = 0;
   for (size_t i = 0; i < in_len; i += 3)
   {
      unsigned v = (unsigned char)in[i] << 16;
      if (i + 1 < in_len)
         v |= (unsigned char)in[i + 1] << 8;
      if (i + 2 < in_len)
         v |= (unsigned char)in[i + 2];
      out[o++] = B64_ALPHA[(v >> 18) & 0x3f];
      out[o++] = B64_ALPHA[(v >> 12) & 0x3f];
      out[o++] = (i + 1 < in_len) ? B64_ALPHA[(v >> 6) & 0x3f] : '=';
      out[o++] = (i + 2 < in_len) ? B64_ALPHA[v & 0x3f] : '=';
   }
   out[o] = '\0';
   return out;
}

static int b64_val(char c)
{
   if (c >= 'A' && c <= 'Z')
      return c - 'A';
   if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
   if (c >= '0' && c <= '9')
      return c - '0' + 52;
   if (c == '+')
      return 62;
   if (c == '/')
      return 63;
   return -1;
}

/* Decode a NUL-terminated base64 string into a malloc'd buffer with a trailing
 * NUL one past *out_len (binary-safe). Returns 0 on success, -1 on malformed
 * input / OOM. */
static int b64_decode(const char *in, char **out, size_t *out_len)
{
   if (!in || !out)
      return -1;
   size_t in_len = strlen(in);
   if (in_len % 4 != 0)
      return -1;
   size_t pad = 0;
   if (in_len >= 1 && in[in_len - 1] == '=')
      pad++;
   if (in_len >= 2 && in[in_len - 2] == '=')
      pad++;
   size_t dec_len = in_len / 4 * 3 - pad;
   char *buf = malloc(dec_len + 1);
   if (!buf)
      return -1;
   size_t o = 0;
   for (size_t i = 0; i < in_len; i += 4)
   {
      int a = b64_val(in[i]), b = b64_val(in[i + 1]);
      int c = (in[i + 2] == '=') ? 0 : b64_val(in[i + 2]);
      int d = (in[i + 3] == '=') ? 0 : b64_val(in[i + 3]);
      if (a < 0 || b < 0 || c < 0 || d < 0)
      {
         free(buf);
         return -1;
      }
      unsigned v = (unsigned)a << 18 | (unsigned)b << 12 | (unsigned)c << 6 | (unsigned)d;
      if (o < dec_len)
         buf[o++] = (char)((v >> 16) & 0xff);
      if (o < dec_len)
         buf[o++] = (char)((v >> 8) & 0xff);
      if (o < dec_len)
         buf[o++] = (char)(v & 0xff);
   }
   buf[dec_len] = '\0';
   *out = buf;
   if (out_len)
      *out_len = dec_len;
   return 0;
}

/* --- request dispatch --- */

/* Send a freshly-built request through the transport; returns the parsed
 * response (caller cJSON_Delete's) or NULL on transport failure. Consumes
 * `req` (the transport owns it). */
static cJSON *detached_dispatch(const workspace_provider_t *p, cJSON *req)
{
   const ws_detached_provider_t *self = (const ws_detached_provider_t *)p;
   if (!req)
      return NULL;
   if (!self->transport)
   {
      cJSON_Delete(req);
      return NULL;
   }
   cJSON *resp = NULL;
   if (self->transport(self->transport_ctx, req, &resp) != 0)
      return NULL; /* transport owns/freed req per contract */
   return resp;
}

static cJSON *op_request(const char *op)
{
   cJSON *req = cJSON_CreateObject();
   if (req)
      cJSON_AddStringToObject(req, "op", op);
   return req;
}

static int resp_ok(const cJSON *resp)
{
   return resp && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "ok"));
}

/* --- provider ops --- */

static int detached_read_all(const workspace_provider_t *p, const char *path, char **out,
                             size_t *len)
{
   if (out)
      *out = NULL;
   if (len)
      *len = 0;
   if (!path || !out)
      return -1;

   cJSON *req = op_request("read_all");
   if (!req)
      return -1;
   cJSON_AddStringToObject(req, "path", path);

   cJSON *resp = detached_dispatch(p, req);
   int rc = -1;
   if (resp_ok(resp))
   {
      const cJSON *data = cJSON_GetObjectItemCaseSensitive(resp, "data");
      if (cJSON_IsString(data) && data->valuestring)
         rc = b64_decode(data->valuestring, out, len);
   }
   cJSON_Delete(resp);
   return rc;
}

static int detached_write_all(const workspace_provider_t *p, const char *path, const char *data,
                              size_t len)
{
   if (!path)
      return -1;

   cJSON *req = op_request("write_all");
   if (!req)
      return -1;
   cJSON_AddStringToObject(req, "path", path);
   char *enc = b64_encode(data ? data : "", len);
   if (!enc)
   {
      cJSON_Delete(req);
      return -1;
   }
   cJSON_AddStringToObject(req, "data", enc);
   free(enc);

   cJSON *resp = detached_dispatch(p, req);
   int rc = resp_ok(resp) ? 0 : -1;
   cJSON_Delete(resp);
   return rc;
}

static int detached_stat(const workspace_provider_t *p, const char *path, ws_stat_t *st)
{
   if (!st)
      return -1;
   st->exists = 0;
   st->is_dir = 0;
   st->size = 0;

   cJSON *req = op_request("stat");
   if (!req)
      return -1;
   cJSON_AddStringToObject(req, "path", path ? path : "");

   cJSON *resp = detached_dispatch(p, req);
   if (resp)
   {
      st->exists = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "exists")) ? 1 : 0;
      st->is_dir = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "is_dir")) ? 1 : 0;
      const cJSON *sz = cJSON_GetObjectItemCaseSensitive(resp, "size");
      if (cJSON_IsNumber(sz))
         st->size = (long)sz->valuedouble;
   }
   cJSON_Delete(resp);
   return 0; /* stat always reports; absence is exists=0 (mirrors shared) */
}

static int detached_list(const workspace_provider_t *p, const char *dir, const char *pattern,
                         char ***out, int *count)
{
   if (out)
      *out = NULL;
   if (count)
      *count = 0;
   if (!dir || !out || !count)
      return -1;

   cJSON *req = op_request("list");
   if (!req)
      return -1;
   cJSON_AddStringToObject(req, "dir", dir);
   if (pattern)
      cJSON_AddStringToObject(req, "pattern", pattern);

   cJSON *resp = detached_dispatch(p, req);
   int rc = -1;
   if (resp_ok(resp))
   {
      const cJSON *entries = cJSON_GetObjectItemCaseSensitive(resp, "entries");
      if (cJSON_IsArray(entries))
      {
         int n = cJSON_GetArraySize(entries);
         char **arr = (n > 0) ? calloc((size_t)n, sizeof(char *)) : NULL;
         if (n == 0 || arr)
         {
            int ok = 1;
            for (int i = 0; i < n && ok; i++)
            {
               const cJSON *e = cJSON_GetArrayItem(entries, i);
               const char *s = cJSON_IsString(e) ? e->valuestring : NULL;
               arr[i] = s ? strdup(s) : strdup("");
               if (!arr[i])
                  ok = 0;
            }
            if (ok)
            {
               *out = arr;
               *count = n;
               rc = 0;
            }
            else
               ws_provider_free_list(arr, n);
         }
      }
   }
   cJSON_Delete(resp);
   return rc;
}

static int detached_exec(const workspace_provider_t *p, const char *const argv[], char **out,
                         size_t max_out)
{
   if (out)
      *out = NULL;
   if (!argv || !argv[0])
      return -1;

   cJSON *req = op_request("exec");
   if (!req)
      return -1;
   cJSON *jargv = cJSON_AddArrayToObject(req, "argv");
   for (int i = 0; argv[i]; i++)
      cJSON_AddItemToArray(jargv, cJSON_CreateString(argv[i]));
   cJSON_AddNumberToObject(req, "max_out", (double)max_out);

   cJSON *resp = detached_dispatch(p, req);
   int rc = -1;
   if (resp)
   {
      const cJSON *jrc = cJSON_GetObjectItemCaseSensitive(resp, "rc");
      rc = cJSON_IsNumber(jrc) ? (int)jrc->valuedouble : -1;
      const cJSON *output = cJSON_GetObjectItemCaseSensitive(resp, "output");
      if (out && cJSON_IsString(output) && output->valuestring)
      {
         size_t olen = 0;
         char *decoded = NULL;
         if (b64_decode(output->valuestring, &decoded, &olen) == 0)
         {
            if (max_out > 0 && olen > max_out)
            {
               decoded[max_out] = '\0';
            }
            *out = decoded;
         }
      }
   }
   cJSON_Delete(resp);
   return rc;
}

/* Marshal a shell command-line (mirrors detached_exec, but carries a `cmd`
 * string + the optional thread-local cwd rather than an argv array). Parses the
 * same {rc, output(base64)} response shape. Returns the decoded combined output
 * (caller frees; may be NULL) and sets *exit_code to the command's status. */
static char *detached_exec_shell(const workspace_provider_t *p, const char *cmd, int *exit_code)
{
   if (exit_code)
      *exit_code = -1;
   if (!cmd)
      return NULL;

   cJSON *req = op_request("exec_shell");
   if (!req)
      return NULL;
   cJSON_AddStringToObject(req, "cmd", cmd);
   const char *cwd = run_cmd_get_cwd();
   if (cwd && cwd[0])
      cJSON_AddStringToObject(req, "cwd", cwd);

   cJSON *resp = detached_dispatch(p, req);
   char *out = NULL;
   if (resp)
   {
      const cJSON *jrc = cJSON_GetObjectItemCaseSensitive(resp, "rc");
      if (exit_code)
         *exit_code = cJSON_IsNumber(jrc) ? (int)jrc->valuedouble : -1;
      const cJSON *joutput = cJSON_GetObjectItemCaseSensitive(resp, "output");
      if (cJSON_IsString(joutput) && joutput->valuestring)
      {
         size_t olen = 0;
         char *decoded = NULL;
         if (b64_decode(joutput->valuestring, &decoded, &olen) == 0)
            out = decoded;
      }
   }
   cJSON_Delete(resp);
   return out;
}

/* Bridge a streaming transport's partial responses (cJSON {"chunk":<b64>}) to
 * the provider-level (data,len) chunk callback the caller passed to
 * exec_stream. */
typedef struct
{
   ws_exec_chunk_fn on_chunk;
   void *cb_ctx;
} detached_stream_bridge_t;

static int detached_partial_bridge(void *ctx, cJSON *partial)
{
   detached_stream_bridge_t *b = (detached_stream_bridge_t *)ctx;
   if (!b || !b->on_chunk)
      return 0;
   const cJSON *c = cJSON_GetObjectItemCaseSensitive(partial, "chunk");
   if (!cJSON_IsString(c) || !c->valuestring)
      return 0;
   char *dec = NULL;
   size_t dl = 0;
   if (b64_decode(c->valuestring, &dec, &dl) != 0)
      return 0;
   int rc = b->on_chunk(b->cb_ctx, dec, dl);
   free(dec);
   return rc;
}

static int detached_exec_stream(const workspace_provider_t *p, const char *const argv[],
                                const char *stdin_data, size_t stdin_len, const char *cwd,
                                ws_exec_chunk_fn on_chunk, void *cb_ctx)
{
   const ws_detached_provider_t *self = (const ws_detached_provider_t *)p;
   if (!self->stream_transport || !argv || !argv[0])
      return -1;

   cJSON *req = op_request("exec_stream");
   if (!req)
      return -1;
   cJSON *jargv = cJSON_AddArrayToObject(req, "argv");
   for (int i = 0; argv[i]; i++)
      cJSON_AddItemToArray(jargv, cJSON_CreateString(argv[i]));
   if (stdin_data && stdin_len > 0)
   {
      char *enc = b64_encode(stdin_data, stdin_len);
      if (enc)
      {
         cJSON_AddStringToObject(req, "stdin", enc);
         free(enc);
      }
   }
   if (cwd && cwd[0])
      cJSON_AddStringToObject(req, "cwd", cwd);

   detached_stream_bridge_t bridge = {on_chunk, cb_ctx};
   cJSON *final = NULL;
   int trc =
       self->stream_transport(self->transport_ctx, req, detached_partial_bridge, &bridge, &final);
   int rc = -1;
   if (trc == 0 && final)
   {
      const cJSON *jrc = cJSON_GetObjectItemCaseSensitive(final, "rc");
      rc = cJSON_IsNumber(jrc) ? (int)jrc->valuedouble : -1;
   }
   cJSON_Delete(final);
   return rc;
}

void ws_detached_provider_init_ex(ws_detached_provider_t *out, ws_detached_transport_fn transport,
                                  ws_detached_stream_transport_fn stream_transport, void *ctx)
{
   if (!out)
      return;
   out->base.kind = WS_PROVIDER_DETACHED;
   out->base.read_all = detached_read_all;
   out->base.write_all = detached_write_all;
   out->base.stat = detached_stat;
   out->base.list = detached_list;
   out->base.exec = detached_exec;
   out->base.exec_shell = detached_exec_shell;
   /* Only expose exec_stream when a streaming transport is bound, so callers can
    * test `ws->exec_stream != NULL` to decide whether to route a CLI agent to
    * the client (a mock/unary-only provider leaves it off → local fork/exec). */
   out->base.exec_stream = stream_transport ? detached_exec_stream : NULL;
   out->transport = transport;
   out->stream_transport = stream_transport;
   out->transport_ctx = ctx;
}

void ws_detached_provider_init(ws_detached_provider_t *out, ws_detached_transport_fn transport,
                               void *ctx)
{
   ws_detached_provider_init_ex(out, transport, NULL, ctx);
}

/* --- runner end: execute an op locally via the shared provider --- */

static const char *req_str(const cJSON *req, const char *key)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(req, key);
   return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;
}

int ws_runner_serve_once(cJSON *(*fetch)(void *), int (*post)(void *, cJSON *), void *ctx)
{
   if (!fetch || !post)
      return -1;
   cJSON *op = fetch(ctx);
   if (!op)
      return 0; /* nothing pending — caller re-polls */

   const char *opname = req_str(op, "op");
   cJSON *resp;
   if (opname && strcmp(opname, "exec_stream") == 0)
   {
      /* Streaming op: handle_stream posts each stdout chunk through `post` as a
       * partial; we post the terminal response below. */
      resp = ws_detached_runner_handle_stream(op, post, ctx);
   }
   else
   {
      resp = ws_detached_runner_handle(op);
   }
   cJSON_Delete(op);
   if (!resp)
   {
      /* unknown/missing op — answer with a failed result rather than stalling */
      resp = cJSON_CreateObject();
      if (resp)
         cJSON_AddBoolToObject(resp, "ok", 0);
   }
   return post(ctx, resp) == 0 ? 1 : -1;
}

/* Bridge the shared provider's (data,len) stdout chunks to {"partial":true,
 * "chunk":<b64>} responses posted through the runner's `emit`. */
typedef struct
{
   int (*emit)(void *ctx, cJSON *partial);
   void *ctx;
} runner_emit_bridge_t;

static int runner_emit_chunk(void *cb_ctx, const char *data, size_t len)
{
   runner_emit_bridge_t *e = (runner_emit_bridge_t *)cb_ctx;
   char *enc = b64_encode(data, len);
   if (!enc)
      return -1;
   cJSON *partial = cJSON_CreateObject();
   if (!partial)
   {
      free(enc);
      return -1;
   }
   cJSON_AddBoolToObject(partial, "ok", 1);
   cJSON_AddBoolToObject(partial, "partial", 1);
   cJSON_AddStringToObject(partial, "chunk", enc);
   free(enc);
   return e->emit(e->ctx, partial); /* emit takes ownership */
}

cJSON *ws_detached_runner_handle_stream(const cJSON *request,
                                        int (*emit)(void *ctx, cJSON *partial), void *emit_ctx)
{
   const char *op = req_str(request, "op");
   if (!op || strcmp(op, "exec_stream") != 0)
      return NULL;

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;

   const workspace_provider_t *ws = workspace_provider_shared();
   const cJSON *jargv = cJSON_GetObjectItemCaseSensitive(request, "argv");
   int n = cJSON_IsArray(jargv) ? cJSON_GetArraySize(jargv) : 0;
   const char **argv = (n > 0) ? calloc((size_t)n + 1, sizeof(char *)) : NULL;
   if (n <= 0 || !argv || !ws->exec_stream)
   {
      cJSON_AddBoolToObject(resp, "ok", 0);
      cJSON_AddNumberToObject(resp, "rc", -1);
      free(argv);
      return resp;
   }
   for (int i = 0; i < n; i++)
   {
      const cJSON *a = cJSON_GetArrayItem(jargv, i);
      argv[i] = (cJSON_IsString(a) && a->valuestring) ? a->valuestring : "";
   }

   char *stdin_buf = NULL;
   size_t stdin_len = 0;
   const char *b64stdin = req_str(request, "stdin");
   if (b64stdin)
      (void)b64_decode(b64stdin, &stdin_buf, &stdin_len);
   const char *cwd = req_str(request, "cwd");

   runner_emit_bridge_t eb = {emit, emit_ctx};
   int rc = ws->exec_stream(ws, argv, stdin_buf, stdin_len, cwd, runner_emit_chunk, &eb);

   free(stdin_buf);
   free(argv);
   cJSON_AddBoolToObject(resp, "ok", 1);
   cJSON_AddNumberToObject(resp, "rc", rc);
   return resp;
}

cJSON *ws_detached_runner_handle(const cJSON *request)
{
   const char *op = req_str(request, "op");
   if (!op)
      return NULL;

   const workspace_provider_t *ws = workspace_provider_shared();
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;

   if (strcmp(op, "read_all") == 0)
   {
      const char *path = req_str(request, "path");
      char *data = NULL;
      size_t len = 0;
      if (path && ws->read_all(ws, path, &data, &len) == 0)
      {
         char *enc = b64_encode(data, len);
         if (enc)
         {
            cJSON_AddBoolToObject(resp, "ok", 1);
            cJSON_AddStringToObject(resp, "data", enc);
            free(enc);
         }
         else
            cJSON_AddBoolToObject(resp, "ok", 0);
      }
      else
         cJSON_AddBoolToObject(resp, "ok", 0);
      free(data);
   }
   else if (strcmp(op, "write_all") == 0)
   {
      const char *path = req_str(request, "path");
      const char *b64 = req_str(request, "data");
      char *raw = NULL;
      size_t rlen = 0;
      if (path && b64 && b64_decode(b64, &raw, &rlen) == 0)
         cJSON_AddBoolToObject(resp, "ok", ws->write_all(ws, path, raw, rlen) == 0 ? 1 : 0);
      else
         cJSON_AddBoolToObject(resp, "ok", 0);
      free(raw);
   }
   else if (strcmp(op, "stat") == 0)
   {
      ws_stat_t st;
      ws->stat(ws, req_str(request, "path"), &st);
      cJSON_AddBoolToObject(resp, "exists", st.exists);
      cJSON_AddBoolToObject(resp, "is_dir", st.is_dir);
      cJSON_AddNumberToObject(resp, "size", (double)st.size);
   }
   else if (strcmp(op, "list") == 0)
   {
      const char *dir = req_str(request, "dir");
      const char *pattern = req_str(request, "pattern");
      char **entries = NULL;
      int n = 0;
      if (dir && ws->list(ws, dir, pattern, &entries, &n) == 0)
      {
         cJSON_AddBoolToObject(resp, "ok", 1);
         cJSON *arr = cJSON_AddArrayToObject(resp, "entries");
         for (int i = 0; i < n; i++)
            cJSON_AddItemToArray(arr, cJSON_CreateString(entries[i]));
         ws_provider_free_list(entries, n);
      }
      else
         cJSON_AddBoolToObject(resp, "ok", 0);
   }
   else if (strcmp(op, "exec") == 0)
   {
      const cJSON *jargv = cJSON_GetObjectItemCaseSensitive(request, "argv");
      int n = cJSON_IsArray(jargv) ? cJSON_GetArraySize(jargv) : 0;
      const cJSON *jmax = cJSON_GetObjectItemCaseSensitive(request, "max_out");
      size_t max_out = cJSON_IsNumber(jmax) ? (size_t)jmax->valuedouble : 0;

      const char **argv = (n > 0) ? calloc((size_t)n + 1, sizeof(char *)) : NULL;
      if (n <= 0 || !argv)
      {
         cJSON_AddNumberToObject(resp, "rc", -1);
      }
      else
      {
         for (int i = 0; i < n; i++)
         {
            const cJSON *a = cJSON_GetArrayItem(jargv, i);
            argv[i] = (cJSON_IsString(a) && a->valuestring) ? a->valuestring : "";
         }
         char *out = NULL;
         int rc = ws->exec(ws, argv, &out, max_out);
         cJSON_AddNumberToObject(resp, "rc", rc);
         if (out)
         {
            char *enc = b64_encode(out, strlen(out));
            if (enc)
            {
               cJSON_AddStringToObject(resp, "output", enc);
               free(enc);
            }
            free(out);
         }
         free(argv);
      }
   }
   else if (strcmp(op, "exec_shell") == 0)
   {
      const char *cmd = req_str(request, "cmd");
      const char *cwd = req_str(request, "cwd");
      if (!cmd)
      {
         cJSON_AddNumberToObject(resp, "rc", -1);
      }
      else
      {
         /* Save + restore the thread-local run_cmd cwd so the runner honors the
          * marshalled cwd without leaking it into later ops on this thread. */
         const char *prev_cwd = run_cmd_get_cwd();
         char *saved = prev_cwd ? strdup(prev_cwd) : NULL;
         run_cmd_set_cwd(cwd);
         int rc = -1;
         char *out = ws->exec_shell(ws, cmd, &rc);
         run_cmd_set_cwd(saved);
         free(saved);
         cJSON_AddNumberToObject(resp, "rc", rc);
         if (out)
         {
            char *enc = b64_encode(out, strlen(out));
            if (enc)
            {
               cJSON_AddStringToObject(resp, "output", enc);
               free(enc);
            }
            free(out);
         }
      }
   }
   else
   {
      cJSON_Delete(resp);
      return NULL; /* unknown op */
   }

   return resp;
}
