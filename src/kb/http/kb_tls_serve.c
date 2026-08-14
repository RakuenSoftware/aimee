/* kb_tls_serve.c: aimee-kb's mTLS request serving + listener (distributed mode).
 * (distributed-mode-auth proposal, mTLS phase.)
 *
 * Split out of kb_tls.c so the KB-only serving path (which routes through
 * kb_http_route_ex) does not pull the kb request router into the aimee-server
 * binary, which links only the client-side TLS primitives in kb_tls.c. */
/* _GNU_SOURCE: strcasestr is a GNU extension; declare it before any include
 * pulls in <string.h>. The Makefile build does not define it globally (only
 * the CMake build does), so older glibc/gcc targets (e.g. Debian 12) need it
 * here or strcasestr is an implicit-declaration error under -Werror. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "kb_tls.h"

/* --- serve one mTLS connection (handshake + request + scoped routing) --- */

#include "cJSON.h"
#include "kb_enroll.h" /* KB_ENROLL_SCOPE_MAX */
#include "kb_http.h"   /* kb_http_route_ex */
#include "kb_http_egress.h"
#include "../../db2/server_registry.h"
#include "../../db2/management_jwks_runtime.h"
#include "../../db2/db2_tenant.h"
#include "db2/db2.h"    /* request-scoped DB2 lease */
#include "kb_ingress.h" /* B5 identity-header ingress guard */
#include "kb_auth_oidc.h"
#include "kb_identity.h"
#include "kb_reqctx.h"
#include "kb_verifier.h"
#include "pam_auth.h"
#include "config.h"
#include "log.h"             /* LOG_WARN */
#include "db2/enrollments.h" /* revocation source of truth + last-seen */
#include "kb_paths.h"        /* kb_default_config_dir */
#include "kb_pki.h"          /* CA load + CSR signing for renew */
#include "runtime_secret.h"
#include "util.h"
#include <aimee/core/connection/auth.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

#define KB_TLS_HEADER_MAX       (64 * 1024)
#define KB_TLS_BODY_MAX         (1024 * 1024)
#define KB_TLS_REQ_MAX          (KB_TLS_HEADER_MAX + KB_TLS_BODY_MAX + 1)
#define KB_TLS_REQUEST_LINE_MAX 8192
#define KB_TLS_URI_MAX          4096
#define KB_TLS_HEADER_COUNT_MAX 64
#define KB_TLS_RESP_MAX         262144
#define KB_TLS_AUTH_MAX         8192
#define KB_TLS_BEARER_MAX       4096
#define KB_TLS_CALLER_MAX       576

static int (*g_pam_check_override)(const char *, const char *);

void kb_tls_set_pam_check_for_test(int (*check)(const char *, const char *))
{
   g_pam_check_override = check;
}

