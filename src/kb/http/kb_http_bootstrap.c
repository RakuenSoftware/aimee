/* kb_http_bootstrap.c — see kb_http_bootstrap.h.
 *
 * The identity-login routes live in their own unit (kb_http_identity_login.c)
 * because they carry real logic and their own test; this file is the pre-auth
 * dispatcher plus the enrollment redeem handler, moved verbatim from kb_http.c. */

#include "kb_http_bootstrap.h"

#include "cJSON.h"
#include "modules/db2/c/enrollments.h"
#include "kb_enroll.h"
#include "kb_http_identity_login.h"
#include "kb_identity.h"
#include "kb_paths.h"
#include "kb_pki.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define KB_ENROLL_CERT_VALID_SECS (60L * 60 * 24 * 90)

static int enrollment_expiry(char out[32])
{
   time_t now = time(NULL);
   time_t expires = now + KB_ENROLL_CERT_VALID_SECS;
   struct tm utc;
   if (now == (time_t)-1 || expires < now || !gmtime_r(&expires, &utc) ||
       strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &utc) == 0)
      return -1;
   return 0;
}

static int enroll_redeem_route(const char *method, const char *path, const char *body,
                               char *out_buf, int out_cap)
{
   /* The single-use enrollment token is the credential, so redeem precedes bearer
    * auth. The caller supplies a CSR; its private key never leaves the client. */
   if (strcmp(path, "/v1/enroll/redeem") == 0)
   {
      if (strcmp(method, "POST") != 0)
      {
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
         return 405;
      }
      cJSON *req = body ? cJSON_Parse(body) : NULL;
      const cJSON *jtok = req ? cJSON_GetObjectItemCaseSensitive(req, "token") : NULL;
      const cJSON *jcsr = req ? cJSON_GetObjectItemCaseSensitive(req, "csr") : NULL;
      if (!cJSON_IsString(jtok) || !cJSON_IsString(jcsr))
      {
         cJSON_Delete(req);
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"bad request: token (string) and csr (PEM string) required\"}");
         return 400;
      }
      char scope[KB_ENROLL_SCOPE_MAX];
      char *cert = malloc(KB_PKI_CERT_PEM_MAX);
      int rc = cert ? kb_enroll_redeem_csr(kb_default_config_dir(), jtok->valuestring,
                                           jcsr->valuestring, KB_ENROLL_CERT_VALID_SECS, scope,
                                           sizeof(scope), cert, KB_PKI_CERT_PEM_MAX)
                    : -1;
      cJSON_Delete(req);
      if (rc != 0)
      {
         free(cert);
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"enrollment failed: invalid or used token, or bad CSR\"}");
         return 401;
      }
      /* Persist the issuer/serial and mTLS certificate-DER SHA-256. Fail closed
       * rather than release an authority the enrollment database cannot revoke. */
      char fp[KB_PKI_FP_HEX] = "", issuer[KB_PKI_ISSUER_MAX + 1] = "";
      char raw_serial[KB_PKI_SERIAL_MAX + 1] = "";
      char serial[KB_PKI_SERIAL_MAX + 1] = "";
      char expires_at[32] = "";
      if (kb_pki_ca_fingerprint(cert, fp, sizeof(fp)) != 0 ||
          kb_pki_cert_metadata(cert, issuer, sizeof(issuer), raw_serial, sizeof(raw_serial)) != 0 ||
          kb_cert_serial_normalize(raw_serial, serial, sizeof(serial)) != 0 ||
          enrollment_expiry(expires_at) != 0 ||
          db2_enrollment_insert(scope, fp, issuer, serial, expires_at, 0, NULL) != 0)
      {
         free(cert);
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"enrollment persistence unavailable\"}");
         return 503;
      }
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "client_cert", cert);
      cJSON_AddStringToObject(resp, "scope", scope);
      char *out = cJSON_PrintUnformatted(resp);
      snprintf(out_buf, (size_t)out_cap, "%s", out ? out : "{}");
      free(out);
      cJSON_Delete(resp);
      free(cert);
      return 200;
   }
   return -1;
}

int kb_http_bootstrap_route(const char *method, const char *path, const char *query_string,
                            const char *body, int64_t now, char *out_buf, int out_cap)
{
   if (!method || !path || !out_buf || out_cap <= 0)
      return -1;
   int ir = kb_http_identity_login_route(method, path, query_string, body, now, out_buf, out_cap);
   if (ir >= 0)
      return ir;
   return enroll_redeem_route(method, path, body, out_buf, out_cap);
}
