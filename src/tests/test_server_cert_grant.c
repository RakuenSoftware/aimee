/* test_server_cert_grant.c: the vault-grant lifecycle that rides on the client
 * certificate, driven through the cert handlers themselves.
 *
 * WHY THIS FILE EXISTS. Nothing in the suite compiled server_cert.o at all, so
 * the enrollment grant and its revocation rested entirely on an end-to-end run
 * against a live server with a real CA. CI passed without touching a line of it.
 * That is precisely the coupling most worth pinning: a cert and its vault grant
 * are issued together and must die together, and the failure mode when they do
 * not is silent — a REISSUED cert for the same CN inherits vault authority it
 * was never granted, and nothing in the reissue path mentions it.
 *
 * PKI is stubbed rather than linked. A real CA would make this test about
 * OpenSSL, which is not where the risk is; the risk is in which handler grants,
 * which revokes, and what happens when one half fails. vault_capability is the
 * REAL implementation over a temp file, so the grant assertions observe actual
 * recorded state rather than a mock's memory of being called.
 */
#include "cJSON.h"
#include "pki.h"
#include "server.h"
#include "vault_capability.h"
#include "vault_principal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* --- captured handler output ------------------------------------------- */

static char g_message[1024];
static int g_was_error;

/* server_send_ok is a static inline over server_send_response, so this one stub
 * captures both outcomes. */
int server_send_response(server_conn_t *conn, cJSON *response)
{
   (void)conn;
   g_was_error = 0;
   g_message[0] = '\0';
   cJSON *err = response ? cJSON_GetObjectItemCaseSensitive(response, "error") : NULL;
   if (cJSON_IsString(err))
   {
      g_was_error = 1;
      snprintf(g_message, sizeof(g_message), "%s", err->valuestring);
   }
   return 0;
}

int server_send_error(server_conn_t *conn, const char *message, const char *request_id)
{
   (void)conn;
   (void)request_id;
   g_was_error = 1;
   snprintf(g_message, sizeof(g_message), "%s", message ? message : "");
   return 0;
}

/* --- PKI stub ------------------------------------------------------------ */

/* One issued cert is enough to bridge serial -> CN, which is the only thing the
 * revoke path needs the roster for. */
static char g_issued_serial[80];
static char g_issued_cn[256];
static int g_issue_fails;
static int g_revoked_count;
static char g_last_revoked[80];
static int g_issued_days;

int pki_issue(const char *cn, int validity_days, char *cert_pem, size_t cert_len, char *key_pem,
              size_t key_len, char *serial_out, size_t serial_len)
{
   if (g_issue_fails)
      return -1;
   g_issued_days = validity_days;
   snprintf(g_issued_cn, sizeof(g_issued_cn), "%s", cn ? cn : "");
   snprintf(g_issued_serial, sizeof(g_issued_serial), "SERIAL-1");
   snprintf(cert_pem, cert_len, "-----BEGIN CERTIFICATE-----\nstub\n-----END CERTIFICATE-----\n");
   snprintf(key_pem, key_len, "-----BEGIN PRIVATE KEY-----\nstub\n-----END PRIVATE KEY-----\n");
   snprintf(serial_out, serial_len, "%s", g_issued_serial);
   return 0;
}

int pki_sign_csr(const char *cn, int validity_days, const char *csr_pem, char *cert_pem,
                 size_t cert_len, char *serial_out, size_t serial_len)
{
   (void)csr_pem;
   if (g_issue_fails)
      return -1;
   g_issued_days = validity_days;
   snprintf(g_issued_cn, sizeof(g_issued_cn), "%s", cn ? cn : "");
   snprintf(g_issued_serial, sizeof(g_issued_serial), "SERIAL-1");
   snprintf(cert_pem, cert_len, "-----BEGIN CERTIFICATE-----\nstub\n-----END CERTIFICATE-----\n");
   snprintf(serial_out, serial_len, "%s", g_issued_serial);
   return 0;
}

int pki_revoke(const char *serial)
{
   g_revoked_count++;
   snprintf(g_last_revoked, sizeof(g_last_revoked), "%s", serial ? serial : "");
   return 0;
}