static int header_name_char(unsigned char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int header_value_has_token(const char *start, const char *end, const char *token)
{
   size_t token_len = strlen(token);
   while (start < end)
   {
      while (start < end && (*start == ' ' || *start == '\t' || *start == ','))
         start++;
      const char *item_end = start;
      while (item_end < end && *item_end != ',')
         item_end++;
      const char *trimmed_end = item_end;
      while (trimmed_end > start && (trimmed_end[-1] == ' ' || trimmed_end[-1] == '\t'))
         trimmed_end--;
      if ((size_t)(trimmed_end - start) == token_len && !strncasecmp(start, token, token_len))
         return 1;
      start = item_end < end ? item_end + 1 : end;
   }
   return 0;
}

static int positive_int64_header(const char *start, const char *end, int64_t *out)
{
   if (!start || !end || start >= end || !out || (size_t)(end - start) > 19)
      return 0;
   char raw[20];
   size_t n = (size_t)(end - start);
   memcpy(raw, start, n);
   raw[n] = '\0';
   char *tail = NULL;
   errno = 0;
   long long value = strtoll(raw, &tail, 10);
   if (errno || !tail || *tail || value <= 0)
      return 0;
   *out = (int64_t)value;
   return 1;
}

/* All three connection layers must name the same enrolled identity. The
 * certificate carries "<kind>:<id>" in its verified CN; the bearer verifier
 * derives the corresponding scope exclusively from the verified credential. */
static int bearer_identity_matches_certificate(const char *cn, const kb_verify_result_t *identity)
{
   if (!cn || !identity || !identity->scope_kind[0] || !identity->scope_id[0])
      return 0;
   char scope[sizeof(identity->scope_kind) + sizeof(identity->scope_id) + 2];
   int n = snprintf(scope, sizeof(scope), "%s:%s", identity->scope_kind, identity->scope_id);
   return n > 0 && (size_t)n < sizeof(scope) && strcmp(scope, cn) == 0;
}

/* Decode one canonical HTTP Basic value and authenticate it through the same
 * PAM service used by the KB login/dashboard. The decoded password never
 * escapes this stack frame and every copy is wiped before return. */
static int pam_service_identity(const char *authorization, kb_principal_t *out)
{
   unsigned char decoded[1100] = {0};
   char canonical[1600] = "";
   int ok = 0;
   if (!authorization || strncasecmp(authorization, "Basic ", 6) != 0)
      goto done;
   const char *encoded = authorization + 6;
   size_t encoded_len = strlen(encoded);
   if (!encoded_len || encoded_len % 4 != 0 || encoded_len >= sizeof(canonical))
      goto done;
   for (size_t i = 0; i < encoded_len; ++i)
      if (!((encoded[i] >= 'A' && encoded[i] <= 'Z') || (encoded[i] >= 'a' && encoded[i] <= 'z') ||
            (encoded[i] >= '0' && encoded[i] <= '9') || encoded[i] == '+' || encoded[i] == '/' ||
            (encoded[i] == '=' && i >= encoded_len - 2)))
         goto done;
   size_t decoded_len = aimee_base64_decode(encoded, decoded, sizeof(decoded) - 1);
   if (decoded_len == (size_t)-1 || !decoded_len || memchr(decoded, '\0', decoded_len))
      goto done;
   decoded[decoded_len] = '\0';
   if (aimee_base64_encode(decoded, decoded_len, canonical, sizeof(canonical)) == 0 ||
       !aimee_core_credential_equal(encoded, canonical))
      goto done;
   char *colon = strchr((char *)decoded, ':');
   if (!colon || colon == (char *)decoded || !colon[1])
      goto done;
   *colon = '\0';
   int authenticated = g_pam_check_override ? g_pam_check_override((char *)decoded, colon + 1)
                                            : pam_check_credentials((char *)decoded, colon + 1);
   if (authenticated && kb_principal_from_host_account((char *)decoded, out) == 0)
      ok = 1;
done:
   OPENSSL_cleanse(canonical, sizeof(canonical));
   OPENSSL_cleanse(decoded, sizeof(decoded));
   return ok;
}

/* Returns 1 for a verified third-layer service identity, 0 for bad/missing
 * credentials, and -1 when an OIDC policy was requested but cannot safely be
 * enforced. OIDC never falls back to PAM. */
static int service_identity_authenticate(const char *authorization, kb_principal_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   int mode = kb_oidc_service_mode();
   if (mode < 0)
      return -1;
   if (mode == 0)
      return pam_service_identity(authorization, out);

   const char *jwt = aimee_core_bearer_token(authorization);
   kb_verify_result_t verified;
   memset(&verified, 0, sizeof(verified));
   char issuer[256] = "";
   int ok = jwt && kb_oidc_verify_service_token(jwt, (long)time(NULL), &verified) &&
            kb_oidc_configured_issuer(issuer, sizeof(issuer)) == 0 && issuer[0] &&
            kb_principal_from_verify(&verified, issuer, out) == 0;
   OPENSSL_cleanse(&verified, sizeof(verified));
   return ok ? 1 : 0;
}

/* All application identities on this listener are service identities and are
 * bound to the independently enrolled mTLS role. For service:<name>, both PAM
 * and OIDC must authenticate exactly <name>; an unrelated valid account is not
 * enough to complete the third layer. */
static int application_identity_matches_certificate(const char *cn, const kb_principal_t *identity)
{
   static const char prefix[] = "service:";
   if (!cn || !identity || !identity->authenticated ||
       strncmp(cn, prefix, sizeof(prefix) - 1) != 0 || !cn[sizeof(prefix) - 1])
      return 0;
   if (identity->kind != KB_PRIN_HOST && identity->kind != KB_PRIN_OIDC)
      return 0;
   return strcmp(cn + sizeof(prefix) - 1, identity->subject) == 0;
}

static void set_recv_timeout(SSL *ssl, int seconds)
{
   int fd = SSL_get_fd(ssl);
   struct timeval timeout = {.tv_sec = seconds, .tv_usec = 0};
   if (fd >= 0)
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

/* Read exactly one strict HTTP/1.1 request. Returns an HTTP error status, 0 on
 * success, or -1 when an idle/cleanly closed peer supplied no request bytes. */
static int strict_request_read(SSL *ssl, char *buf, size_t cap, int *total_out, int *header_out,
                               size_t *body_out, int *close_out, char *authorization_out,
                               size_t authorization_cap, char *service_authorization_out,
                               size_t service_authorization_cap, char *caller_subject_out,
                               size_t caller_subject_cap, int64_t *named_team_out)
{
   size_t total = 0, header_len = 0, content_len = 0;
   int have_cl = 0, have_authorization = 0, have_service_authorization = 0, have_caller_subject = 0,
       have_named_team = 0;
   if (!authorization_out || authorization_cap == 0 || !service_authorization_out ||
       service_authorization_cap == 0 || !caller_subject_out || caller_subject_cap == 0 ||
       !named_team_out)
      return 400;
   authorization_out[0] = '\0';
   service_authorization_out[0] = '\0';
   caller_subject_out[0] = '\0';
   *named_team_out = 0;
   while (total + 1 < cap && total < KB_TLS_HEADER_MAX)
   {
      int n = SSL_read(ssl, buf + total, (int)(KB_TLS_HEADER_MAX - total));
      if (n <= 0)
         return total == 0 ? -1 : 400;
      total += (size_t)n;
      set_recv_timeout(ssl, 10); /* first byte arrived: bound the remaining head */
      buf[total] = '\0';
      char *end = strstr(buf, "\r\n\r\n");
      if (end)
      {
         header_len = (size_t)(end + 4 - buf);
         break;
      }
   }
   if (!header_len || header_len > KB_TLS_HEADER_MAX)
      return 413;
   for (size_t i = 0; i < header_len; i++)
      if (buf[i] == '\0' || (buf[i] == '\r' && (i + 1 >= header_len || buf[i + 1] != '\n')) ||
          (buf[i] == '\n' && (i == 0 || buf[i - 1] != '\r')))
         return 400;
   char *line_end = strstr(buf, "\r\n");
   if (!line_end || (size_t)(line_end - buf) > KB_TLS_REQUEST_LINE_MAX)
      return 400;
   char method[16], target[KB_TLS_URI_MAX + 1], version[16], extra;
   *line_end = '\0';
   int fields = sscanf(buf, "%15s %4096s %15s %c", method, target, version, &extra);
   *line_end = '\r';
   if (fields != 3 || strcmp(version, "HTTP/1.1") || target[0] != '/' || strstr(target, "://") ||
       strchr(target, '#'))
      return 400;
   char *p = line_end + 2;
   char *headers_end = buf + header_len - 2;
   int header_count = 0;
   /* Start every request with no content type, so a request that sends none
    * cannot inherit the previous one's on a reused connection. */
   kb_reqctx_set_content_type("");
   while (p < headers_end && !(p[0] == '\r' && p[1] == '\n'))
   {
      if (++header_count > KB_TLS_HEADER_COUNT_MAX)
         return 400;
      char *e = strstr(p, "\r\n");
      if (!e || e > headers_end || p[0] == ' ' || p[0] == '\t')
         return 400;
      char *colon = memchr(p, ':', (size_t)(e - p));
      if (!colon || colon == p || colon[-1] == ' ' || colon[-1] == '\t')
         return 400;
      for (char *q = p; q < colon; q++)
         if (!header_name_char((unsigned char)*q))
            return 400;
      for (char *q = colon + 1; q < e; q++)
         if (((unsigned char)*q < 0x20 && *q != '\t') || (unsigned char)*q == 0x7f)
            return 400;
      size_t name_len = (size_t)(colon - p);
      if (name_len == 17 && !strncasecmp(p, "Transfer-Encoding", 17))
         return 400;
      if (name_len == 14 && !strncasecmp(p, "Content-Length", 14))
      {
         if (have_cl)
            return 400;
         have_cl = 1;
         char *v = colon + 1;
         while (v < e && (*v == ' ' || *v == '\t'))
            v++;
         char *ve = e;
         while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
            ve--;
         if (v == ve || (ve - v > 1 && *v == '0'))
            return 400;
         size_t value = 0;
         for (char *q = v; q < ve; q++)
         {
            if (*q < '0' || *q > '9' || value > (cap - 1) / 10)
               return 400;
            value = value * 10 + (size_t)(*q - '0');
         }
         content_len = value;
      }
      if (name_len == 12 && !strncasecmp(p, "Content-Type", 12))
      {
         /* Kept for the routes whose security depends on it (see kb_reqctx.h). */
         char *v = colon + 1;
         while (v < e && (*v == ' ' || *v == '\t'))
            v++;
         char ct[128];
         size_t vlen = (size_t)(e - v);
         if (vlen >= sizeof(ct))
            vlen = sizeof(ct) - 1;
         memcpy(ct, v, vlen);
         ct[vlen] = '\0';
         kb_reqctx_set_content_type(ct);
      }
      if (name_len == 13 && !strncasecmp(p, "Authorization", 13))
      {
         if (have_authorization)
            return 400;
         have_authorization = 1;
         char *v = colon + 1;
         while (v < e && (*v == ' ' || *v == '\t'))
            v++;
         char *ve = e;
         while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
            ve--;
         size_t vlen = (size_t)(ve - v);
         if (!vlen || vlen >= authorization_cap)
            return 400;
         memcpy(authorization_out, v, vlen);
         authorization_out[vlen] = '\0';
      }
      if (name_len == sizeof("X-Aimee-Service-Authorization") - 1 &&
          !strncasecmp(p, "X-Aimee-Service-Authorization", name_len))
      {
         if (have_service_authorization)
            return 400;
         have_service_authorization = 1;
         char *v = colon + 1;
         while (v < e && (*v == ' ' || *v == '\t'))
            v++;
         char *ve = e;
         while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
            ve--;
         size_t vlen = (size_t)(ve - v);
         if (!vlen || vlen >= service_authorization_cap)
            return 400;
         memcpy(service_authorization_out, v, vlen);
         service_authorization_out[vlen] = '\0';
      }
      if (name_len == sizeof("X-Aimee-Caller-Subject") - 1 &&
          !strncasecmp(p, "X-Aimee-Caller-Subject", name_len))
      {
         if (have_caller_subject)
            return 400;
         have_caller_subject = 1;
         char *v = colon + 1;
         while (v < e && (*v == ' ' || *v == '\t'))
            v++;
         char *ve = e;
         while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
            ve--;
         size_t vlen = (size_t)(ve - v);
         if (!vlen || vlen >= caller_subject_cap)
            return 400;
         memcpy(caller_subject_out, v, vlen);
         caller_subject_out[vlen] = '\0';
      }
      if (name_len == sizeof("X-Aimee-Team-ID") - 1 &&
          !strncasecmp(p, "X-Aimee-Team-ID", name_len))
      {
         if (have_named_team)
            return 400;
         have_named_team = 1;
         char *v = colon + 1;
         while (v < e && (*v == ' ' || *v == '\t'))
            v++;
         char *ve = e;
         while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
            ve--;
         if (!positive_int64_header(v, ve, named_team_out))
            return 400;
      }
      if (name_len == 10 && !strncasecmp(p, "Connection", 10) &&
          header_value_has_token(colon + 1, e, "close"))
         *close_out = 1;
      p = e + 2;
   }
   int bodyless = !strcmp(method, "GET") || !strcmp(method, "HEAD");
   if ((bodyless && have_cl) || (!bodyless && !have_cl))
      return 400;
   if (content_len > KB_TLS_BODY_MAX || content_len > cap - header_len - 1)
      return 413;
   if (total > header_len + content_len)
      return 400;
   set_recv_timeout(ssl, 30); /* body transfer uses the normal I/O deadline */
   while (total < header_len + content_len)
   {
      int n = SSL_read(ssl, buf + total, (int)(header_len + content_len - total));
      if (n <= 0)
         return 400;
      total += (size_t)n;
   }
   buf[total] = '\0';
   *total_out = (int)total;
   *header_out = (int)header_len;
   *body_out = content_len;
   return 0;
}

static const char *http_reason(int status)
{
   switch (status)
   {
   case 200:
      return "OK";
   case 400:
      return "Bad Request";
   case 401:
      return "Unauthorized";
   case 402:
      return "Payment Required";
   case 403:
      return "Forbidden";
   case 404:
      return "Not Found";
   case 405:
      return "Method Not Allowed";
   case 409:
      return "Conflict";
   case 429:
      return "Too Many Requests";
   case 500:
      return "Internal Server Error";
   case 502:
      return "Bad Gateway";
   case 503:
      return "Service Unavailable";
   case 504:
      return "Gateway Timeout";
   default:
      return "OK";
   }
}

/* Certificate-bound server heartbeat. The server_id is untrusted input but the
 * DB update is keyed by both server_id and the verified peer cert CN, so a
 * certificate cannot refresh another registry row. */
static int mtls_server_heartbeat(const char *issuer, const char *serial, const char *fingerprint,
                                 const char *body, char *resp, int cap)
{
   cJSON *j = body ? cJSON_Parse(body) : NULL;
   cJSON *sid = j ? cJSON_GetObjectItemCaseSensitive(j, "server_id") : NULL;
   cJSON *health = j ? cJSON_GetObjectItemCaseSensitive(j, "health") : NULL;
   cJSON *version = j ? cJSON_GetObjectItemCaseSensitive(j, "version") : NULL;
   int ok = cJSON_IsString(sid) && cJSON_IsString(health) && cJSON_IsString(version) &&
            db2_server_registry_heartbeat(cJSON_GetStringValue(sid), issuer, serial, fingerprint,
                                          cJSON_GetStringValue(health),
                                          cJSON_GetStringValue(version)) == 0;
   cJSON_Delete(j);
   snprintf(resp, (size_t)cap, ok ? "{\"ok\":true}" : "{\"error\":\"heartbeat rejected\"}");
   return ok ? 200 : 403;
}

static int mtls_management_jwks(const char *issuer, const char *serial, const char *fingerprint,
                                char *resp, int cap)
{
   db2_management_jwks_runtime_record_t record;
   db2_management_jwks_runtime_result_t result =
       db2_management_jwks_runtime_fetch(issuer, serial, fingerprint, &record);
   if (result == DB2_MANAGEMENT_JWKS_RUNTIME_DENIED)
   {
      snprintf(resp, (size_t)cap, "{\"error\":\"management JWKS fetch denied\"}");
      return 403;
   }
   if (result != DB2_MANAGEMENT_JWKS_RUNTIME_OK || record.envelope_len + 1 > (size_t)cap)
   {
      snprintf(resp, (size_t)cap, "{\"error\":\"management JWKS unavailable\"}");
      return 503;
   }
   memcpy(resp, record.envelope, record.envelope_len + 1);
   return 200;
}

/* GET /v1/enroll/ca: return the CA certificate (public trust anchor) so a
 * bootstrapping client can pin it by fingerprint. The private key is never
 * exposed. Writes the JSON response; returns the HTTP status. */
static int mtls_get_ca(char *resp, int cap)
{
   char ca_dir[1024];
   snprintf(ca_dir, sizeof(ca_dir), "%s/kb-ca", kb_default_config_dir());
   kb_pki_ca_t ca;
   if (kb_pki_ca_load_custodied(ca_dir, &ca) != 0)
   {
      snprintf(resp, (size_t)cap, "{\"error\":\"no CA\"}");
      return 500;
   }
   cJSON *out = cJSON_CreateObject();
   cJSON_AddStringToObject(out, "ca_cert", ca.cert_pem); /* cert only, never the key */
   OPENSSL_cleanse(&ca, sizeof(ca));
   char *s = cJSON_PrintUnformatted(out);
   snprintf(resp, (size_t)cap, "%s", s ? s : "{}");
   free(s);
   cJSON_Delete(out);
   return 200;
}

/* Handle POST /v1/enroll/renew for an authenticated mTLS client: sign the body's
 * CSR with the CA, binding it to `scope_cn` — the caller's CURRENT verified cert
 * scope (NOT anything in the request) — for a fresh validity period. This lets a
 * client rotate its cert before expiry with no token and no operator action. The
 * client keeps its (new) private key. Writes the JSON response into resp[cap];
 * returns the HTTP status. */
static int mtls_renew(const char *scope_cn, const char *old_fp, const char *old_issuer,
                      const char *old_serial_norm, const char *body, char *resp, int cap)
{
   cJSON *req = body ? cJSON_Parse(body) : NULL;
   const cJSON *jcsr = req ? cJSON_GetObjectItemCaseSensitive(req, "csr") : NULL;
   if (!cJSON_IsString(jcsr))
   {
      cJSON_Delete(req);
      snprintf(resp, (size_t)cap, "{\"error\":\"bad request: csr (PEM string) required\"}");
      return 400;
   }

   char ca_dir[1024];
   snprintf(ca_dir, sizeof(ca_dir), "%s/kb-ca", kb_default_config_dir());
   kb_pki_ca_t ca;
   if (kb_pki_ca_load_custodied(ca_dir, &ca) != 0)
   {
      cJSON_Delete(req);
      snprintf(resp, (size_t)cap, "{\"error\":\"renew unavailable: no CA\"}");
      return 500;
   }

   char *cert = malloc(KB_PKI_CERT_PEM_MAX);
   int rc = cert ? kb_pki_sign_csr(&ca, jcsr->valuestring, scope_cn, 60L * 60 * 24 * 90, cert,
                                   KB_PKI_CERT_PEM_MAX)
                 : -1;
   OPENSSL_cleanse(&ca, sizeof(ca));
   cJSON_Delete(req);
   if (rc != 0)
   {
      free(cert);
      snprintf(resp, (size_t)cap, "{\"error\":\"renew failed: bad CSR\"}");
      return 400;
   }
   char new_fp[KB_PKI_FP_HEX] = "", new_issuer[KB_PKI_ISSUER_MAX + 1] = "";
   char raw_serial[KB_PKI_SERIAL_MAX + 1] = "";
   char new_serial[KB_PKI_SERIAL_MAX + 1] = "";
   int metadata_ok = kb_pki_ca_fingerprint(cert, new_fp, sizeof(new_fp)) == 0 &&
                     kb_pki_cert_metadata(cert, new_issuer, sizeof(new_issuer), raw_serial,
                                          sizeof(raw_serial)) == 0 &&
                     kb_cert_serial_normalize(raw_serial, new_serial, sizeof(new_serial)) == 0;
   kb_principal_t renew_actor = {.kind = KB_PRIN_CERT, .authenticated = 1};
   snprintf(renew_actor.issuer, sizeof(renew_actor.issuer), "%s", old_issuer);
   snprintf(renew_actor.subject, sizeof(renew_actor.subject), "%s", old_serial_norm);
   int persisted = -1;
   if (metadata_ok && db2_tenant_scope_begin(&renew_actor, 0) == 0)
   {
      persisted = db2_enrollment_renew(old_fp, old_issuer, old_serial_norm, scope_cn, new_fp,
                                       new_issuer, new_serial, NULL);
      if (persisted == 0)
         persisted = db2_tenant_scope_commit();
      else
         db2_tenant_scope_rollback();
   }
   if (!metadata_ok || persisted != 0)
   {
      OPENSSL_cleanse(cert, KB_PKI_CERT_PEM_MAX);
      free(cert);
      snprintf(resp, (size_t)cap, "{\"error\":\"renew persistence unavailable\"}");
      return 503;
   }
   cJSON *out = cJSON_CreateObject();
   cJSON_AddStringToObject(out, "client_cert", cert);
   cJSON_AddStringToObject(out, "scope", scope_cn);
   char *s = cJSON_PrintUnformatted(out);
   snprintf(resp, (size_t)cap, "%s", s ? s : "{}");
   free(s);
   cJSON_Delete(out);
   free(cert);
   return 200;
}

void kb_tls_serve_conn(int fd, SSL_CTX *ctx)
{
   if (!ctx)
      return;
   SSL *ssl = SSL_new(ctx);
   if (!ssl)
      return;
   SSL_set_fd(ssl, fd);
   struct timeval io_timeout = {.tv_sec = 30, .tv_usec = 0};
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout));
   setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout));
   /* Handshake — REQUIRES + verifies the client cert (server ctx config). */
   if (SSL_accept(ssl) != 1)
   {
      SSL_free(ssl);
      return;
   }

   char *buf = malloc(KB_TLS_REQ_MAX);
   char *resp = malloc(KB_TLS_RESP_MAX);
   if (!buf || !resp)
      goto done;

   for (;;)
   {
      /* A reusable connection may idle for 30 seconds before the next request.
       * strict_request_read shortens the deadline after the first byte arrives. */
      set_recv_timeout(ssl, 30);
      int total = 0, header_len = 0;
      size_t declared_body = 0;
      int close_after_response = 0;
      char presented_authorization[KB_TLS_AUTH_MAX] = "";
      char service_authorization[KB_TLS_AUTH_MAX] = "";
      char caller_subject[KB_TLS_CALLER_MAX + 1] = "";
      int64_t named_team = 0;
      int read_status = strict_request_read(
          ssl, buf, KB_TLS_REQ_MAX, &total, &header_len, &declared_body, &close_after_response,
          presented_authorization, sizeof(presented_authorization), service_authorization,
          sizeof(service_authorization), caller_subject, sizeof(caller_subject), &named_team);
      io_timeout.tv_sec = 30;
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout));
      if (read_status < 0)
         break;
      if (read_status)
      {
         const char *b = read_status == 413 ? "{\"error\":\"request too large\"}"
                                            : "{\"error\":\"bad request\"}";
         char head[160];
         int hn = snprintf(head, sizeof(head),
                           "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: "
                           "%zu\r\nConnection: close\r\n\r\n",
                           read_status, read_status == 413 ? "Payload Too Large" : "Bad Request",
                           strlen(b));
         SSL_write(ssl, head, hn);
         SSL_write(ssl, b, (int)strlen(b));
         goto done;
      }

      char method[16] = {0}, path[KB_TLS_URI_MAX + 1] = {0};
      if (sscanf(buf, "%15s %4096s", method, path) < 2)
      {
         const char *b = "{\"error\":\"bad request\"}";
         char head[160];
         int hn = snprintf(head, sizeof(head),
                           "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-"
                           "Length: %zu\r\nConnection: close\r\n\r\n",
                           strlen(b));
         SSL_write(ssl, head, hn);
         SSL_write(ssl, b, (int)strlen(b));
         goto done;
      }

      /* Body follows the blank line. */
      const char *body = "";
      int body_len = 0;
      if (header_len > 0)
      {
         body = buf + header_len;
         body_len = (int)declared_body;
      }

      /* Worker threads are long-lived, so a lazily acquired DB2 connection
       * would otherwise remain pinned until shutdown. Bound every routed
       * request explicitly; this also keeps persistent mTLS connections from
       * exhausting the shared DB2 pool after one request per worker. */
      db2_lease_begin();

      /* Split query string off the path. */
      char qs[KB_TLS_URI_MAX + 1] = "", cpath[KB_TLS_URI_MAX + 1] = "";
      const char *qmark = strchr(path, '?');
      if (qmark)
      {
         size_t plen = (size_t)(qmark - path);
         if (plen >= sizeof(cpath))
            plen = sizeof(cpath) - 1;
         memcpy(cpath, path, plen);
         cpath[plen] = '\0';
         snprintf(qs, sizeof(qs), "%s", qmark + 1);
      }
      else
      {
         snprintf(cpath, sizeof(cpath), "%s", path);
      }

      char cn[128] = "";
      int have_cert = (kb_tls_peer_cn(ssl, cn, sizeof(cn)) == 0);

      /* Primary-authoritative mTLS seam: issuer + normalized serial must resolve
       * to an active enrollment. Unknown, revoked, and authority-error outcomes
       * all fail closed before routing; active use gets a debounced last-seen bump. */
      int cert_authority = 0;
      char fp[65] = "", issuer[KB_TLS_PEER_ISSUER_MAX + 1] = "";
      char serial[KB_TLS_PEER_SERIAL_MAX + 1] = "";
      kb_principal_t transport;
      memset(&transport, 0, sizeof(transport));
      if (have_cert)
      {
         if (kb_tls_peer_fingerprint(ssl, fp, sizeof(fp)) == 0 &&
             kb_tls_peer_issuer(ssl, issuer, sizeof(issuer)) == 0 &&
             kb_tls_peer_serial(ssl, serial, sizeof(serial)) == 0 &&
             kb_principal_from_cert(issuer, serial, cn, &transport) == 0)
         {
            cert_authority = db2_enrollment_is_active_by_key(transport.issuer, transport.subject);
            if (cert_authority == 1)
               db2_enrollment_touch_last_seen(fp, cn); /* transport-use telemetry */
         }
      }

      /* Routes reachable WITHOUT a client cert (the enrollment bootstrap): fetch
       * the CA for TOFU pinning, and redeem a token for a cert. */
      int is_bootstrap =
          (strcmp(cpath, "/v1/enroll/ca") == 0 || strcmp(cpath, "/v1/enroll/redeem") == 0);

      /* The certificate authenticates the transport; it does not stand in for
       * the independently rotating service bearer. Verify the bearer afresh on
       * every request so a pooled connection observes rotation/revocation at
       * request N+1. OIDC, when configured, runs through this same verifier
       * registry with its issuer/audience/signature policy pinned. */
      char expected_bearer[KB_TLS_BEARER_MAX + 1] = "";
      if (!runtime_secret_get("AIMEE_KB_API_BEARER_TOKEN", expected_bearer,
                              sizeof(expected_bearer)))
         snprintf(expected_bearer, sizeof(expected_bearer), "%s", config_kb_api_bearer_token());
      const char *presented_bearer = aimee_core_bearer_token(presented_authorization);
      kb_verify_result_t service_identity;
      int bearer_authority =
          expected_bearer[0] && presented_bearer &&
          kb_verifier_authenticate(presented_bearer, expected_bearer, &service_identity, NULL, 0);
      int identity_matches =
          bearer_authority && bearer_identity_matches_certificate(cn, &service_identity);
      kb_principal_t application_identity;
      int application_authority =
          service_identity_authenticate(service_authorization, &application_identity);
      kb_principal_t caller_identity = {0};
      int caller_authority =
          caller_subject[0]
              ? kb_principal_from_identity_key(caller_subject, &caller_identity) == 0 ? 1 : -1
              : 0;

      int status;
      /* B5: kb never honors a client-supplied identity header; reject fail-closed
       * before any route runs. */
      if (kb_ingress_identity_header_present_ex(buf, 1))
      {
         LOG_WARN(
             "kb.tls",
             "kb ingress (mtls): rejected request bearing a spoofable X-Aimee-* identity header");
         snprintf(resp, KB_TLS_RESP_MAX, "{\"error\":\"identity header not permitted\"}");
         status = 400;
      }
      else if (caller_subject[0] && is_bootstrap)
      {
         snprintf(resp, KB_TLS_RESP_MAX,
                  "{\"error\":\"caller subject is not permitted on bootstrap routes\"}");
         status = 400;
      }
      /* A revoked/unknown cert or unavailable authority is rejected before any
       * route runs. Authority failure is retryable but never fail-open. */
      else if (have_cert && cert_authority != 1)
      {
         close_after_response = 1;
         if (cert_authority < 0)
         {
            snprintf(resp, KB_TLS_RESP_MAX,
                     "{\"error\":\"certificate authority temporarily unavailable\"}");
            status = 503;
         }
         else
         {
            snprintf(resp, KB_TLS_RESP_MAX,
                     "{\"error\":\"client certificate is unknown or revoked\"}");
            status = 403;
         }
      }
      /* A client without a cert yet (still enrolling) may ONLY use bootstrap
       * routes. Everything else requires an identity, so a cert-less peer is 401. */
      else if (!have_cert && !is_bootstrap)
      {
         snprintf(resp, KB_TLS_RESP_MAX,
                  "{\"error\":\"client certificate required (enroll first via "
                  "/v1/enroll/redeem)\"}");
         status = 401;
      }
      /* Enrollment bootstrap is the only bearer-less exception. Every request
       * made with an enrolled certificate must also carry the current bearer;
       * a missing KB-side authority is a service/configuration failure, while a
       * missing or rejected presented credential is ordinary unauthorized. */
      else if (have_cert && !is_bootstrap && !bearer_authority)
      {
         close_after_response = 1;
         if (!expected_bearer[0])
         {
            snprintf(resp, KB_TLS_RESP_MAX,
                     "{\"error\":\"service bearer authority is not configured\"}");
            status = 503;
         }
         else
         {
            snprintf(resp, KB_TLS_RESP_MAX, "{\"error\":\"service bearer required\"}");
            status = 401;
         }
      }
      else if (have_cert && !is_bootstrap && !identity_matches)
      {
         close_after_response = 1;
         snprintf(resp, KB_TLS_RESP_MAX,
                  "{\"error\":\"service identity does not match client certificate\"}");
         status = 403;
      }
      else if (have_cert && !is_bootstrap && application_authority < 0)
      {
         close_after_response = 1;
         snprintf(resp, KB_TLS_RESP_MAX,
                  "{\"error\":\"service OIDC authority is not safely configured\"}");
         status = 503;
      }
      else if (have_cert && !is_bootstrap && application_authority != 1)
      {
         close_after_response = 1;
         snprintf(resp, KB_TLS_RESP_MAX, "{\"error\":\"OIDC or PAM service identity required\"}");
         status = 401;
      }
      else if (have_cert && !is_bootstrap &&
               !application_identity_matches_certificate(cn, &application_identity))
      {
         close_after_response = 1;
         snprintf(resp, KB_TLS_RESP_MAX,
                  "{\"error\":\"service identity does not match client certificate\"}");
         status = 403;
      }
      else if (have_cert && !is_bootstrap && caller_authority < 0)
      {
         snprintf(resp, KB_TLS_RESP_MAX, "{\"error\":\"invalid caller subject\"}");
         status = 400;
      }
      /* GET /v1/enroll/ca: return the CA cert so a bootstrapping client can pin it
       * by fingerprint (the value in its connection string). */
      else if (strcmp(cpath, "/v1/enroll/ca") == 0)
      {
         status = (strcmp(method, "GET") == 0) ? mtls_get_ca(resp, KB_TLS_RESP_MAX) : 405;
         if (status == 405)
            snprintf(resp, KB_TLS_RESP_MAX, "{\"error\":\"method not allowed\"}");
      }
      /* Certificate-only P2b egress authority: never route through the synthetic
       * CN bearer. The exact verified issuer/serial/fingerprint are carried in. */
      else if (have_cert && strcmp(cpath, "/v1/llm/egress") == 0)
      {
         status = kb_http_egress_route(method, cpath, body, body_len, &transport, fp, resp,
                                       KB_TLS_RESP_MAX);
      }
      /* P5-C2c public verification artifact: certificate-only, primary-backed,
       * exact FINAL bytes.  It never enters the bearer/console router. */
      else if (have_cert && strcmp(cpath, "/v1/management/jwks") == 0)
      {
         if (qs[0] || body_len)
         {
            snprintf(resp, KB_TLS_RESP_MAX, "{\"error\":\"query or body not allowed\"}");
            status = 400;
         }
         else if (strcmp(method, "GET") != 0)
         {
            snprintf(resp, KB_TLS_RESP_MAX, "{\"error\":\"method not allowed\"}");
            status = 405;
         }
         else
            status = mtls_management_jwks(issuer, transport.subject, fp, resp, KB_TLS_RESP_MAX);
         if (status == 403 || status == 503)
            close_after_response = 1;
      }
      /* Cert rotation: an authenticated client renews its cert for its CURRENT
       * verified scope (the cert is the credential — no token needed). */
      else if (have_cert && strcmp(cpath, "/v1/enroll/renew") == 0)
      {
         if (strcmp(method, "POST") != 0)
         {
            snprintf(resp, KB_TLS_RESP_MAX, "{\"error\":\"method not allowed\"}");
            status = 405;
         }
         else
         {
            status = mtls_renew(cn, fp, issuer, transport.subject, body, resp, KB_TLS_RESP_MAX);
         }
      }
      else if (have_cert && strcmp(cpath, "/v1/server/heartbeat") == 0)
      {
         status = (strcmp(method, "POST") == 0)
                      ? mtls_server_heartbeat(transport.issuer, transport.subject, fp, body, resp,
                                              KB_TLS_RESP_MAX)
                      : 405;
         if (status == 405)
            snprintf(resp, KB_TLS_RESP_MAX, "{\"error\":\"method not allowed\"}");
      }
      else
      {
         int tenant_scope_open = 0;
         kb_request_context_t resolved;
         memset(&resolved, 0, sizeof(resolved));
         if (caller_authority == 1 && kb_http_is_content_read(method, cpath))
         {
            /* The OIDC/PAM identity is the enrolled SERVICE side of this
             * intersection. mTLS has already independently authenticated and
             * authorized the transport above; certificate identity is not a
             * substitute for the service's third-layer identity. */
            kb_resolve_status_t rr = kb_identity_resolve(&application_identity, &caller_identity,
                                                          named_team, &resolved);
            if (rr == KB_RESOLVE_CONFLICT)
            {
               snprintf(resp, KB_TLS_RESP_MAX,
                        "{\"error\":\"service and caller have no shared KB team\"}");
               status = 403;
               goto content_done;
            }
            if (rr == KB_RESOLVE_AMBIGUOUS_DEFAULT)
            {
               snprintf(resp, KB_TLS_RESP_MAX,
                        "{\"error\":\"service and caller have no unambiguous default team\"}");
               status = 409;
               goto content_done;
            }
            if (rr != KB_RESOLVE_OK || resolved.billing_team <= 0)
            {
               snprintf(resp, KB_TLS_RESP_MAX,
                        "{\"error\":\"content caller could not be resolved\"}");
               status = 403;
               goto content_done;
            }
            int scope_rc = db2_tenant_scope_begin(&resolved.actor, resolved.billing_team);
            if (scope_rc != 0)
            {
               snprintf(resp, KB_TLS_RESP_MAX,
                        "{\"error\":\"content tenant scope unavailable\"}");
               status = scope_rc == DB2_ERR_TENANT_DENIED ? 403 : 503;
               goto content_done;
            }
            tenant_scope_open = 1;
         }
         if (tenant_scope_open)
            status = kb_http_route_ex_with_context(method, cpath, qs, presented_authorization,
                                                    expected_bearer, body, body_len, &resolved, resp,
                                                    KB_TLS_RESP_MAX);
         else
            status = kb_http_route_ex_with_actor(
                method, cpath, qs, presented_authorization, expected_bearer, body, body_len,
                caller_authority == 1 ? &caller_identity : NULL, resp, KB_TLS_RESP_MAX);
      content_done:
         if (tenant_scope_open)
            db2_tenant_scope_rollback();
         memset(&resolved, 0, sizeof(resolved));
      }
      kb_reqctx_clear(); /* drop the request's actor before the next request on this conn */
      db2_lease_end();
      OPENSSL_cleanse(&service_identity, sizeof(service_identity));
      OPENSSL_cleanse(&application_identity, sizeof(application_identity));
      OPENSSL_cleanse(&caller_identity, sizeof(caller_identity));
      runtime_secret_wipe(expected_bearer, sizeof(expected_bearer));
      OPENSSL_cleanse(presented_authorization, sizeof(presented_authorization));
      OPENSSL_cleanse(service_authorization, sizeof(service_authorization));
      OPENSSL_cleanse(caller_subject, sizeof(caller_subject));

      char head[256];
      int hn = snprintf(head, sizeof(head),
                        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: "
                        "%zu\r\nConnection: %s\r\n\r\n",
                        status, http_reason(status), strlen(resp),
                        close_after_response ? "close" : "keep-alive");
      SSL_write(ssl, head, hn);
      SSL_write(ssl, resp, (int)strlen(resp));
      if (close_after_response)
         break;
   }

