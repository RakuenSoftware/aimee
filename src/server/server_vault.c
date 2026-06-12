/* server_vault.c: /v1 route handlers for the credential vault (WP-C.1). Thin
 * adapters — parse the request, resolve the attested principal/transport from the
 * connection (WP-C.0, never from the request body), call vault_service, format
 * the response. All policy + crypto lives below in vault_service. */
#include "server.h" /* server_conn_t, server_send_*, handle_vault_* decls */
#include "vault_service.h"
#include "vault_crypto.h" /* VAULT_ROOT_KEY_LEN */
#include "cJSON.h"
#include <openssl/crypto.h>
#include <string.h>
#include <time.h>

/* Map a non-OK vault status to a client error response. */
static int vault_send_status_error(server_conn_t *conn, vault_status_t st)
{
   const char *msg;
   switch (st)
   {
   case VAULT_ERR_UNATTESTED:
      msg = "vault: this connection has no attested local identity";
      break;
   case VAULT_ERR_TRANSPORT:
      msg = "vault: operation not permitted on this transport (root key push is UDS-only)";
      break;
   case VAULT_ERR_LOCKED:
      msg = "vault locked: run `aimee vault unlock`";
      break;
   case VAULT_ERR_BADARG:
      msg = "vault: missing or invalid argument";
      break;
   case VAULT_ERR_CRYPTO:
      msg = "vault: cryptographic operation failed";
      break;
   case VAULT_ERR_IO:
      msg = "vault: storage error";
      break;
   default:
      msg = "vault: error";
      break;
   }
   return server_send_error(conn, msg, NULL);
}

/* Decode `hex` (2*n chars) into out[n]; returns 0 on success, -1 otherwise. */
static int hex_decode(const char *hex, uint8_t *out, size_t n)
{
   if (!hex || strlen(hex) != n * 2)
      return -1;
   for (size_t i = 0; i < n; i++)
   {
      int hi = -1, lo = -1;
      char c = hex[i * 2], d = hex[i * 2 + 1];
      hi = (c >= '0' && c <= '9')   ? c - '0'
           : (c >= 'a' && c <= 'f') ? c - 'a' + 10
           : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                    : -1;
      lo = (d >= '0' && d <= '9')   ? d - '0'
           : (d >= 'a' && d <= 'f') ? d - 'a' + 10
           : (d >= 'A' && d <= 'F') ? d - 'A' + 10
                                    : -1;
      if (hi < 0 || lo < 0)
         return -1;
      out[i] = (uint8_t)((hi << 4) | lo);
   }
   return 0;
}

/* POST /v1/vault/unlock — derive + cache the KEK from the client root key. The
 * root key is a 32-byte value sent hex-encoded; it is UDS-only and cleansed
 * immediately after derivation. */
int handle_vault_unlock(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jrk = cJSON_GetObjectItemCaseSensitive(req, "root_key_hex");
   if (!cJSON_IsString(jrk))
      return server_send_error(conn, "vault: missing root_key_hex", NULL);

   uint8_t root_key[VAULT_ROOT_KEY_LEN];
   if (hex_decode(jrk->valuestring, root_key, sizeof(root_key)) != 0)
      return server_send_error(conn, "vault: root_key_hex must be 64 hex chars", NULL);

   vault_status_t st = vault_service_unlock(conn->vault_principal, conn->attested_transport,
                                            root_key, sizeof(root_key), time(NULL));
   OPENSSL_cleanse(root_key, sizeof(root_key));
   if (st != VAULT_OK)
      return vault_send_status_error(conn, st);

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return server_send_error(conn, "vault: out of memory", NULL);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "principal", conn->vault_principal);
   return server_send_ok(conn, resp);
}

/* POST /v1/vault/set — store a credential under the unlocked vault. */
int handle_vault_set(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *ja = cJSON_GetObjectItemCaseSensitive(req, "agent");
   cJSON *jc = cJSON_GetObjectItemCaseSensitive(req, "cred");
   cJSON *js = cJSON_GetObjectItemCaseSensitive(req, "secret");
   if (!cJSON_IsString(ja) || !cJSON_IsString(jc) || !cJSON_IsString(js))
      return server_send_error(conn, "vault: set requires agent, cred, secret", NULL);

   vault_status_t st = vault_service_set(conn->vault_principal, ja->valuestring, jc->valuestring,
                                         js->valuestring, time(NULL));
   if (st != VAULT_OK)
      return vault_send_status_error(conn, st);

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return server_send_error(conn, "vault: out of memory", NULL);
   cJSON_AddStringToObject(resp, "status", "ok");
   return server_send_ok(conn, resp);
}

/* POST /v1/vault/list — names only, never secrets. */
int handle_vault_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   vault_store_entry_t entries[64];
   int count = 0;
   vault_status_t st = vault_service_list(conn->vault_principal, entries,
                                          (int)(sizeof(entries) / sizeof(entries[0])), &count);
   if (st != VAULT_OK)
      return vault_send_status_error(conn, st);

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return server_send_error(conn, "vault: out of memory", NULL);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *arr = cJSON_AddArrayToObject(resp, "credentials");
   for (int i = 0; arr && i < count; i++)
   {
      cJSON *e = cJSON_CreateObject();
      if (!e)
         break;
      cJSON_AddStringToObject(e, "agent", entries[i].agent);
      cJSON_AddStringToObject(e, "cred", entries[i].cred);
      cJSON_AddItemToArray(arr, e);
   }
   return server_send_ok(conn, resp);
}

/* POST /v1/vault/delete — remove a credential. */
int handle_vault_delete(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *ja = cJSON_GetObjectItemCaseSensitive(req, "agent");
   cJSON *jc = cJSON_GetObjectItemCaseSensitive(req, "cred");
   if (!cJSON_IsString(ja) || !cJSON_IsString(jc))
      return server_send_error(conn, "vault: delete requires agent, cred", NULL);

   vault_status_t st =
       vault_service_delete(conn->vault_principal, ja->valuestring, jc->valuestring);
   if (st != VAULT_OK)
      return vault_send_status_error(conn, st);

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return server_send_error(conn, "vault: out of memory", NULL);
   cJSON_AddStringToObject(resp, "status", "ok");
   return server_send_ok(conn, resp);
}

/* POST /v1/vault/lock — evict the cached KEK. */
int handle_vault_lock(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   vault_status_t st = vault_service_lock(conn->vault_principal);
   if (st != VAULT_OK)
      return vault_send_status_error(conn, st);

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return server_send_error(conn, "vault: out of memory", NULL);
   cJSON_AddStringToObject(resp, "status", "ok");
   return server_send_ok(conn, resp);
}
