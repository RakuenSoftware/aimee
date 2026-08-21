/* server/server_config.c: RPC handlers for the top-level `config` command.
 *
 * Exposes the get/set allowlist in config_fields.c over typed server methods so
 * the thin client (and any /v1 caller) can read and update aimee.yaml without a
 * local legacy_config_read. Mirrors handle_aux_config_show in server_jobs_aux.c. */
#include "aimee.h"
#include "cJSON.h"
#include "config.h"
#include "config_client.h"
#include "config_database.h" /* config_emit_deploy_env_current */
#include "json_fluent.h" /* jo_ok */
#include "server.h"
#include "server_http.h"
#include "server_http_identity.h"
#include "runtime_secret.h"
#include <string.h>

/* config.show: return every allowlisted field and its current value. */
int handle_config_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   if (!config_present())
      return server_send_error(conn, "config: could not load configuration", NULL);

   cJSON *obj = config_client_snapshot_copy();
   /* Advertise the surface group of every non-runtime key so the Settings GUI can
    * hide deploy/advanced/dev keys by default. Additive + non-breaking: the flat
    * `config` map still carries EVERY key's value, so `aimee config show` and any
    * existing consumer are unchanged; a client that ignores `groups` sees all. */
   cJSON *groups = cJSON_CreateObject();
   cJSON *secrets = cJSON_CreateObject();
   if (!obj)
      return server_send_error(conn, "config: could not read configuration", NULL);
   for (cJSON *item = obj->child, *next = NULL; item; item = next)
   {
      next = item->next;
      if (config_client_key_is_secret(item->string))
      {
         cJSON_AddBoolToObject(secrets, item->string, 1);
         cJSON_ReplaceItemInObjectCaseSensitive(obj, item->string, cJSON_CreateBool(0));
      }
   }

   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "config", obj);
   cJSON_AddItemToObject(resp, "groups", groups);
   cJSON_AddItemToObject(resp, "secrets", secrets);
   return server_send_ok(conn, resp);
}

/* config.get: return one field's value. */
int handle_config_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *key = jo_str(req, "key", "");
   if (!key || !key[0])
      return server_send_error(conn, "usage: aimee config get <key>", NULL);

   if (!config_present())
      return server_send_error(conn, "config: could not load configuration", NULL);

   int secret = config_client_key_is_secret(key);
   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "key", key);
   cJSON *value = NULL;
   if (secret)
   {
      const char *secret_name = config_client_secret_name(key);
      value = cJSON_CreateBool(secret_name && runtime_secret_has(secret_name));
   }
   else
   {
      value = config_client_value_copy(key);
      if (!value)
      {
         cJSON_Delete(resp);
         return server_send_error(conn, "config: unknown key", NULL);
      }
   }
   cJSON_AddItemToObject(resp, "value", value);
   cJSON_AddBoolToObject(resp, "secret", secret);
   return server_send_ok(conn, resp);
}

/* config.deploy_env: emit the compose environment for this backend record.
 *
 * Routed rather than local-only because the operator CLI on a managed deployment
 * IS the thin client, and this command's whole purpose is the recreate wrapper in
 * cmd_data.c: `eval "$(aimee config deploy-env)" && docker compose up -d`. Without
 * a route that wrapper cannot run where it is needed, and recreating a managed
 * container by hand silently drops every variable the compose file interpolates --
 * EMBEDDER_MODEL (the kb then refuses to serve, loudly) and AIMEE_KB_VARIANT (the
 * image resolves to the embedderless aimee-kb, quietly), which is exactly the
 * regression config_emit_deploy_env's own comments were written to prevent.
 *
 * No secret reaches this output: config_emit_deploy_env deliberately omits
 * embedder_api_key and synthesis_api_key, and check-vault-only-container-env
 * enforces that. What remains is a strict subset of config.show's sensitivity,
 * so it carries config.show's capability rather than an admin-only one. */
