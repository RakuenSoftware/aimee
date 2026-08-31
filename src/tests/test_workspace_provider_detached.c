/* test_workspace_provider_detached.c: the detached provider must marshal each
 * op to a JSON request and parse the runner's JSON response back into the
 * provider contract — binary-safe (base64), via a pluggable transport. A mock
 * transport stands in for the client-side runner. */
#include "modules/workspace/workspace_provider_detached.h"
#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* base64 of known payloads (precomputed so the test needs no codec):
 *   "a\0b" (3 bytes) -> "YQBi"      "x\0y" (3 bytes) -> "eAB5"
 *   "done"           -> "ZG9uZQ==" */
static char g_captured_write[64];

static char *direct_read(const char *path, size_t *len);

/* serve_once mocks: fetch yields one write_all op (base64 "aGk=" == "hi") then
 * nothing; post records whether the runner reported ok. */
static int g_fetch_count;
static char g_serve_path[320];
static int g_post_ok;
static cJSON *serve_fetch(void *ctx)
{
   (void)ctx;
   if (g_fetch_count++ > 0)
      return NULL;
   cJSON *op = cJSON_CreateObject();
   cJSON_AddStringToObject(op, "op", "write_all");
   cJSON_AddStringToObject(op, "path", g_serve_path);
   cJSON_AddStringToObject(op, "data", "aGk=");
   return op;
}
static int serve_post(void *ctx, cJSON *resp)
{
   (void)ctx;
   g_post_ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "ok"));
   cJSON_Delete(resp);
   return 0;
}

static int mock_transport(void *ctx, cJSON *request, cJSON **response)
{
   (void)ctx;
   *response = NULL;
   const cJSON *op = cJSON_GetObjectItemCaseSensitive(request, "op");
   const char *name = (cJSON_IsString(op) && op->valuestring) ? op->valuestring : "";
   cJSON *resp = cJSON_CreateObject();

   if (strcmp(name, "read_all") == 0)
   {
      cJSON_AddBoolToObject(resp, "ok", 1);
      cJSON_AddStringToObject(resp, "data", "YQBi");
   }
   else if (strcmp(name, "write_all") == 0)
   {
      const cJSON *d = cJSON_GetObjectItemCaseSensitive(request, "data");
      snprintf(g_captured_write, sizeof(g_captured_write), "%s",
               (cJSON_IsString(d) && d->valuestring) ? d->valuestring : "");
      cJSON_AddBoolToObject(resp, "ok", 1);
   }
   else if (strcmp(name, "stat") == 0)
   {
      cJSON_AddBoolToObject(resp, "exists", 1);
      cJSON_AddBoolToObject(resp, "is_dir", 0);
      cJSON_AddNumberToObject(resp, "size", 42);
   }
   else if (strcmp(name, "list") == 0)
   {
      cJSON_AddBoolToObject(resp, "ok", 1);
      cJSON *arr = cJSON_AddArrayToObject(resp, "entries");
      cJSON_AddItemToArray(arr, cJSON_CreateString("/w/a.c"));
      cJSON_AddItemToArray(arr, cJSON_CreateString("/w/b.c"));
   }
   else if (strcmp(name, "exec") == 0)
   {
      cJSON_AddNumberToObject(resp, "rc", 0);
      cJSON_AddStringToObject(resp, "output", "ZG9uZQ==");
   }
   else
   {
      cJSON_Delete(resp);
      cJSON_Delete(request);
      return -1;
   }
   cJSON_Delete(request);
   *response = resp;
   return 0;
}

/* Always-failing transport: must free the request and report failure. */
static int failing_transport(void *ctx, cJSON *request, cJSON **response)
{
   (void)ctx;
   *response = NULL;
   cJSON_Delete(request);
   return -1;
}

/* In-process transport that runs the request through the real runner (which
 * executes against the local filesystem via the shared provider). Wiring the
 * detached provider to this closes the full marshal -> runner -> fs loop. */
static int runner_transport(void *ctx, cJSON *request, cJSON **response)
{
   (void)ctx;
   *response = ws_detached_runner_handle(request);
   cJSON_Delete(request);
   return *response ? 0 : -1;
}