done:
   free(buf);
   free(resp);
   SSL_shutdown(ssl);
   SSL_free(ssl);
}

/* --- the kb mTLS listener (distributed mode) --- */

#include "kb_pki.h"
#include "log.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/crypto.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_mtls_listen_fd = -1;
static int g_mtls_port = 0;
static volatile int g_mtls_running = 0;
static pthread_t g_mtls_thread;
static SSL_CTX *g_mtls_ctx = NULL;

#define KB_MTLS_CONNECTIONS_MAX   64
#define KB_MTLS_QUEUE_CAP         64
#define KB_MTLS_WORKER_STACK_SIZE (16 * 1024 * 1024)
/* Keep ample headroom for route-local state and TLS/libpq frames; live memory
 * queries exhausted 4 MiB once nested search frames were active concurrently.
 *
 * This used to be asserted as sizeof(config_t) + 1 MiB, because routes loaded a
 * whole ~750 KiB config_t onto this stack. They no longer do -- config is read a
 * field at a time -- so the config term is gone and the floor is stated
 * directly. The headroom is still needed for the nested-search case, which is
 * what actually exhausted the old 4 MiB. */
_Static_assert(KB_MTLS_WORKER_STACK_SIZE >= 8 * 1024 * 1024,
               "kb mTLS worker stack must keep headroom for nested route + TLS/libpq frames");
