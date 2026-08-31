/* workspace_runner_queue.c — hand one op to the client serving a tree, over the
 * bus. See workspace_runner_queue.h.
 *
 * The handoff itself is in the workspace module. What was here was a condition
 * variable, a FIFO, and a single-in-flight discipline, which is logic and not
 * transport; it is channels in Go now. This file only marshals a cJSON op onto
 * the wire and reads the answer back. */
#include "workspace_runner_queue.h"
#include "workspace_runner_registry.h"
#include "cJSON.h"

#include <aimee/workspace/module_api.h>

#include <stdlib.h>
#include <string.h>

/* Same bound as a submit: the caller is waiting on real work. */
#define WS_RUNNER_OP_MS 600000ULL

void ws_runner_queue_init(ws_runner_queue_t *q)
{
   if (q)
      q->id[0] = '\0';
}

void ws_runner_queue_destroy(ws_runner_queue_t *q)
{
   (void)q; /* nothing is owned here any more */
}

void ws_runner_queue_close(ws_runner_queue_t *q)
{
   /* Waking blocked waiters was this file's job when the waiting happened here.
    * It happens in the module now, and unregistering the tree is what releases
    * anyone parked on it. */
   (void)q;
}

/* Send `request` and return the module's answer. Consumes `request` either way,
 * per ws_detached_transport_fn. */
static int queue_send(ws_runner_queue_t *q, unsigned op, cJSON *request, char **out,
                      size_t *out_len, int *more)
{
   int had_request = request != NULL;
   char *text = request ? cJSON_PrintUnformatted(request) : NULL;
   cJSON_Delete(request); /* consumed either way, per ws_detached_transport_fn */
   if (!q || !q->id[0] || (had_request && !text))
   {
      free(text);
      return -1;
   }
   int rc =
       ws_runner_io(op, q->id, text, text ? strlen(text) : 0, out, out_len, more, WS_RUNNER_OP_MS);
   free(text);
   return rc;
}

int ws_runner_queue_transport(void *ctx, cJSON *request, cJSON **response)
{
   if (response)
      *response = NULL;
   char *body = NULL;
   size_t body_len = 0;
   int more = 0;
   if (queue_send((ws_runner_queue_t *)ctx, AIMEE_WS_IO_OP_SUBMIT, request, &body, &body_len,
                  &more) != 0)
      return -1;
   cJSON *parsed = body_len ? cJSON_Parse(body) : NULL;
   free(body);
   if (!parsed)
      return -1;
   /* A unary op answers in one chunk. Being told more is coming means the client
    * streamed a reply to a caller that cannot read one, so refuse rather than
    * silently keep the first piece and drop the rest. */
   if (more)
   {
      cJSON_Delete(parsed);
      return -1;
   }
   if (response)
      *response = parsed;
   else
      cJSON_Delete(parsed);
   return 0;
}

int ws_runner_queue_transport_stream(void *ctx, cJSON *request, ws_runner_partial_fn on_partial,
                                     void *cb_ctx, cJSON **final)
{
   if (final)
      *final = NULL;
   ws_runner_queue_t *q = (ws_runner_queue_t *)ctx;
   char *body = NULL;
   size_t body_len = 0;
   int more = 0;
   if (queue_send(q, AIMEE_WS_IO_OP_SUBMIT, request, &body, &body_len, &more) != 0)
      return -1;

   for (;;)
   {
      cJSON *chunk = body_len ? cJSON_Parse(body) : NULL;
      free(body);
      body = NULL;
      body_len = 0;
      if (!chunk)
         return -1;

      if (!more)
      {
         if (final)
            *final = chunk;
         else
            cJSON_Delete(chunk);
         return 0;
      }

      /* A partial is borrowed by the callback, exactly as before. */
      int abort_stream = on_partial ? on_partial(cb_ctx, chunk) : 0;
      cJSON_Delete(chunk);
      if (abort_stream)
         return -1;

      if (ws_runner_io(AIMEE_WS_IO_OP_DRAIN, q->id, NULL, 0, &body, &body_len, &more,
                       WS_RUNNER_OP_MS) != 0)
         return -1;
   }
}

cJSON *ws_runner_queue_poll(ws_runner_queue_t *q, int timeout_ms)
{
   /* This wrapper has no way to report "unserved" to its caller, so it does not
    * pretend to: the distinction is made and acted on at the two poll endpoints
    * that own a retry loop. */
   return q ? ws_runner_registry_poll(q->id, timeout_ms, NULL) : NULL;
}

int ws_runner_queue_respond(ws_runner_queue_t *q, cJSON *response)
{
   if (!q)
   {
      cJSON_Delete(response);
      return -1;
   }
   return ws_runner_registry_respond(q->id, response);
}
