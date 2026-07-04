/* server/server_config.c: RPC handlers for the top-level `config` command.
 *
 * Exposes the get/set allowlist in config_fields.c over typed server methods so
 * the thin client (and any /v1 caller) can read and update aimee.yaml without a
 * local config_load. Mirrors handle_aux_config_show in server_jobs_aux.c. */
#include "aimee.h"
#include "cJSON.h"
#include "config.h"
#include "config_fields.h"
#include "json_fluent.h" /* jo_ok */
#include "server.h"
#include <string.h>

/* config.show: return every allowlisted field and its current value. */
int handle_config_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   config_t cfg;
   if (config_load(&cfg) != 0)
      return server_send_error(conn, "config: could not load configuration", NULL);

   cJSON *obj = cJSON_CreateObject();
   for (int i = 0; config_fields[i].key; i++)
      cJSON_AddItemToObject(obj, config_fields[i].key,
                            config_field_value_json(&cfg, &config_fields[i]));

   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "config", obj);
   return server_send_ok(conn, resp);
}

/* config.get: return one field's value. */
int handle_config_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *key = jo_str(req, "key", "");
   if (!key || !key[0])
      return server_send_error(conn, "usage: aimee config get <key>", NULL);

   const config_field_t *f = config_field_lookup(key);
   if (!f)
      return server_send_error(conn, "config: unknown key", NULL);

   config_t cfg;
   if (config_load(&cfg) != 0)
      return server_send_error(conn, "config: could not load configuration", NULL);

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "key", key);
   cJSON_AddItemToObject(resp, "value", config_field_value_json(&cfg, f));
   return server_send_ok(conn, resp);
}

/* config.set: assign one field and persist aimee.yaml. */
int handle_config_set(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *key = jo_str(req, "key", "");
   if (!key || !key[0])
      return server_send_error(conn, "usage: aimee config set <key> <value>", NULL);

   /* Accept any JSON scalar for value; coerce to string for the field parser. */
   cJSON *jval = cJSON_GetObjectItemCaseSensitive(req, "value");
   char numbuf[64];
   const char *value = NULL;
   if (cJSON_IsString(jval))
      value = jval->valuestring;
   else if (cJSON_IsBool(jval))
      value = cJSON_IsTrue(jval) ? "true" : "false";
   else if (cJSON_IsNumber(jval))
   {
      double d = jval->valuedouble;
      if (d == (double)(long)d)
         snprintf(numbuf, sizeof(numbuf), "%ld", (long)d);
      else
         snprintf(numbuf, sizeof(numbuf), "%g", d);
      value = numbuf;
   }
   if (!value)
      return server_send_error(conn, "usage: aimee config set <key> <value>", NULL);

   const config_field_t *f = config_field_lookup(key);
   if (!f)
      return server_send_error(conn, "config: unknown key", NULL);

   config_t cfg;
   if (config_load(&cfg) != 0)
      return server_send_error(conn, "config: could not load configuration", NULL);

   if (config_field_set_value(&cfg, f, value) != 0)
      return server_send_error(conn, "config: invalid value for key", NULL);

   if (config_save(&cfg) != 0)
      return server_send_error(conn, "config: could not save configuration", NULL);

   /* Push the change into the live snapshot NOW so it takes effect immediately for every
    * config_load reader, instead of waiting for an mtime-cache miss (live-config-reload P1b). */
   (void)config_reload();

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "key", key);
   cJSON_AddItemToObject(resp, "value", config_field_value_json(&cfg, f));
   /* One-time setup warning: enabling delegate use of Claude-via-CLI carries an
    * Anthropic account-action risk. Surfaced here (not on every delegate call)
    * and in DELEGATES.md. */
   if (strcmp(key, "claude_cli_delegate_enabled") == 0 && cfg.claude_cli_delegate_enabled)
      cJSON_AddStringToObject(
          resp, "notice",
          "WARNING: Claude run via the `claude` CLI / tmux login (not an API key) can now be used "
          "as a delegate. Driving a personal Anthropic Claude subscription through "
          "automated/headless delegation may violate Anthropic's terms of service and risk "
          "suspension or termination of your account. Use an Anthropic API key (billed per token) "
          "for automated/delegated workloads. See DELEGATES.md.");
   return server_send_ok(conn, resp);
}
