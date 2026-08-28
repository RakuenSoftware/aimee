/* server_state_vectors.c: the two vector-maintenance relays.
 *
 * Its own translation unit because server_state.c sits at the 2500-line ceiling
 * line-check enforces. aimee-kb owns both operations and all their gating; the
 * server only forwards flags and reports what came back.
 */
#include "server.h"
#include "server_state_internal.h"

#include "aimee.h"
#include "cJSON.h"
#include "json_fluent.h"
#include "kb_client.h"
#include "db1_client/server_sessions.h"
#include "platform_random.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* As kb_relay_send, but a kb-reported failure is returned AS an error.
 *
 * kb_relay_send hands any parseable body straight through, so a refusal arrives
 * as a successful response that merely contains an error field -- the CLI has no
 * printer for it, so the operator sees nothing and exit 0. Now that the client
 * preserves the kb's body on a non-2xx (kb_client_v1_post_json_keep_error), the
 * reason is present and worth surfacing. Recognises both shapes the kb uses:
 * {"error": "..."} and {"status": "error"|"unavailable", ...}. */
static int kb_relay_send_checked(server_conn_t *conn, char *json, const char *fallback)
{
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, fallback, NULL);

   const cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
   const cJSON *st = cJSON_GetObjectItemCaseSensitive(resp, "status");
   const char *reason = NULL;
   if (cJSON_IsString(err) && err->valuestring && err->valuestring[0])
      reason = err->valuestring;
   else if (cJSON_IsString(st) && st->valuestring &&
            (strcmp(st->valuestring, "error") == 0 || strcmp(st->valuestring, "unavailable") == 0))
   {
      const cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      reason = (cJSON_IsString(msg) && msg->valuestring[0]) ? msg->valuestring : fallback;
   }
   if (reason)
   {
      char msg[512];
      snprintf(msg, sizeof(msg), "%s", reason);
      cJSON_Delete(resp);
      return server_send_error(conn, msg, NULL);
   }
   return send_and_free(conn, resp);
}

/* kb.reembed — the double-gated embedder dimension-change reset.
 *
 * aimee-kb owns the operation (POST /v1/reembed); this only relays. Without this
 * route the command existed in kb_subcmds[] and in the docs but could not be
 * reached from a managed appliance at all: the CLI dispatches over /v1 there, and
 * "reembed" was absent from the route table, so it failed with "'reembed' is not
 * a subcommand of 'kb'". That left the documented remedy for embedder drift
 * unusable on exactly the deployment shape that needs it — a real 1024 -> 384
 * migration had to POST to the kb directly.
 *
 * Every flag is forwarded verbatim; the gating (kb.reembed_on_dim_change, and
 * confirm vs dry-run) stays server-side in the kb, which is where it belongs. */
int handle_kb_reembed(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   int confirm = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "confirm"));
   int force = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "force"));
   int dry_run = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "dry_run"));
   int clear_maintenance = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "clear_maintenance"));
   const cJSON *td = cJSON_GetObjectItemCaseSensitive(req, "target_dim");
   int target_dim = cJSON_IsNumber(td) ? (int)td->valuedouble : 0;

   int status = 0;
   char *json = kb_client_reembed(confirm, force, dry_run, target_dim, clear_maintenance, &status);
   return kb_relay_send_checked(conn, json, "knowledge service reembed failed");
}

/* memory.embed — (re)generate memory embeddings.
 *
 * The kb already handles it (kb_service.c "memory.embed") and the client call
 * already existed; only the server route was missing, so on a managed appliance
 * `aimee memory embed --all` answered "'embed' is not a subcommand of 'memory'".
 *
 * That matters after a dimension change: kb.reembed drops memory_embeddings and
 * requeues curator work, but memories are embedded on write and nothing requeues
 * them, so their dense retrieval stays dead until something re-embeds. On prod a
 * 1024 -> 384 migration left 14 memories with no vectors and no reachable way to
 * rebuild them. */
int handle_memory_embed(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   int all = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "all"));
   const cJSON *mid = cJSON_GetObjectItemCaseSensitive(req, "memory_id");
   int64_t memory_id = cJSON_IsNumber(mid) ? (int64_t)mid->valuedouble : 0;
   const cJSON *ver = cJSON_GetObjectItemCaseSensitive(req, "version");
   const char *version = cJSON_IsString(ver) ? ver->valuestring : NULL;
   if (!all && memory_id <= 0)
      return server_send_error(conn, "memory.embed requires all=true or memory_id", NULL);

   return kb_relay_send_checked(conn, kb_client_memory_embed_json(all, memory_id, version, NULL),
                                "knowledge service memory embed failed");
}

