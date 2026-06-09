/* server_plugin.c: server handlers for plugin registry operations */
#include "aimee.h"
#include "server.h"
#include "plugin.h"
#include "cJSON.h"
#include "json_fluent.h"

static int send_and_free(server_conn_t *conn, cJSON *resp)
{
   return server_send_ok(conn, resp);
}

static cJSON *parse_or_array(char *json)
{
   cJSON *v = json ? cJSON_Parse(json) : NULL;
   return v ? v : cJSON_CreateArray();
}

int handle_plugin_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   char *json = plugin_registry_json();
   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "data", parse_or_array(json));
   free(json);
   return send_and_free(conn, resp);
}

static int handle_plugin_set_enabled(server_ctx_t *ctx, server_conn_t *conn, cJSON *req,
                                     int enabled)
{
   (void)ctx;
   cJSON *name_item = cJSON_GetObjectItemCaseSensitive(req, "name");
   if (!cJSON_IsString(name_item) || !name_item->valuestring || !name_item->valuestring[0])
      return send_and_free(conn, jo_err("plugin name required"));

   char err[512];
   if (plugin_set_enabled(name_item->valuestring, enabled, err, sizeof(err)) != 0)
      return send_and_free(conn, jo_err(err[0] ? err : "plugin update failed"));

   char *json = plugin_registry_json();
   cJSON *plugins = parse_or_array(json);
   free(json);

   cJSON *resp = jo_ok();
   cJSON *selected = NULL;
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, plugins)
   {
      cJSON *plugin_name = cJSON_GetObjectItemCaseSensitive(item, "name");
      if (cJSON_IsString(plugin_name) &&
          strcmp(plugin_name->valuestring, name_item->valuestring) == 0)
      {
         selected = cJSON_Duplicate(item, 1);
         break;
      }
   }
   cJSON_Delete(plugins);
   if (selected)
      cJSON_AddItemToObject(resp, "data", selected);
   else
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "name", name_item->valuestring);
      cJSON_AddBoolToObject(obj, "enabled", enabled);
      cJSON_AddItemToObject(resp, "data", obj);
   }
   return send_and_free(conn, resp);
}

int handle_plugin_enable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return handle_plugin_set_enabled(ctx, conn, req, 1);
}

int handle_plugin_disable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return handle_plugin_set_enabled(ctx, conn, req, 0);
}
