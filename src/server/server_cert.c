/* server_cert.c: /v1 route handlers for mTLS client-cert lifecycle (slice 2b).
 * Thin adapters over pki.c: issue / list / revoke client certs. Operator-gated —
 * issuance/revocation are allowed only on an attested operator channel (local
 * UDS, or native-TLS+bearer), never a plaintext TCP bearer or an mTLS client.
 * The route table also gates these at CAP_DELEGATE so a read-only bearer can't
 * even reach them. */
#include "server.h" /* server_conn_t, server_send_*, handle_cert_* decls */
#include "pki.h"
#include "vault_principal.h" /* attested_transport_t */
#include "cJSON.h"
#include <openssl/crypto.h> /* OPENSSL_cleanse */
#include <stddef.h>

/* Cert issuance/revocation is an operator action: only a kernel-attested local
 * peer (UDS) or a confidential native-TLS+bearer connection may perform it. A
 * plaintext TCP bearer and an mTLS *client* (which must not mint certs) are
 * refused. */
static int cert_op_allowed(const server_conn_t *conn)
{
   return conn && (conn->attested_transport == ATTEST_UDS_PEERCRED ||
                   conn->attested_transport == ATTEST_TLS_BEARER);
}

/* POST /v1/cert/issue {cn, days?} -> {cn, serial, cert, key}. The private key is
 * returned ONCE (server-generated); the channel is attested per cert_op_allowed. */
int handle_cert_issue(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   if (!cert_op_allowed(conn))
      return server_send_error(
          conn, "cert: issuance requires an attested operator connection (local UDS or TLS+bearer)",
          NULL);
   cJSON *jcn = cJSON_GetObjectItemCaseSensitive(req, "cn");
   if (!cJSON_IsString(jcn) || !jcn->valuestring[0])
      return server_send_error(conn, "cert: 'cn' (client name) is required", NULL);
   int days = 90;
   cJSON *jd = cJSON_GetObjectItemCaseSensitive(req, "days");
   if (cJSON_IsNumber(jd) && jd->valuedouble > 0)
      days = (int)jd->valuedouble;

   char cert[8192] = "", key[4096] = "", serial[80] = "";
   if (pki_issue(jcn->valuestring, days, cert, sizeof(cert), key, sizeof(key), serial,
                 sizeof(serial)) != 0)
      return server_send_error(
          conn, "cert: issuance failed (invalid/unsanitizable CN, or CA unavailable)", NULL);

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
   {
      OPENSSL_cleanse(key, sizeof(key));
      return server_send_error(conn, "cert: out of memory", NULL);
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "cn", jcn->valuestring);
   cJSON_AddStringToObject(resp, "serial", serial);
   cJSON_AddStringToObject(resp, "cert", cert);
   cJSON_AddStringToObject(resp, "key", key); /* delivered once; client must store 0600 */
   OPENSSL_cleanse(key, sizeof(key));
   return server_send_ok(conn, resp);
}

/* POST /v1/cert/revoke {serial} -> ok. */
int handle_cert_revoke(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   if (!cert_op_allowed(conn))
      return server_send_error(conn, "cert: revocation requires an attested operator connection",
                               NULL);
   cJSON *js = cJSON_GetObjectItemCaseSensitive(req, "serial");
   if (!cJSON_IsString(js) || !js->valuestring[0])
      return server_send_error(conn, "cert: 'serial' is required", NULL);
   if (pki_revoke(js->valuestring) != 0)
      return server_send_error(conn, "cert: revocation failed", NULL);
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return server_send_error(conn, "cert: out of memory", NULL);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "serial", js->valuestring);
   return server_send_ok(conn, resp);
}

static void cert_list_cb(void *ctx, const char *serial, const char *cn, long issued_at,
                         long expires_at, int revoked)
{
   cJSON *arr = (cJSON *)ctx;
   cJSON *e = cJSON_CreateObject();
   if (!e)
      return;
   cJSON_AddStringToObject(e, "serial", serial ? serial : "");
   cJSON_AddStringToObject(e, "cn", cn ? cn : "");
   cJSON_AddNumberToObject(e, "issued_at", (double)issued_at);
   cJSON_AddNumberToObject(e, "expires_at", (double)expires_at);
   cJSON_AddBoolToObject(e, "revoked", revoked ? 1 : 0);
   cJSON_AddItemToArray(arr, e);
}

/* POST /v1/cert/list -> {certs: [...]}. */
int handle_cert_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   if (!cert_op_allowed(conn))
      return server_send_error(conn, "cert: listing requires an attested operator connection",
                               NULL);
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return server_send_error(conn, "cert: out of memory", NULL);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *arr = cJSON_AddArrayToObject(resp, "certs");
   if (arr)
      pki_list(cert_list_cb, arr);
   return server_send_ok(conn, resp);
}