int handle_config_deploy_env(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   if (!config_present())
      return server_send_error(conn, "config: could not load configuration", NULL);

   char env[4096];
   config_emit_deploy_env_current(env, sizeof(env));

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "env", env);
   return server_send_ok(conn, resp);
}

/* config.set: assign one field and persist aimee.yaml. */
int handle_config_set(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *operation = jo_str(req, "operation", "");
   if (operation && operation[0])
   {
      if (strcmp(operation, "profile-create") != 0 &&
          strcmp(operation, "profile-present") != 0)
         return server_send_error(conn, "config: unsupported structured operation", NULL);
      cJSON *value = cJSON_GetObjectItemCaseSensitive(req, "value");
      if (!cJSON_IsObject(value))
         return server_send_error(conn, "config: structured operation requires an object", NULL);
      int rc = config_client_operation(operation, cJSON_Duplicate(value, 1));
      int present = rc == 0;
      if (!strcmp(operation, "profile-present") && rc == -2)
         rc = 0;
      if (rc != 0)
         return server_send_error(conn, "config: structured operation failed", NULL);
      cJSON *resp = jo_ok();
      cJSON_AddStringToObject(resp, "operation", operation);
      if (!strcmp(operation, "profile-present"))
         cJSON_AddBoolToObject(resp, "present", present);
      return server_send_ok(conn, resp);
   }

   const char *key = jo_str(req, "key", "");
   if (!key || !key[0])
      return server_send_error(conn, "usage: aimee config set <key> <value>", NULL);

   /* Do not let the generic config surface bypass the roundtable preset API's
    * operator boundary (notably through roundtable.default). Local UDS callers
    * have full generic capabilities, but remain uid: principals; only the
    * root-UDS-attested appliance administrator may alter this policy. */
   if (roundtable_policy_config_key(key) &&
       !route_roundtable_mutation_authorized(server_http_identity_principal()))
      return server_send_error(
          conn, "roundtable changes require the authenticated appliance administrator", NULL);

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

   if (config_client_key_is_secret(key))
   {
      const char *secret_name = config_client_secret_name(key);
      int configured = value[0] ? 1 : 0;
      int stored = config_secret_store(secret_name, value);
      if (cJSON_IsString(jval) && jval->valuestring)
         runtime_secret_wipe(jval->valuestring, strlen(jval->valuestring));
      if (stored != 0)
         return server_send_error(conn, "config: could not store credential in Vault", NULL);
      cJSON *resp = jo_ok();
      cJSON_AddStringToObject(resp, "key", key);
      cJSON_AddBoolToObject(resp, "value", configured);
      cJSON_AddBoolToObject(resp, "secret", 1);
      cJSON_AddStringToObject(resp, "reload", "hot");
      cJSON_AddBoolToObject(resp, "applied_live", 1);
      return server_send_ok(conn, resp);
   }

   /* config_set patches the key in the DOCUMENT on disk (not the live snapshot),
    * so a config.set never clobbers an external edit made to the file since the
    * last reload (live-config-reload P1b), validates against the field
    * descriptor, and republishes -- the three steps this did by hand. */
   if (config_set(key, value) != 0)
      return server_send_error(conn, "config: invalid value for key", NULL);

   /* Push the change into the live snapshot NOW so it takes effect immediately for every
    * legacy_config_read reader, instead of waiting for an mtime-cache miss (live-config-reload P1b). */
   (void)config_reload();

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "key", key);
   cJSON *current = config_client_value_copy(key);
   if (!current)
      current = cJSON_CreateNull();
   cJSON_AddItemToObject(resp, "value", current);
   cJSON_AddBoolToObject(resp, "secret", 0);
   /* Live/Restart verdict (live-config-reload P2): tell the caller whether the change is in
    * effect now or needs a restart, instead of leaving them to guess. */
   cJSON_AddStringToObject(resp, "reload", "hot");
   cJSON_AddBoolToObject(resp, "applied_live", 1);
   return server_send_ok(conn, resp);
}