int pki_list(void (*cb)(void *ctx, const char *serial, const char *cn, long issued_at,
                        long expires_at, int revoked),
             void *ctx)
{
   if (cb && g_issued_serial[0])
      cb(ctx, g_issued_serial, g_issued_cn, 1000, 2000, 0);
   return 0;
}

/* --- helpers ------------------------------------------------------------- */

static server_conn_t conn_on(attested_transport_t t)
{
   server_conn_t c;
   memset(&c, 0, sizeof(c));
   c.attested_transport = t;
   return c;
}

static int issue(attested_transport_t t, const char *cn, cJSON *extra)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "cn", cn);
   if (extra)
   {
      cJSON *d = cJSON_GetObjectItemCaseSensitive(extra, "days");
      if (d)
         cJSON_AddNumberToObject(req, "days", d->valuedouble);
   }
   server_conn_t c = conn_on(t);
   g_was_error = 0;
   g_message[0] = '\0';
   handle_cert_issue(NULL, &c, req);
   cJSON_Delete(req);
   return g_was_error;
}

static int revoke_cert(attested_transport_t t, const char *serial)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "serial", serial);
   server_conn_t c = conn_on(t);
   g_was_error = 0;
   g_message[0] = '\0';
   handle_cert_revoke(NULL, &c, req);
   cJSON_Delete(req);
   return g_was_error;
}

static void principal_for(const char *cn, char *out, size_t cap)
{
   char san[VAULT_CERT_CN_MAX + 1];
   assert(vault_principal_name_sanitize(cn, san, sizeof(san)));
   snprintf(out, cap, VAULT_CERT_PRINCIPAL_PREFIX "%s", san);
}

static void reset_pki(void)
{
   g_issue_fails = 0;
   g_revoked_count = 0;
   g_issued_days = 0;
   g_issued_serial[0] = '\0';
   g_issued_cn[0] = '\0';
   g_last_revoked[0] = '\0';
}

/* --- tests --------------------------------------------------------------- */

/* Minting and revoking client certs is an operator action. An mTLS *client* must
 * not be able to mint further certs (privilege escalation by self-issue), and a
 * plaintext TCP bearer must never be trusted with it at all. */
static void test_transport_gate(void)
{
   reset_pki();
   const attested_transport_t allowed[] = {ATTEST_UDS_PEERCRED, ATTEST_TLS_BEARER};
   for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
   {
      assert(issue(allowed[i], "operator-ok", NULL) == 0);
   }

   const attested_transport_t refused[] = {ATTEST_TCP_BEARER, ATTEST_MTLS_CLIENT,
                                           ATTEST_WEBCHAT_TRUSTED};
   for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
   {
      assert(issue(refused[i], "not-an-operator", NULL) == 1);
      assert(strstr(g_message, "attested operator connection"));
      /* Refused before reaching the CA at all -- a rejected caller must not have
       * caused a cert to exist, revoked or otherwise. */
      assert(g_issued_serial[0] == '\0' || strcmp(g_issued_cn, "not-an-operator") != 0);
   }

   assert(revoke_cert(ATTEST_TCP_BEARER, "SERIAL-1") == 1);
   assert(strstr(g_message, "attested operator connection"));
   printf("  test_transport_gate ok\n");
}

/* The grant is what makes an enrolled client able to reach the shared vault at
 * all. Issuing without it means every new cert needs a second manual step. */
static void test_issue_grants_the_vault_capability(void)
{
   reset_pki();
   char principal[VAULT_PRINCIPAL_MAX];
   principal_for("enrolled-client", principal, sizeof(principal));

   assert(vault_capability_has(principal) == 0);
   assert(issue(ATTEST_UDS_PEERCRED, "enrolled-client", NULL) == 0);
   assert(vault_capability_has(principal) == 1);
   printf("  test_issue_grants_the_vault_capability ok\n");
}

/* A cert must never outlive the grant it was issued with: if the grant cannot be
 * recorded, the cert is revoked and the call fails. Otherwise a client would
 * hold a valid certificate the server believes carries authority it never
 * recorded. */
