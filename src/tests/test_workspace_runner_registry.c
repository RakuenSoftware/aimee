/* test_workspace_runner_registry.c: the registry maps a workspace id to one
 * runner queue (get-or-create idempotent, lookup, remove, capacity bound), and
 * a registry-resolved queue carries a request/response round-trip. */
#include "modules/workspace/workspace_runner_registry.h"
#include "modules/workspace/workspace_runner_queue.h"
#include "cJSON.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static ws_runner_queue_t *g_rt;

/* The transport (server detached provider) side of the endpoint round-trip:
 * enqueue one "ping" op onto "ep"'s queue and block for the response. */
static cJSON *g_tx_resp;
static int g_tx_rc;
static void *transport_side(void *arg)
{
   (void)arg;
   ws_runner_queue_t *q = ws_runner_registry_get_or_create("ep");
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "op", "ping");
   g_tx_rc = ws_runner_queue_transport(q, req, &g_tx_resp);
   return NULL;
}

/* One-shot echo: take a request and answer {ok:true}. */
static void *echo_runner(void *arg)
{
   (void)arg;
   cJSON *req = ws_runner_queue_poll(g_rt, 5000);
   if (req)
   {
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddBoolToObject(resp, "ok", 1);
      cJSON_Delete(req);
      ws_runner_queue_respond(g_rt, resp);
   }
   return NULL;
}

int main(void)
{
   /* --- map semantics --- */
   ws_runner_queue_t *a1 = ws_runner_registry_get_or_create("a");
   assert(a1 != NULL);
   assert(ws_runner_registry_get_or_create("a") == a1); /* idempotent */
   assert(ws_runner_registry_lookup("a") == a1);
   assert(ws_runner_registry_lookup("missing") == NULL);

   ws_runner_queue_t *b1 = ws_runner_registry_get_or_create("b");
   assert(b1 != NULL && b1 != a1); /* distinct id -> distinct queue */

   ws_runner_registry_remove("a");
   assert(ws_runner_registry_lookup("a") == NULL);
   assert(ws_runner_registry_lookup("b") == b1); /* removing one leaves others */
   ws_runner_registry_remove("b");

   /* --- invalid ids --- */
   assert(ws_runner_registry_get_or_create("") == NULL);
   assert(ws_runner_registry_get_or_create(NULL) == NULL);

   /* --- round-trip through a registry-resolved queue --- */
   g_rt = ws_runner_registry_get_or_create("rt");
   assert(g_rt != NULL);
   pthread_t th;
   assert(pthread_create(&th, NULL, echo_runner, NULL) == 0);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "op", "stat");
   cJSON *resp = NULL;
   assert(ws_runner_queue_transport(g_rt, req, &resp) == 0);
   assert(resp && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "ok")));
   cJSON_Delete(resp);

   pthread_join(th, NULL);
   ws_runner_registry_remove("rt");

   /* --- endpoint helpers: poll/respond by id bridge to the transport --- */
   {
      /* The transport side (server detached provider) enqueues + blocks. */
      pthread_t tt;
      assert(pthread_create(&tt, NULL, transport_side, NULL) == 0);

      /* The client-serve side fetches the op by id and answers it. */
      cJSON *op = ws_runner_registry_poll("ep", 5000);
      assert(op != NULL);
      const cJSON *opname = cJSON_GetObjectItemCaseSensitive(op, "op");
      assert(cJSON_IsString(opname) && strcmp(opname->valuestring, "ping") == 0);
      cJSON_Delete(op);

      cJSON *r = cJSON_CreateObject();
      cJSON_AddBoolToObject(r, "ok", 1);
      assert(ws_runner_registry_respond("ep", r) == 0);

      pthread_join(tt, NULL);
      assert(g_tx_rc == 0 && g_tx_resp != NULL);
      assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g_tx_resp, "ok")));
      cJSON_Delete(g_tx_resp);
      ws_runner_registry_remove("ep");

      /* respond to an unregistered id frees the response and reports -1 */
      cJSON *orphan = cJSON_CreateObject();
      assert(ws_runner_registry_respond("no-such-id", orphan) == -1);
   }

   /* --- capacity bound: fill the table, the next create fails, then drain --- */
   char id[32];
   for (int i = 0; i < WS_RUNNER_REGISTRY_MAX; i++)
   {
      snprintf(id, sizeof(id), "cap%d", i);
      assert(ws_runner_registry_get_or_create(id) != NULL);
   }
   assert(ws_runner_registry_get_or_create("overflow") == NULL);
   for (int i = 0; i < WS_RUNNER_REGISTRY_MAX; i++)
   {
      snprintf(id, sizeof(id), "cap%d", i);
      ws_runner_registry_remove(id);
   }
   assert(ws_runner_registry_get_or_create("after-drain") != NULL);
   ws_runner_registry_remove("after-drain");

   /* --- lookup_for_path: who is serving this tree? --- */
   {
      assert(ws_runner_registry_get_or_create("/srv/repo") != NULL);

      /* The tree itself, and anything under it, is served by that runner. */
      assert(ws_runner_registry_lookup_for_path("/srv/repo") ==
             ws_runner_registry_lookup("/srv/repo"));
      assert(ws_runner_registry_lookup_for_path("/srv/repo/src/main.c") ==
             ws_runner_registry_lookup("/srv/repo"));

      /* Component boundary: a sibling whose name merely starts the same is NOT
       * served by it. Without the '/' check, /srv/repo-backup would be handed to
       * the client serving /srv/repo and edits would land in the wrong tree. */
      assert(ws_runner_registry_lookup_for_path("/srv/repo-backup") == NULL);
      assert(ws_runner_registry_lookup_for_path("/srv/other") == NULL);

      /* Nobody serving it is a clean no, not a manufactured queue. Answering
       * "yes" here by creating one would strand the turn on an empty queue. */
      assert(ws_runner_registry_lookup("/srv/other") == NULL);

      /* Longest match wins, so a nested runner takes precedence over its parent. */
      assert(ws_runner_registry_get_or_create("/srv/repo/vendor") != NULL);
      assert(ws_runner_registry_lookup_for_path("/srv/repo/vendor/lib.c") ==
             ws_runner_registry_lookup("/srv/repo/vendor"));
      /* ...and the parent still serves what the child does not cover. */
      assert(ws_runner_registry_lookup_for_path("/srv/repo/src/main.c") ==
             ws_runner_registry_lookup("/srv/repo"));

      assert(ws_runner_registry_lookup_for_path(NULL) == NULL);
      assert(ws_runner_registry_lookup_for_path("") == NULL);

      ws_runner_registry_remove("/srv/repo/vendor");
      ws_runner_registry_remove("/srv/repo");
      /* Once the client is gone, so is the answer. */
      assert(ws_runner_registry_lookup_for_path("/srv/repo/src/main.c") == NULL);
   }

   printf("workspace_runner_registry: all tests passed\n");
   return 0;
}