static pthread_t g_mtls_workers[KB_MTLS_CONNECTIONS_MAX];
static int g_mtls_workers_started = 0;
static int g_mtls_connection_limit = KB_MTLS_CONNECTIONS_MAX;
static int g_mtls_connections_live = 0;
static int g_mtls_queue[KB_MTLS_QUEUE_CAP];
static size_t g_mtls_queue_head = 0;
static size_t g_mtls_queue_len = 0;
static pthread_mutex_t g_mtls_queue_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_mtls_queue_cv = PTHREAD_COND_INITIALIZER;

static void *mtls_worker_thread(void *arg)
{
   (void)arg;
   for (;;)
   {
      pthread_mutex_lock(&g_mtls_queue_mu);
      while (g_mtls_queue_len == 0 && g_mtls_running)
         pthread_cond_wait(&g_mtls_queue_cv, &g_mtls_queue_mu);
      if (g_mtls_queue_len == 0 && !g_mtls_running)
      {
         pthread_mutex_unlock(&g_mtls_queue_mu);
         break;
      }
      int fd = g_mtls_queue[g_mtls_queue_head];
      g_mtls_queue_head = (g_mtls_queue_head + 1) % KB_MTLS_QUEUE_CAP;
      g_mtls_queue_len--;
      pthread_mutex_unlock(&g_mtls_queue_mu);

      int one = 1;
      (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      kb_tls_serve_conn(fd, g_mtls_ctx);
      db2_lease_release_idle();
      close(fd);
      pthread_mutex_lock(&g_mtls_queue_mu);
      if (g_mtls_connections_live > 0)
         g_mtls_connections_live--;
      pthread_mutex_unlock(&g_mtls_queue_mu);
   }
   return NULL;
}

static int mtls_queue_conn(int fd)
{
   int queued = 0;
   pthread_mutex_lock(&g_mtls_queue_mu);
   if (g_mtls_running && g_mtls_connections_live < g_mtls_connection_limit &&
       g_mtls_queue_len < KB_MTLS_QUEUE_CAP)
   {
      size_t tail = (g_mtls_queue_head + g_mtls_queue_len) % KB_MTLS_QUEUE_CAP;
      g_mtls_queue[tail] = fd;
      g_mtls_queue_len++;
      g_mtls_connections_live++;
      queued = 1;
      pthread_cond_signal(&g_mtls_queue_cv);
   }
   pthread_mutex_unlock(&g_mtls_queue_mu);
   return queued ? 0 : -1;
}

static void *mtls_listener_thread(void *arg)
{
   (void)arg;
   while (g_mtls_running)
   {
      int fd = accept(g_mtls_listen_fd, NULL, NULL);
      if (fd < 0)
      {
         if (g_mtls_running)
            continue;
         break;
      }
      /* Close-on-exec, as the plaintext listener already does (kb_http_listener.c).
       * This process forks constantly — a curator sidecar per symbol, pdf and
       * normalize helpers — and a child that inherits a client's socket holds it
       * open past our close(), leaving that client blocked reading a response we
       * already finished. In the managed topology this listener carries the
       * aimee-server -> aimee-kb traffic, so the stall lands on the server. */
      fcntl(fd, F_SETFD, FD_CLOEXEC);
      /* Bound both concurrent handshakes and queued sockets. Saturation is
       * fail-closed: the accepted socket is dropped before reading a request. */
      if (mtls_queue_conn(fd) != 0)
         close(fd);
   }
   return NULL;
}

int kb_mtls_start(int port, const char *data_dir, const char *host)
{
   if (port < 0 || !data_dir || !data_dir[0] || !host || !host[0])
      return -1;

   g_mtls_connection_limit = KB_MTLS_CONNECTIONS_MAX;
   const char *limit_text = getenv("AIMEE_KB_MTLS_MAX_CONNECTIONS");
   if (limit_text && limit_text[0])
   {
      char *end = NULL;
      long configured = strtol(limit_text, &end, 10);
      if (!end || *end || configured < 1 || configured > KB_MTLS_CONNECTIONS_MAX)
         return -1;
      g_mtls_connection_limit = (int)configured;
   }

   /* CA (persistent) + a fresh server cert signed by it. */
   char ca_dir[1024];
   if (snprintf(ca_dir, sizeof(ca_dir), "%s/kb-ca", data_dir) >= (int)sizeof(ca_dir))
      return -1;
   kb_pki_ca_t ca;
   if (kb_pki_ca_load_or_create_custodied(ca_dir, &ca, NULL) != 0)
      return -1;
   char scert[KB_PKI_CERT_PEM_MAX], skey[KB_PKI_KEY_PEM_MAX];
   int issued = kb_pki_issue_server_cert(&ca, host, 60L * 60 * 24 * 365, scert, sizeof(scert), skey,
                                         sizeof(skey));
   if (issued != 0)
   {
      OPENSSL_cleanse(&ca, sizeof(ca));
      return -1;
   }
   g_mtls_ctx = kb_tls_server_ctx(ca.cert_pem, scert, skey);
   OPENSSL_cleanse(&ca, sizeof(ca));
   OPENSSL_cleanse(skey, sizeof(skey));
   if (!g_mtls_ctx)
      return -1;

   g_mtls_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
   if (g_mtls_listen_fd < 0)
      goto fail;
   int opt = 1;
   setsockopt(g_mtls_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
   struct sockaddr_in sa;
   memset(&sa, 0, sizeof(sa));
   sa.sin_family = AF_INET;
   sa.sin_addr.s_addr = htonl(INADDR_ANY); /* distributed mode: remote peers */
   sa.sin_port = htons((uint16_t)port);
   if (bind(g_mtls_listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
       listen(g_mtls_listen_fd, KB_MTLS_CONNECTIONS_MAX) < 0)
      goto fail;

   /* Resolve the actually-bound port (when started with 0). */
   struct sockaddr_in bound;
   socklen_t blen = sizeof(bound);
   if (getsockname(g_mtls_listen_fd, (struct sockaddr *)&bound, &blen) == 0)
      g_mtls_port = ntohs(bound.sin_port);
   else
      g_mtls_port = port;

   pthread_mutex_lock(&g_mtls_queue_mu);
   g_mtls_queue_head = 0;
   g_mtls_queue_len = 0;
   g_mtls_connections_live = 0;
   g_mtls_running = 1;
   pthread_mutex_unlock(&g_mtls_queue_mu);
   pthread_attr_t worker_attr;
   pthread_attr_t *worker_attr_ptr = NULL;
   int worker_attr_initialized = pthread_attr_init(&worker_attr) == 0;
   if (worker_attr_initialized)
   {
      if (pthread_attr_setstacksize(&worker_attr, KB_MTLS_WORKER_STACK_SIZE) == 0)
         worker_attr_ptr = &worker_attr;
   }
   for (int i = 0; i < g_mtls_connection_limit; i++)
   {
      if (pthread_create(&g_mtls_workers[i], worker_attr_ptr, mtls_worker_thread, NULL) != 0)
      {
         if (worker_attr_initialized)
            pthread_attr_destroy(&worker_attr);
         pthread_mutex_lock(&g_mtls_queue_mu);
         g_mtls_running = 0;
         pthread_cond_broadcast(&g_mtls_queue_cv);
         pthread_mutex_unlock(&g_mtls_queue_mu);
         for (int j = 0; j < i; j++)
            pthread_join(g_mtls_workers[j], NULL);
         g_mtls_workers_started = 0;
         goto fail;
      }
      g_mtls_workers_started++;
   }
   if (worker_attr_initialized)
      pthread_attr_destroy(&worker_attr);
   if (pthread_create(&g_mtls_thread, NULL, mtls_listener_thread, NULL) != 0)
   {
      pthread_mutex_lock(&g_mtls_queue_mu);
      g_mtls_running = 0;
      pthread_cond_broadcast(&g_mtls_queue_cv);
      pthread_mutex_unlock(&g_mtls_queue_mu);
      for (int i = 0; i < g_mtls_workers_started; i++)
         pthread_join(g_mtls_workers[i], NULL);
      g_mtls_workers_started = 0;
      goto fail;
   }
   LOG_INFO("kb_mtls", "mTLS listening on 0.0.0.0:%d (host %s, max connections %d)", g_mtls_port,
            host, g_mtls_connection_limit);
   return 0;

fail:
   if (g_mtls_listen_fd >= 0)
   {
      close(g_mtls_listen_fd);
      g_mtls_listen_fd = -1;
   }
   if (g_mtls_ctx)
   {
      SSL_CTX_free(g_mtls_ctx);
      g_mtls_ctx = NULL;
   }
   g_mtls_port = 0;
   return -1;
}

int kb_mtls_bound_port(void)
{
   return g_mtls_running ? g_mtls_port : 0;
}

void kb_mtls_connection_stats(int *limit_out, int *live_out, int *queued_out)
{
   pthread_mutex_lock(&g_mtls_queue_mu);
   if (limit_out)
      *limit_out = g_mtls_connection_limit;
   if (live_out)
      *live_out = g_mtls_connections_live;
   if (queued_out)
      *queued_out = (int)g_mtls_queue_len;
   pthread_mutex_unlock(&g_mtls_queue_mu);
}

void kb_mtls_stop(void)
{
   if (!g_mtls_running)
      return;
   pthread_mutex_lock(&g_mtls_queue_mu);
   g_mtls_running = 0;
   pthread_mutex_unlock(&g_mtls_queue_mu);
   if (g_mtls_listen_fd >= 0)
   {
      shutdown(g_mtls_listen_fd, SHUT_RDWR);
      close(g_mtls_listen_fd);
      g_mtls_listen_fd = -1;
   }
   pthread_join(g_mtls_thread, NULL);
   pthread_mutex_lock(&g_mtls_queue_mu);
   pthread_cond_broadcast(&g_mtls_queue_cv);
   pthread_mutex_unlock(&g_mtls_queue_mu);
   for (int i = 0; i < g_mtls_workers_started; i++)
      pthread_join(g_mtls_workers[i], NULL);
   g_mtls_workers_started = 0;
   if (g_mtls_ctx)
   {
      SSL_CTX_free(g_mtls_ctx);
      g_mtls_ctx = NULL;
   }
   g_mtls_port = 0;
}