static void test_grant_failure_revokes_the_cert(void)
{
   reset_pki();
   /* Point the capability store somewhere unwritable so the grant fails while
    * issuance succeeds -- the exact split this path exists to handle. */
   vault_capability_set_path_for_test("/proc/definitely-not-writable/caps");

   assert(issue(ATTEST_UDS_PEERCRED, "doomed-client", NULL) == 1);
   assert(strstr(g_message, "vault grant"));
   /* The cert that was minted a moment earlier must have been revoked. */
   assert(g_revoked_count == 1);
   assert(strcmp(g_last_revoked, "SERIAL-1") == 0);
   printf("  test_grant_failure_revokes_the_cert ok\n");
}

/* Revocation names only a serial; the grant is keyed by cert:<CN>. If the roster
 * bridge breaks, the grant survives its certificate and a reissued cert for the
 * same CN silently inherits authority. */
static void test_revoke_removes_the_grant(void)
{
   reset_pki();
   char principal[VAULT_PRINCIPAL_MAX];
   principal_for("retiring-client", principal, sizeof(principal));

   assert(issue(ATTEST_UDS_PEERCRED, "retiring-client", NULL) == 0);
   assert(vault_capability_has(principal) == 1);

   assert(revoke_cert(ATTEST_UDS_PEERCRED, "SERIAL-1") == 0);
   assert(vault_capability_has(principal) == 0);
   printf("  test_revoke_removes_the_grant ok\n");
}

/* days is cast from a double. An out-of-range value is undefined on cast and
 * would mint an absurd-validity cert; <1 silently floors to 0. */
static void test_days_bounds(void)
{
   reset_pki();
   cJSON *extra = cJSON_CreateObject();

   cJSON_AddNumberToObject(extra, "days", 1e18);
   assert(issue(ATTEST_UDS_PEERCRED, "range-client", extra) == 1);
   assert(strstr(g_message, "between 1 and 3650"));
   cJSON_Delete(extra);

   extra = cJSON_CreateObject();
   cJSON_AddNumberToObject(extra, "days", 0);
   assert(issue(ATTEST_UDS_PEERCRED, "range-client", extra) == 1);
   cJSON_Delete(extra);

   extra = cJSON_CreateObject();
   cJSON_AddNumberToObject(extra, "days", 3650);
   assert(issue(ATTEST_UDS_PEERCRED, "range-client", extra) == 0);
   assert(g_issued_days == 3650);
   cJSON_Delete(extra);

   /* Default when unspecified. */
   assert(issue(ATTEST_UDS_PEERCRED, "range-client", NULL) == 0);
   assert(g_issued_days == 90);
   printf("  test_days_bounds ok\n");
}

static void test_cn_required(void)
{
   reset_pki();
   cJSON *req = cJSON_CreateObject();
   server_conn_t c = conn_on(ATTEST_UDS_PEERCRED);
   g_was_error = 0;
   handle_cert_issue(NULL, &c, req);
   assert(g_was_error == 1);
   assert(strstr(g_message, "'cn'"));
   cJSON_Delete(req);
   printf("  test_cn_required ok\n");
}

int main(void)
{
   char tmpl[256];
   snprintf(tmpl, sizeof tmpl, "%s/aimee-cert-grant-XXXXXX", platform_tmpdir());
   char *dir = mkdtemp(tmpl);
   assert(dir);
   char caps[512];
   snprintf(caps, sizeof(caps), "%s/capabilities", dir);
   vault_capability_set_path_for_test(caps);

   test_transport_gate();
   test_issue_grants_the_vault_capability();
   test_revoke_removes_the_grant();
   test_days_bounds();
   test_cn_required();
   /* Last: it deliberately repoints the capability store at an unwritable path. */
   test_grant_failure_revokes_the_cert();

   unlink(caps);
   rmdir(dir);
   printf("PASS: server_cert_grant (transport gate, grant on issue, revoke on revoke, "
          "revoke-on-grant-failure, days bounds)\n");
   return 0;
}