int handle_kb_erase_subject(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const cJSON *js = cJSON_GetObjectItemCaseSensitive(req, "subject");
   const cJSON *jr = cJSON_GetObjectItemCaseSensitive(req, "request_id");
   if (!cJSON_IsString(js) || !js->valuestring[0] || strlen(js->valuestring) > 600)
      return server_send_error(conn, "kb.erase-subject requires subject", NULL);
   char request_id[129] = "";
   if (cJSON_IsString(jr) && jr->valuestring[0])
   {
      size_t request_len = strlen(jr->valuestring);
      if (request_len < 16 || request_len > 128)
         return server_send_error(conn, "invalid erasure request_id", NULL);
      for (size_t i = 0; i < request_len; i++)
      {
         unsigned char c = (unsigned char)jr->valuestring[i];
         if (!isalnum(c) && c != '.' && c != '_' && c != '-')
            return server_send_error(conn, "invalid erasure request_id", NULL);
      }
      snprintf(request_id, sizeof(request_id), "%s", jr->valuestring);
   }
   else
   {
      unsigned char random[16];
      if (platform_random_bytes(random, sizeof(random)) != 0)
         return server_send_error(conn, "cannot mint erasure request id", NULL);
      snprintf(request_id, sizeof(request_id), "erase-");
      for (size_t i = 0; i < sizeof(random); i++)
         snprintf(request_id + 6 + i * 2, sizeof(request_id) - 6 - i * 2, "%02x", random[i]);
   }
   char(*sessions)[DB1_SS_ID_LEN] = calloc(4096, sizeof(*sessions));
   if (!sessions)
      return server_send_error(conn, "out of memory", NULL);
   int session_count = db1_server_session_list_by_subject(js->valuestring, sessions, 4096);
   if (session_count < 0 || session_count == 4096)
   {
      free(sessions);
      return server_send_error(conn, "cannot enumerate complete subject session set", NULL);
   }
   cJSON *session_ids = cJSON_CreateArray();
   for (int i = 0; session_ids && i < session_count; i++)
      cJSON_AddItemToArray(session_ids, cJSON_CreateString(sessions[i]));
   free(sessions);
   if (!session_ids)
      return server_send_error(conn, "out of memory", NULL);

   int status = 0;
   char *begin_json =
       kb_client_subject_erasure_begin(request_id, js->valuestring, session_ids, &status);
   cJSON_Delete(session_ids);
   cJSON *begin = begin_json ? cJSON_Parse(begin_json) : NULL;
   free(begin_json);
   if (status != 200 || !begin)
   {
      cJSON_Delete(begin);
      char msg[192];
      snprintf(msg, sizeof(msg), "DB2 erasure failed; retry request_id=%s", request_id);
      return server_send_error(conn, msg, NULL);
   }

   int db1_count = db1_server_session_erase_subject(request_id, js->valuestring);
   if (db1_count < 0)
   {
      cJSON_Delete(begin);
      char msg[192];
      snprintf(msg, sizeof(msg), "DB1 erasure failed; retry request_id=%s", request_id);
      return server_send_error(conn, msg, NULL);
   }
   char *complete_json = kb_client_subject_erasure_complete(request_id, db1_count, &status);
   cJSON *complete = complete_json ? cJSON_Parse(complete_json) : NULL;
   free(complete_json);
   if (status != 200 || !complete)
   {
      cJSON_Delete(begin);
      cJSON_Delete(complete);
      char msg[192];
      snprintf(msg, sizeof(msg), "completion evidence failed; retry request_id=%s", request_id);
      return server_send_error(conn, msg, NULL);
   }

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "request_id", request_id);
   cJSON_AddStringToObject(resp, "subject", js->valuestring);
   cJSON_AddNumberToObject(resp, "session_count", db1_count);
   const cJSON *mc = cJSON_GetObjectItemCaseSensitive(begin, "memory_count");
   const cJSON *dc = cJSON_GetObjectItemCaseSensitive(begin, "document_count");
   const cJSON *ec = cJSON_GetObjectItemCaseSensitive(complete, "event_created");
   cJSON_AddNumberToObject(resp, "memory_count", cJSON_IsNumber(mc) ? mc->valuedouble : 0);
   cJSON_AddNumberToObject(resp, "document_count", cJSON_IsNumber(dc) ? dc->valuedouble : 0);
   cJSON_AddBoolToObject(resp, "event_created", cJSON_IsTrue(ec));
   cJSON_Delete(begin);
   cJSON_Delete(complete);
   return server_send_ok(conn, resp);
}