int main(void)
{
   ws_detached_provider_t dp;
   ws_detached_provider_init(&dp, mock_transport, NULL);
   const workspace_provider_t *ws = &dp.base;
   assert(ws->kind == WS_PROVIDER_DETACHED);

   /* read_all: base64 "YQBi" -> bytes {'a', 0, 'b'} (binary-safe) */
   {
      char *out = NULL;
      size_t len = 0;
      assert(ws->read_all(ws, "/x", &out, &len) == 0);
      assert(len == 3);
      assert(out[0] == 'a' && out[1] == '\0' && out[2] == 'b');
      assert(out[3] == '\0'); /* NUL one past len */
      free(out);
   }

   /* write_all: bytes {'x',0,'y'} marshal to base64 "eAB5" */
   {
      const char data[3] = {'x', '\0', 'y'};
      assert(ws->write_all(ws, "/x", data, 3) == 0);
      assert(strcmp(g_captured_write, "eAB5") == 0);
   }

   /* stat */
   {
      ws_stat_t st;
      assert(ws->stat(ws, "/x", &st) == 0);
      assert(st.exists == 1 && st.is_dir == 0 && st.size == 42);
   }

   /* list */
   {
      char **entries = NULL;
      int n = 0;
      assert(ws->list(ws, "/w", "*.c", &entries, &n) == 0);
      assert(n == 2);
      assert(strcmp(entries[0], "/w/a.c") == 0);
      assert(strcmp(entries[1], "/w/b.c") == 0);
      ws_provider_free_list(entries, n);
   }

   /* exec: output base64 "ZG9uZQ==" -> "done" */
   {
      const char *argv[] = {"echo", "hi", NULL};
      char *out = NULL;
      assert(ws->exec(ws, argv, &out, 4096) == 0);
      assert(out && strcmp(out, "done") == 0);
      free(out);
   }

   /* transport failure -> ops report -1, out cleared */
   {
      ws_detached_provider_t fp;
      ws_detached_provider_init(&fp, failing_transport, NULL);
      char *out = (char *)0x1;
      size_t len = 1;
      assert(fp.base.read_all(&fp.base, "/x", &out, &len) == -1);
      assert(out == NULL && len == 0);
      assert(fp.base.write_all(&fp.base, "/x", "z", 1) == -1);
   }

   /* NULL transport -> ops fail cleanly (no crash, no leak of the request) */
   {
      ws_detached_provider_t np;
      ws_detached_provider_init(&np, NULL, NULL);
      char *out = NULL;
      size_t len = 0;
      assert(np.base.read_all(&np.base, "/x", &out, &len) == -1);
      assert(out == NULL);
   }

   /* --- full round-trip: detached provider <-> runner <-> real filesystem --- */
   {
      char dir[256];
      snprintf(dir, sizeof(dir), "%s/ws_detached_rt.XXXXXX", platform_tmpdir());
      assert(mkdtemp(dir) != NULL);
      char fpath[320];
      snprintf(fpath, sizeof(fpath), "%s/file.bin", dir);

      ws_detached_provider_t rt;
      ws_detached_provider_init(&rt, runner_transport, NULL);
      const workspace_provider_t *r = &rt.base;

      /* write binary content (with an embedded NUL) through detached -> runner */
      const char payload[7] = {'h', 'i', '\0', 'b', 'y', 't', 'e'};
      assert(r->write_all(r, fpath, payload, 7) == 0);

      /* the runner wrote a real file: confirm with plain libc */
      size_t dlen = 0;
      char *direct = direct_read(fpath, &dlen);
      assert(direct && dlen == 7 && memcmp(direct, payload, 7) == 0);
      free(direct);

      /* read it back through detached -> runner, byte-for-byte */
      char *got = NULL;
      size_t glen = 0;
      assert(r->read_all(r, fpath, &got, &glen) == 0);
      assert(glen == 7 && memcmp(got, payload, 7) == 0);
      free(got);

      /* stat the real file */
      ws_stat_t st;
      assert(r->stat(r, fpath, &st) == 0);
      assert(st.exists == 1 && st.is_dir == 0 && st.size == 7);

      /* list the real dir */
      char **entries = NULL;
      int n = 0;
      assert(r->list(r, dir, "*.bin", &entries, &n) == 0);
      assert(n == 1 && strstr(entries[0], "file.bin") != NULL);
      ws_provider_free_list(entries, n);

      /* exec a real command */
      const char *echo_argv[] = {"echo", "rt-ok", NULL};
      char *eout = NULL;
      assert(r->exec(r, echo_argv, &eout, 4096) == 0);
      assert(eout && strstr(eout, "rt-ok") != NULL);
      free(eout);

      /* exec_shell a real command-line: shell features (2>&1) round-trip
       * through the detached marshalling and back */
      int src = -1;
      char *sout = r->exec_shell(r, "echo det_shell_ok 2>&1", &src);
      assert(src == 0);
      assert(sout && strstr(sout, "det_shell_ok") != NULL);
      free(sout);

      char *sbad = r->exec_shell(r, "exit 5", &src);
      assert(src != 0);
      free(sbad);

      unlink(fpath);
      rmdir(dir);
   }

   /* --- ws_runner_serve_once: fetch op -> runner executes -> post result --- */
   {
      char sdir[256];
      snprintf(sdir, sizeof(sdir), "%s/ws_serve_once.XXXXXX", platform_tmpdir());
      assert(mkdtemp(sdir) != NULL);
      snprintf(g_serve_path, sizeof(g_serve_path), "%s/served.txt", sdir);
      g_fetch_count = 0;
      g_post_ok = 0;

      assert(ws_runner_serve_once(serve_fetch, serve_post, NULL) == 1); /* served an op */
      assert(g_post_ok == 1);

      size_t len = 0;
      char *d = direct_read(g_serve_path, &len);
      assert(d && len == 2 && memcmp(d, "hi", 2) == 0); /* runner wrote it */
      free(d);

      assert(ws_runner_serve_once(serve_fetch, serve_post, NULL) == 0); /* nothing pending */

      unlink(g_serve_path);
      rmdir(sdir);
   }

   printf("workspace_provider_detached: all tests passed\n");
   return 0;
}

/* Read a file with plain libc — the ground truth for the round-trip test. */
static char *direct_read(const char *path, size_t *len)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   char *buf = malloc((size_t)sz + 1);
   size_t rd = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   buf[rd] = '\0';
   if (len)
      *len = rd;
   return buf;
}
