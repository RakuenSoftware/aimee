/* server_vault.c: /v1 route handlers for the credential vault (WP-C.1). Thin
 * adapters — parse the request, resolve the attested principal/transport from the
 * connection (WP-C.0, never from the request body), call vault_service, format
 * the response. All policy + crypto lives below in vault_service. */
#include "server.h" /* server_conn_t, server_send_*, handle_vault_* decls */
#include "vault_service.h"
#include "vault_crypto.h"     /* VAULT_ROOT_KEY_LEN */
#include "vault_capability.h" /* vault:write:server gate (D2c) */
#include "log.h"              /* audit_log dedicated 0600 audit sink (D2/D2c) */
#include "cJSON.h"
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <stdio.h>
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
   vault_status_t st;

   /* A webchat-asserted webuser principal unlocks with its login password
    * (scrypt KEK, WP-C.2); a kernel-attested uid: peer unlocks with its 32-byte
    * client root key (hex). The transport — not the request body — selects the
    * path, so a request can't pick the wrong (weaker) unlock for its identity. */
   if (conn->attested_transport == ATTEST_WEBCHAT_TRUSTED)
   {
      cJSON *jpw = cJSON_GetObjectItemCaseSensitive(req, "password");
      if (!cJSON_IsString(jpw))
         return server_send_error(conn, "vault: missing password", NULL);
      st = vault_service_unlock_password(conn->vault_principal, conn->attested_transport,
                                         (const uint8_t *)jpw->valuestring,
                                         strlen(jpw->valuestring), time(NULL));
      /* Best-effort scrub of the request-body copy of the password. */
      OPENSSL_cleanse(jpw->valuestring, strlen(jpw->valuestring));
   }
   else
   {
      cJSON *jrk = cJSON_GetObjectItemCaseSensitive(req, "root_key_hex");
      if (!cJSON_IsString(jrk))
         return server_send_error(conn, "vault: missing root_key_hex", NULL);
      uint8_t root_key[VAULT_ROOT_KEY_LEN];
      if (hex_decode(jrk->valuestring, root_key, sizeof(root_key)) != 0)
         return server_send_error(conn, "vault: root_key_hex must be 64 hex chars", NULL);
      st = vault_service_unlock(conn->vault_principal, conn->attested_transport, root_key,
                                sizeof(root_key), time(NULL));
      OPENSSL_cleanse(root_key, sizeof(root_key));
   }
   if (st != VAULT_OK)
      return vault_send_status_error(conn, st);

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return server_send_error(conn, "vault: out of memory", NULL);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "principal", conn->vault_principal);
   return server_send_ok(conn, resp);
}

/* POST /v1/vault/rekey — re-wrap a webuser vault on a login-password change.
 * Takes {old_password, new_password}; webchat-trusted transport only. */
