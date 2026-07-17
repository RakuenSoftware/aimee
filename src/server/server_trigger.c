/* server_trigger.c: server-side handlers for the event-triggered autopilot API.
 *
 * Four methods are exposed:
 *   trigger.fire   — enqueue a new trigger run (auth-gated, concurrency-limited)
 *   trigger.list   — list trigger runs, optionally filtered by status
 *   trigger.status — fetch a single trigger run by id
 *   trigger.cancel — cancel a queued/running trigger run
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "server_trigger.h"
#include "server.h"
#include "config.h"
#include "db1/db1_trigger.h"
#include "db1/pipelines.h"
#include "cJSON.h"
#include "json_fluent.h" /* jo_ok */
#include "log.h"
#include "platform_random.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

/* Generate a trigger id of the form "trig_" + 16 random hex chars. */
static void gen_trigger_id(char *buf, size_t cap)
{
   unsigned char raw[8];
   if (platform_random_bytes(raw, sizeof(raw)) != 0)
   {
      unsigned int r = (unsigned int)time(NULL) ^ (unsigned int)clock();
      snprintf(buf, cap, "trig_%08x", r);
      return;
   }
   snprintf(buf, cap, "trig_%02x%02x%02x%02x%02x%02x%02x%02x", raw[0], raw[1], raw[2], raw[3],
            raw[4], raw[5], raw[6], raw[7]);
}

static int create_trigger_pipeline(const char *task, char *out, size_t out_len)
{
   int pipeline_id = 0;
   if (db1_pipeline_create(task, "simple", "simple", &pipeline_id) != 0 || pipeline_id <= 0)
      return -1;
   snprintf(out, out_len, "%d", pipeline_id);
   return 0;
}

/* ------------------------------------------------------------------ */
/* handle_trigger_fire                                                  */
/* ------------------------------------------------------------------ */

int handle_trigger_fire(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   /* 1. Load config */
   config_t cfg;
   config_load(&cfg);

   /* 2. Auth check */
   if (cfg.trigger_auth_token[0])
   {
      cJSON *tok_item = cJSON_GetObjectItemCaseSensitive(req, "auth_token");
      if (!tok_item)
         tok_item = cJSON_GetObjectItemCaseSensitive(req, "token");
      const char *tok = (tok_item && cJSON_IsString(tok_item)) ? tok_item->valuestring : "";
      if (strcmp(tok, cfg.trigger_auth_token) != 0)
         return server_send_error(conn, "unauthorized", NULL);
   }

   /* Manual one-at-a-time proposal fire: source=proposals + proposal=<name> files exactly
    * that pending proposal through the WFE pipeline, bypassing the default-off auto scan
    * (wfe_proposals_autoscan_enabled). This is the controlled 'send one proposal at a time'
    * path used while the autonomous pipeline is under test. */
   {
      cJSON *src = cJSON_GetObjectItemCaseSensitive(req, "source");
      cJSON *prop = cJSON_GetObjectItemCaseSensitive(req, "proposal");
      if (src && cJSON_IsString(src) && strcmp(src->valuestring, "proposals") == 0 && prop &&
          cJSON_IsString(prop) && prop->valuestring[0])
      {
         cJSON *ws = cJSON_GetObjectItemCaseSensitive(req, "workspace");
         if (!ws || !cJSON_IsString(ws) || !ws->valuestring[0])
            return server_send_error(conn, "workspace is required for a proposals fire", NULL);
         cJSON *pl = cJSON_GetObjectItemCaseSensitive(req, "pipeline");
         const char *pipeline =
             (pl && cJSON_IsString(pl) && pl->valuestring[0]) ? pl->valuestring : "build";
         cJSON *ev = cJSON_GetObjectItemCaseSensitive(req, "event");
         const char *event = (ev && cJSON_IsString(ev)) ? ev->valuestring : "";
         cJSON *rf = cJSON_GetObjectItemCaseSensitive(req, "ref");
         const char *ref = (rf && cJSON_IsString(rf)) ? rf->valuestring : "";
         cJSON *md = cJSON_GetObjectItemCaseSensitive(req, "mode");
         const char *mode = (md && cJSON_IsString(md)) ? md->valuestring : "";
         char wid[80];
         if (trigger_proposals_file_one(ws->valuestring, pipeline, event, ref, mode,
                                        prop->valuestring, wid) != 0)
            return server_send_error(
                conn, "proposal not filed (not found, already filed, or error)", NULL);
         cJSON *resp = jo_ok();
         cJSON_AddStringToObject(resp, "work_item_id", wid);
         cJSON_AddStringToObject(resp, "proposal", prop->valuestring);
         return server_send_ok(conn, resp);
      }
   }

   /* 3. Extract required + optional fields */
   cJSON *task_item = cJSON_GetObjectItemCaseSensitive(req, "task");
   if (!task_item || !cJSON_IsString(task_item) || !task_item->valuestring[0])
      return server_send_error(conn, "task is required", NULL);
   const char *task = task_item->valuestring;

   cJSON *source_item = cJSON_GetObjectItemCaseSensitive(req, "source");
   const char *source =
       (source_item && cJSON_IsString(source_item)) ? source_item->valuestring : "";

   cJSON *event_item = cJSON_GetObjectItemCaseSensitive(req, "event");
   const char *event = (event_item && cJSON_IsString(event_item)) ? event_item->valuestring : "";

   cJSON *ws_item = cJSON_GetObjectItemCaseSensitive(req, "workspace");
   const char *workspace = (ws_item && cJSON_IsString(ws_item)) ? ws_item->valuestring : "";

   /* metadata may be a JSON object/string; serialise to string if object */
   const char *metadata_str = "{}";
   char *metadata_alloc = NULL;
   cJSON *meta_item = cJSON_GetObjectItemCaseSensitive(req, "metadata");
   if (meta_item)
   {
      if (cJSON_IsString(meta_item))
      {
         metadata_str = meta_item->valuestring;
      }
      else
      {
         metadata_alloc = cJSON_PrintUnformatted(meta_item);
         if (metadata_alloc)
            metadata_str = metadata_alloc;
      }
   }

   /* 4. Generate id. trigger.max_concurrent gates future dispatch workers, not
    * queue admission; accepting queued work preserves trigger events during
    * bursts and lets the dispatcher drain them later. */
   char id[32];
   gen_trigger_id(id, sizeof(id));

   /* 5. Insert */
   int rc = db1_trigger_insert(id, source, event, task, workspace, metadata_str);
   free(metadata_alloc);
   if (rc != 0)
   {
      LOG_ERROR("trigger", "db1_trigger_insert failed for id=%s", id);
      return server_send_error(conn, "failed to enqueue trigger", NULL);
   }

   char pipeline_id[32] = "";
   if (create_trigger_pipeline(task, pipeline_id, sizeof(pipeline_id)) != 0)
   {
      db1_trigger_status_set(id, "failed", "", "failed to create pipeline");
      return server_send_error(conn, "failed to create pipeline", NULL);
   }
   if (db1_trigger_status_set(id, "queued", pipeline_id, "") != 0)
   {
      db1_pipeline_cancel(atoi(pipeline_id));
      return server_send_error(conn, "failed to link trigger pipeline", NULL);
   }

   /* 6. Send success */
   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "trigger_id", id);
   cJSON_AddStringToObject(resp, "pipeline_id", pipeline_id);
   cJSON_AddStringToObject(resp, "trigger_status", "queued");
   return server_send_ok(conn, resp);
}

