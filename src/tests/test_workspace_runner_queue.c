/* test_workspace_runner_queue.c: the runner queue must carry op requests from a
 * detached provider (one thread) to a runner (another thread) and the responses
 * back — closing the detached loop across threads. The runner thread drains the
 * queue and executes ops via the real runner against a real tmp dir, so this
 * exercises detached provider -> queue -> runner -> filesystem end to end. */
#include "workspace_runner_queue.h"
#include "workspace_provider_detached.h"
#include "workspace_provider.h"
#include "cJSON.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static ws_runner_queue_t g_q;

static int q_post(void *ctx, cJSON *resp)
{
   return ws_runner_queue_respond((ws_runner_queue_t *)ctx, resp);
}

/* Runner: drain the queue, execute each op against the local fs, respond.
 * Streaming ops (exec_stream) post their stdout chunks as partials through
 * q_post, then the terminal response below. Exits when poll returns NULL. */
static void *runner_thread(void *arg)
{
   (void)arg;
   for (;;)
   {
      cJSON *req = ws_runner_queue_poll(&g_q, 5000);
      if (!req)
         break;
      const cJSON *opj = cJSON_GetObjectItemCaseSensitive(req, "op");
      const char *op = (opj && cJSON_IsString(opj)) ? opj->valuestring : "";
      cJSON *resp = (strcmp(op, "exec_stream") == 0)
                        ? ws_detached_runner_handle_stream(req, q_post, &g_q)
                        : ws_detached_runner_handle(req);
      cJSON_Delete(req);
      ws_runner_queue_respond(&g_q, resp);
   }
   return NULL;
}

/* Streaming chunk collector: appends each delivered chunk to a growing buffer. */
typedef struct
{
   char *buf;
   size_t len;
   int calls;
} chunk_collector_t;

static int collect_chunk(void *cb_ctx, const char *data, size_t len)
{
   chunk_collector_t *c = (chunk_collector_t *)cb_ctx;
   char *grown = realloc(c->buf, c->len + len + 1);
   if (!grown)
      return -1;
   c->buf = grown;
   memcpy(c->buf + c->len, data, len);
   c->len += len;
   c->buf[c->len] = '\0';
   c->calls++;
   return 0;
}

int main(void)
{
   char dir[256];
   snprintf(dir, sizeof(dir), "/tmp/ws_runner_q.XXXXXX");
   assert(mkdtemp(dir) != NULL);
   char fpath[320];
   snprintf(fpath, sizeof(fpath), "%s/q.bin", dir);

   /* No serving runner: a detached workspace whose client never shows up must
    * fail the transport fast (pickup timeout) rather than deadlock the caller.
    * This is the delegate-wedge regression: a backgrounded delegate bound to a
    * detached workspace with no client used to block forever here. */
   {
      ws_runner_queue_t nq;
      ws_runner_queue_init(&nq);
      nq.pickup_timeout_ms = 100; /* short deadline so the test is quick */
      ws_detached_provider_t ndp;
      ws_detached_provider_init_ex(&ndp, ws_runner_queue_transport,
                                   ws_runner_queue_transport_stream, &nq);
      const workspace_provider_t *nws = &ndp.base;
      /* Nothing polls nq, so no runner claims the op -> fail fast, no hang. */
      char *nout = NULL;
      size_t nlen = 0;
      assert(nws->read_all(nws, "/nonexistent", &nout, &nlen) == -1);
      assert(nout == NULL);
      /* The streaming transport must fail fast with no runner too. */
      const char *sargv[] = {"echo", "x", NULL};
      chunk_collector_t cc = {0};
      assert(nws->exec_stream(nws, sargv, NULL, 0, "/tmp", collect_chunk, &cc) != 0);
      free(cc.buf);
      ws_runner_queue_destroy(&nq);
   }

   ws_runner_queue_init(&g_q);
   pthread_t th;
   assert(pthread_create(&th, NULL, runner_thread, NULL) == 0);

   /* The detached provider's transport is the queue (both unary + streaming). */
   ws_detached_provider_t dp;
   ws_detached_provider_init_ex(&dp, ws_runner_queue_transport, ws_runner_queue_transport_stream,
                                &g_q);
   const workspace_provider_t *ws = &dp.base;

   /* write -> read across threads, binary-safe (embedded NUL) */
   const char payload[5] = {'q', '\0', 'u', 'e', 'p'};
   assert(ws->write_all(ws, fpath, payload, 5) == 0);

   char *got = NULL;
   size_t glen = 0;
   assert(ws->read_all(ws, fpath, &got, &glen) == 0);
   assert(glen == 5 && memcmp(got, payload, 5) == 0);
   free(got);

   /* stat + list + exec, all routed through the queue to the runner thread */
   ws_stat_t st;
   assert(ws->stat(ws, fpath, &st) == 0);
   assert(st.exists == 1 && st.size == 5);

   char **entries = NULL;
   int n = 0;
   assert(ws->list(ws, dir, "*.bin", &entries, &n) == 0);
   assert(n == 1 && strstr(entries[0], "q.bin") != NULL);
   ws_provider_free_list(entries, n);

   const char *argv[] = {"echo", "queue-ok", NULL};
   char *eout = NULL;
   assert(ws->exec(ws, argv, &eout, 4096) == 0);
   assert(eout && strstr(eout, "queue-ok") != NULL);
   free(eout);

   /* exec_stream: stdout streamed back as partials, reassembled by the collector.
    * This is the seam a local-CLI agent (claude -p) runs through on the client. */
   assert(ws->exec_stream != NULL);
   {
      const char *sargv[] = {"sh", "-c", "printf 'one\\ntwo\\nthree\\n'", NULL};
      chunk_collector_t cc = {0};
      int rc = ws->exec_stream(ws, sargv, NULL, 0, dir, collect_chunk, &cc);
      assert(rc == 0);
      assert(cc.buf && strstr(cc.buf, "one") && strstr(cc.buf, "two") && strstr(cc.buf, "three"));
      free(cc.buf);
   }

   /* exec_stream feeds stdin to the child (claude reads its prompt on stdin). */
   {
      const char *catargv[] = {"cat", NULL};
      const char prompt[] = "streamed-prompt-payload";
      chunk_collector_t cc = {0};
      int rc = ws->exec_stream(ws, catargv, prompt, sizeof(prompt) - 1, dir, collect_chunk, &cc);
      assert(rc == 0);
      assert(cc.buf && strcmp(cc.buf, prompt) == 0);
      free(cc.buf);
   }

   /* a failed exec_stream surfaces the child's non-zero exit status */
   {
      const char *failargv[] = {"sh", "-c", "exit 3", NULL};
      chunk_collector_t cc = {0};
      int rc = ws->exec_stream(ws, failargv, NULL, 0, dir, collect_chunk, &cc);
      assert(rc == 3);
      free(cc.buf);
   }

   /* close unblocks the runner thread's poll; transport after close fails */
   ws_runner_queue_close(&g_q);
   pthread_join(th, NULL);

   char *after = NULL;
   size_t alen = 0;
   assert(ws->read_all(ws, fpath, &after, &alen) == -1); /* closed -> transport fails */
   assert(after == NULL);

   ws_runner_queue_destroy(&g_q);
   unlink(fpath);
   rmdir(dir);

   printf("workspace_runner_queue: all tests passed\n");
   return 0;
}