int handle_vault_rekey(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jold = cJSON_GetObjectItemCaseSensitive(req, "old_password");
   cJSON *jnew = cJSON_GetObjectItemCaseSensitive(req, "new_password");
   if (!cJSON_IsString(jold) || !cJSON_IsString(jnew))
      return server_send_error(conn, "vault: rekey requires old_password, new_password", NULL);

   vault_status_t st = vault_service_rekey_password(
       conn->vault_principal, conn->attested_transport, (const uint8_t *)jold->valuestring,
       strlen(jold->valuestring), (const uint8_t *)jnew->valuestring, strlen(jnew->valuestring),
       time(NULL));
   OPENSSL_cleanse(jold->valuestring, strlen(jold->valuestring));
   OPENSSL_cleanse(jnew->valuestring, strlen(jnew->valuestring));
   if (st != VAULT_OK)
      return vault_send_status_error(conn, st);

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return server_send_error(conn, "vault: out of memory", NULL);
   cJSON_AddStringToObject(resp, "status", "ok");
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

/* A non-secret fingerprint of a credential value for the audit line: the first 4
 * bytes of SHA-256, hex. Never logs the key itself. */
static void vault_cred_fingerprint(const char *secret, char *out, size_t out_len)
{
   unsigned char dig[SHA256_DIGEST_LENGTH];
   SHA256((const unsigned char *)secret, strlen(secret), dig);
   snprintf(out, out_len, "%02x%02x%02x%02x", dig[0], dig[1], dig[2], dig[3]);
}

/* True iff the connection is an attested transport — local UDS, trusted webchat,
 * or native-TLS+bearer — never a plaintext TCP bearer. The D2b precondition for
 * any server-principal write. */
static int vault_conn_is_attested(const server_conn_t *conn)
{
   return conn && (conn->attested_transport == ATTEST_UDS_PEERCRED ||
                   conn->attested_transport == ATTEST_WEBCHAT_TRUSTED ||
                   conn->attested_transport == ATTEST_TLS_BEARER);
}

void vault_audit_server_write(const server_conn_t *conn, const char *agent, const char *cred,
                              const char *secret)
{
   char fp[16];
   vault_cred_fingerprint(secret ? secret : "", fp, sizeof(fp));
   /* Explicit switch so a future attested transport cannot silently fall through
    * to a wrong label; only attested transports reach a server-vault write. */
   const char *transport;
   switch (conn ? conn->attested_transport : ATTEST_NONE)
   {
   case ATTEST_WEBCHAT_TRUSTED:
      transport = "webchat";
      break;
   case ATTEST_TLS_BEARER:
      transport = "tls";
      break;
   case ATTEST_UDS_PEERCRED:
      transport = "uds";
      break;
   default:
      transport = "unknown";
      break;
   }
   /* D2/D2c: server-principal writes go to the dedicated append-only 0600 audit
    * sink (audit_log), NOT the operator-readable general server log — preserving
    * tamper-evidence + access separation. Never logs the key (fingerprint only). */
   audit_log("VAULT_SERVER_WRITE",
             "by=%s transport=%s agent=%s cred=%s fp=%s",
             (conn && conn->vault_principal[0]) ? conn->vault_principal : "(server)", transport,
             agent ? agent : "?", cred ? cred : "?", fp);
}

/* POST /v1/vault/set_server — store a CLIENT-SUPPLIED credential under the
 * server-owned principal (autonomous decrypt). Gated (D2b/D2c): an attested
 * transport AND the vault:write:server capability for the caller's principal.
 * Audited with a key fingerprint (never the key). Distinct from /vault/set, which
 * writes the caller's OWN per-user vault. */
int handle_vault_set_server(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *ja = cJSON_GetObjectItemCaseSensitive(req, "agent");
   cJSON *jc = cJSON_GetObjectItemCaseSensitive(req, "cred");
   cJSON *js = cJSON_GetObjectItemCaseSensitive(req, "secret");
   if (!cJSON_IsString(ja) || !cJSON_IsString(jc) || !cJSON_IsString(js))
      return server_send_error(conn, "vault: set_server requires agent, cred, secret", NULL);

   if (!vault_capability_server_write_allowed(conn->attested_transport, conn->vault_principal))
   {
      if (!vault_conn_is_attested(conn))
         return server_send_error(
             conn, "vault: server-principal write requires an attested (UDS/webchat) connection",
             NULL);
      /* Name the caller's resolved principal so the operator grants the RIGHT one
       * — a UDS peer is `uid:<N>`, not the unix username, which is a common
       * footgun (granting the username silently doesn't match). */
      char msg[256];
      const char *who = (conn && conn->vault_principal[0]) ? conn->vault_principal : "(unknown)";
      snprintf(msg, sizeof msg,
               "vault: caller (principal %s) lacks the vault:write:server capability "
               "(grant it over UDS with `aimee vault capability grant %s`)",
               who, who);
      return server_send_error(conn, msg, NULL);
   }

   vault_status_t st = vault_service_set_server(ja->valuestring, jc->valuestring, js->valuestring);
   if (st != VAULT_OK)
      return vault_send_status_error(conn, st);

   vault_audit_server_write(conn, ja->valuestring, jc->valuestring, js->valuestring);

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return server_send_error(conn, "vault: out of memory", NULL);
   cJSON_AddStringToObject(resp, "status", "ok");
   return server_send_ok(conn, resp);
}

/* POST /v1/vault/capability — manage the vault:write:server allow-list. UDS-only
 * (a kernel-attested operator). {action: "grant"|"revoke"|"list", principal?}. */
int handle_vault_capability(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   if (!conn || conn->attested_transport != ATTEST_UDS_PEERCRED)
      return server_send_error(conn, "vault: capability management is UDS-only", NULL);

   cJSON *jaction = cJSON_GetObjectItemCaseSensitive(req, "action");
   if (!cJSON_IsString(jaction))
      return server_send_error(conn, "vault: capability requires action (grant|revoke|list)", NULL);
   const char *action = jaction->valuestring;

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return server_send_error(conn, "vault: out of memory", NULL);

   if (strcmp(action, "list") == 0)
   {
      char buf[4096] = "";
      (void)vault_capability_list(buf, sizeof(buf));
      cJSON_AddStringToObject(resp, "status", "ok");
      cJSON_AddStringToObject(resp, "principals", buf);
      return server_send_ok(conn, resp);
   }

   cJSON *jp = cJSON_GetObjectItemCaseSensitive(req, "principal");
   if (!cJSON_IsString(jp) || !jp->valuestring[0])
   {
      cJSON_Delete(resp);
      return server_send_error(conn, "vault: capability grant/revoke requires principal", NULL);
   }
   int rc;
   if (strcmp(action, "grant") == 0)
      rc = vault_capability_grant(jp->valuestring);
   else if (strcmp(action, "revoke") == 0)
      rc = vault_capability_revoke(jp->valuestring);
   else
   {
      cJSON_Delete(resp);
      return server_send_error(conn, "vault: unknown capability action (grant|revoke|list)", NULL);
   }
   if (rc != 0)
   {
      cJSON_Delete(resp);
      return server_send_error(conn, "vault: capability update failed", NULL);
   }
   /* D2/D2c: capability grant/revoke is an authz change to the server-write
    * allow-list — record it in the dedicated append-only 0600 audit sink. */
   audit_log("VAULT_CAPABILITY", "action=%s principal=%s by=%s", action, jp->valuestring,
             conn->vault_principal);
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