/* ------------------------------------------------------------------ */
/* handle_trigger_list                                                  */
/* ------------------------------------------------------------------ */

int handle_trigger_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   /* Optional status filter */
   const char *status_filter = NULL;
   cJSON *sf_item = cJSON_GetObjectItemCaseSensitive(req, "status");
   if (sf_item && cJSON_IsString(sf_item) && sf_item->valuestring[0])
      status_filter = sf_item->valuestring;

   char *json_str = db1_trigger_list_json(status_filter);
   if (!json_str)
      return server_send_error(conn, "failed to list triggers", NULL);

   cJSON *arr = cJSON_Parse(json_str);
   free(json_str);

   cJSON *resp = jo_ok();
   if (arr)
      cJSON_AddItemToObject(resp, "triggers", arr);
   else
      cJSON_AddItemToObject(resp, "triggers", cJSON_CreateArray());

   return server_send_ok(conn, resp);
}

/* ------------------------------------------------------------------ */
/* handle_trigger_status                                                */
/* ------------------------------------------------------------------ */

int handle_trigger_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *id_item = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (!id_item || !cJSON_IsString(id_item) || !id_item->valuestring[0])
      return server_send_error(conn, "id is required", NULL);

   db1_trigger_run_t run;
   if (db1_trigger_get(id_item->valuestring, &run) != 0)
      return server_send_error(conn, "not found", NULL);

   cJSON *resp = jo_ok();
   cJSON *obj = cJSON_AddObjectToObject(resp, "trigger");
   cJSON_AddStringToObject(obj, "id", run.id);
   cJSON_AddStringToObject(obj, "source", run.source);
   cJSON_AddStringToObject(obj, "event", run.event);
   cJSON_AddStringToObject(obj, "task", run.task);
   cJSON_AddStringToObject(obj, "workspace", run.workspace);
   cJSON_AddStringToObject(obj, "metadata", run.metadata);
   cJSON_AddStringToObject(obj, "pipeline_id", run.pipeline_id);
   cJSON_AddStringToObject(obj, "status", run.status);
   cJSON_AddStringToObject(obj, "queued_at", run.queued_at);
   cJSON_AddStringToObject(obj, "started_at", run.started_at);
   cJSON_AddStringToObject(obj, "finished_at", run.finished_at);
   cJSON_AddStringToObject(obj, "error", run.error);
   cJSON_AddStringToObject(resp, "trigger_status", run.status);

   return server_send_ok(conn, resp);
}

/* ------------------------------------------------------------------ */
/* handle_trigger_cancel                                                */
/* ------------------------------------------------------------------ */

int handle_trigger_cancel(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *id_item = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (!id_item || !cJSON_IsString(id_item) || !id_item->valuestring[0])
      return server_send_error(conn, "id is required", NULL);

   db1_trigger_run_t run;
   int have_run = (db1_trigger_get(id_item->valuestring, &run) == 0);
   const char *pipeline_id = have_run ? run.pipeline_id : "";
   int rc = db1_trigger_status_set(id_item->valuestring, "cancelled", pipeline_id, NULL);
   if (rc != 0)
      return server_send_error(conn, "not found or already complete", NULL);
   if (pipeline_id[0])
      db1_pipeline_cancel(atoi(pipeline_id));

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "trigger_id", id_item->valuestring);
   cJSON_AddStringToObject(resp, "trigger_status", "cancelled");
   return server_send_ok(conn, resp);
}
