/* server_cert.c: /v1 route handlers for mTLS client-cert lifecycle (slice 2b).
 * Thin adapters over pki.c: issue / list / revoke client certs. Operator-gated —
 * issuance/revocation are allowed only on an attested operator channel (local
 * UDS, or native-TLS+bearer), never a plaintext TCP bearer or an mTLS client.
 * The route table also gates these at CAP_DELEGATE so a read-only bearer can't
 * even reach them. */
#include "server.h" /* server_conn_t, server_send_*, handle_cert_* decls */
#include "pki.h"
#include "server_http.h"
#include "server_http_identity.h"
#include "vault_principal.h"  /* attested_transport_t, cert:<CN> principal form */
#include "vault_capability.h" /* the grant issued with the cert and revoked with it */
#include "cJSON.h"
#include <openssl/crypto.h> /* OPENSSL_cleanse */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* The vault principal a connection presenting this CN will resolve to, built the
 * same way vault_principal_resolve builds it so the grant and the request agree.
 * Returns 0 when the CN cannot sanitize: no principal will ever be formed for it,
 * so there is nothing to grant and nothing to revoke. */
static int cert_vault_principal(const char *cn, char *out, size_t out_len)
{
   char san[VAULT_CERT_CN_MAX + 1];
   if (out && out_len)
      out[0] = '\0';
   if (!cn || !vault_principal_name_sanitize(cn, san, sizeof(san)) || !out ||
       out_len < VAULT_PRINCIPAL_MAX)
      return 0;
   snprintf(out, out_len, VAULT_CERT_PRINCIPAL_PREFIX "%s", san);
   return 1;
}

/* Find the CN a serial was issued to. Revocation names only the serial, but the
 * vault grant is keyed by cert:<CN>, so the roster is the only bridge between
 * them. */
typedef struct
{
   const char *serial;
   char cn[VAULT_CERT_CN_MAX + 1];
} cert_cn_lookup_t;

static void cert_cn_lookup_cb(void *ctx, const char *serial, const char *cn, long issued_at,
                              long expires_at, int revoked)
{
   cert_cn_lookup_t *lk = (cert_cn_lookup_t *)ctx;
   (void)issued_at;
   (void)expires_at;
   (void)revoked;
   if (!lk || lk->cn[0] || !serial || !cn || strcmp(serial, lk->serial) != 0)
      return;
   snprintf(lk->cn, sizeof(lk->cn), "%s", cn);
}

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
   /* Default 90d. Bound the range before the double->int cast: an out-of-range
    * value (e.g. 1e18) is undefined on cast and would mint an absurd-validity
    * cert, while <1 silently floors to 0. Reject explicitly. */
   int days = 90;
   cJSON *jd = cJSON_GetObjectItemCaseSensitive(req, "days");
   if (jd && cJSON_IsNumber(jd))
   {
      if (jd->valuedouble < 1 || jd->valuedouble > 3650)
         return server_send_error(conn, "cert: 'days' must be between 1 and 3650", NULL);
      days = (int)jd->valuedouble;
   }

   char cert[8192] = "", key[4096] = "", serial[80] = "";
   if (pki_issue(jcn->valuestring, days, cert, sizeof(cert), key, sizeof(key), serial,
                 sizeof(serial)) != 0)
      return server_send_error(
          conn, "cert: issuance failed (invalid/unsanitizable CN, or CA unavailable)", NULL);

   /* Same grant as the CSR path below: this cert is just as much an enrolled
    * client, and leaving it out would mean an operator-issued client still needed
    * a second, manual `vault capability grant` — the exact step this removes.
    * Revoke on failure so a cert never exists without its grant. */
   char vprincipal[VAULT_PRINCIPAL_MAX];
   if (cert_vault_principal(jcn->valuestring, vprincipal, sizeof(vprincipal)) &&
       vault_capability_grant(vprincipal) != 0)
   {
      OPENSSL_cleanse(key, sizeof(key));
      (void)pki_revoke(serial);
      return server_send_error(conn, "cert: could not record the vault grant for this client",
                               NULL);
   }

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

/* POST /v1/cert/sign {cn,csr,days?} -> {cn,serial,cert}. The caller generated
 * and retains the private key; CSR signature verification proves possession. */
int handle_cert_sign(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   if (!cert_op_allowed(conn))
      return server_send_error(conn, "cert: signing requires an attested operator connection",
                               NULL);
   cJSON *jcn = cJSON_GetObjectItemCaseSensitive(req, "cn");
   cJSON *jcsr = cJSON_GetObjectItemCaseSensitive(req, "csr");
   if (!cJSON_IsString(jcn) || !jcn->valuestring[0] || !cJSON_IsString(jcsr) ||
       !jcsr->valuestring[0])
      return server_send_error(conn, "cert: 'cn' and 'csr' are required", NULL);
   int days = 90;
   cJSON *jd = cJSON_GetObjectItemCaseSensitive(req, "days");
   if (jd && cJSON_IsNumber(jd))
   {
      if (jd->valuedouble < 1 || jd->valuedouble > 3650)
         return server_send_error(conn, "cert: 'days' must be between 1 and 3650", NULL);
      days = (int)jd->valuedouble;
   }
   char cert[8192] = "", serial[80] = "";
   if (pki_sign_csr(jcn->valuestring, days, jcsr->valuestring, cert, sizeof(cert), serial,
                    sizeof(serial)) != 0)
      return server_send_error(conn, "cert: CSR signing failed", NULL);

   /* A bearer minted by the setup wizard is an enrollment credential, not a
    * standing write grant.  Activate that grant only now, after possession of
    * the client-generated private key has been proved by the CSR and its serial
    * is durably present in the PKI roster. Ordinary operator-issued certs have
    * no matching enrollment record and retain their existing behavior. */
   int bound = server_http_first_user_bind_cert(server_http_identity_bearer(), serial);
   if (bound < 0)
   {
      (void)pki_revoke(serial);
      return server_send_error(conn,
                               bound == -2 ? "cert: this enrollment bearer is already paired"
                                           : "cert: could not persist the first-user write grant",
                               NULL);
   }
   /* Enrolling IS the authorization. `aimee remote enroll` is the one command a
    * user runs to attach a machine, and it lands here; making the vault grant a
    * second, separate step would mean every operator shelling into the host over
    * UDS before that machine could use a credential at all — and doing it again
    * for the next machine. Mint it here, with the cert, so enrollment is the only
    * thing anyone performs.
    *
    * This is not a widening of who may enroll: cert_op_allowed already restricts
    * signing to an attested operator channel, so the authority to reach this line
    * is strictly greater than the grant it confers.
    *
    * Fail the whole issuance if the grant cannot be recorded, revoking the cert as
    * the bind failure above does. A cert that exists without its grant is exactly
    * the silent half-provisioned state this change is meant to abolish. */
   char vprincipal[VAULT_PRINCIPAL_MAX];
   if (cert_vault_principal(jcn->valuestring, vprincipal, sizeof(vprincipal)) &&
       vault_capability_grant(vprincipal) != 0)
   {
      (void)pki_revoke(serial);
      return server_send_error(conn, "cert: could not record the vault grant for this client",
                               NULL);
   }

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return server_send_error(conn, "cert: out of memory", NULL);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "cn", jcn->valuestring);
   cJSON_AddStringToObject(resp, "serial", serial);
   cJSON_AddStringToObject(resp, "cert", cert);
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
   /* Resolve the CN BEFORE revoking: the grant is keyed by cert:<CN> while
    * revocation names only the serial, and the roster is the sole bridge. */
   cert_cn_lookup_t lk;
   memset(&lk, 0, sizeof(lk));
   lk.serial = js->valuestring;
   (void)pki_list(cert_cn_lookup_cb, &lk);

   if (pki_revoke(js->valuestring) != 0)
      return server_send_error(conn, "cert: revocation failed", NULL);

   /* The cert and its vault grant are issued together, so they must die together.
    * Leaving the grant behind would let a REISSUED cert for the same CN inherit
    * vault authority it was never granted — silently, since nothing in the reissue
    * path would mention it. Idempotent, so a cert issued before this existed (or
    * whose CN never sanitized) revokes cleanly with nothing to remove. */
   char vprincipal[VAULT_PRINCIPAL_MAX];
   if (lk.cn[0] && cert_vault_principal(lk.cn, vprincipal, sizeof(vprincipal)))
      (void)vault_capability_revoke(vprincipal);

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
   if (!arr)
   {
      cJSON_Delete(resp);
      return server_send_error(conn, "cert: out of memory", NULL);
   }
   /* Surface a DB error distinctly from a legitimately empty list — an empty
    * {certs:[]} on prepare/step failure would mask DB corruption. */
   if (pki_list(cert_list_cb, arr) < 0)
   {
      cJSON_Delete(resp);
      return server_send_error(conn, "cert: failed to enumerate certificates", NULL);
   }
   return server_send_ok(conn, resp);
}
