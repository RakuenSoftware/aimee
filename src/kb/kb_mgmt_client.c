#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "kb_mgmt_client.h"
#include "kb_mgmt_endpoint.h"
#include "kb_tls.h"
#include <openssl/crypto.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int response_content_length(const char *raw, const char *end, size_t cap, size_t *length,
                                   int *reusable)
{
   const char *line = strstr(raw, "\r\n");
   int seen = 0, connection_seen = 0;
   *reusable = 1; /* HTTP/1.1 persistence is the default. */
   if (!line || line >= end)
      return -1;
   line += 2;
   while (line < end)
   {
      const char *next = strstr(line, "\r\n");
      if (!next || next > end || next == line)
         return -1;
      size_t n = (size_t)(next - line);
      const char *colon = memchr(line, ':', n);
      if (!colon || colon == line)
         return -1;
      for (const char *p = line; p < colon; p++)
         if (!(isalnum((unsigned char)*p) || *p == '-'))
            return -1;
      for (const char *p = colon + 1; p < next; p++)
         if (((unsigned char)*p < 0x20 && *p != '\t') || (unsigned char)*p == 0x7f)
            return -1;
      if (n >= 18 && strncasecmp(line, "Transfer-Encoding:", 18) == 0)
         return -1;
      if ((n >= 8 && strncasecmp(line, "Upgrade:", 8) == 0) ||
          (n >= 8 && strncasecmp(line, "Trailer:", 8) == 0))
         return -1;
      if (n >= 11 && strncasecmp(line, "Connection:", 11) == 0)
      {
         if (connection_seen++)
            return -1;
         const char *p = line + 11;
         while (p < next && (*p == ' ' || *p == '\t'))
            p++;
         if ((size_t)(next - p) == 10 && strncasecmp(p, "keep-alive", 10) == 0)
            *reusable = 1;
         else if ((size_t)(next - p) == 5 && strncasecmp(p, "close", 5) == 0)
            *reusable = 0;
         else
            return -1;
      }
      if (n >= 15 && strncasecmp(line, "Content-Length:", 15) == 0)
      {
         const char *p = line + 15;
         while (p < next && (*p == ' ' || *p == '\t'))
            p++;
         if (seen++ || p == next)
            return -1;
         size_t v = 0;
         for (; p < next; p++)
         {
            if (*p < '0' || *p > '9' || v > (SIZE_MAX - (size_t)(*p - '0')) / 10)
               return -1;
            v = v * 10 + (size_t)(*p - '0');
         }
         if (v >= cap)
            return -1;
         *length = v;
      }
      line = next + 2;
   }
   return seen == 1 ? 0 : -1;
}

static int response_status(const char *raw, const char *end)
{
   const char *line_end = strstr(raw, "\r\n");
   if (!line_end || line_end >= end || line_end - raw < 13 || memcmp(raw, "HTTP/1.1 ", 9) != 0 ||
       raw[9] < '1' || raw[9] > '5' || raw[10] < '0' || raw[10] > '9' || raw[11] < '0' ||
       raw[11] > '9' || raw[12] != ' ')
      return -1;
   return (raw[9] - '0') * 100 + (raw[10] - '0') * 10 + raw[11] - '0';
}

static int response_json_content_type(const char *raw, const char *end)
{
   const char *line = strstr(raw, "\r\n");
   int seen = 0;
   if (!line || line >= end)
      return 0;
   line += 2;
   while (line < end)
   {
      const char *next = strstr(line, "\r\n");
      if (!next || next > end || next == line)
         return 0;
      static const char name[] = "Content-Type:";
      if ((size_t)(next - line) >= sizeof(name) - 1 &&
          strncasecmp(line, name, sizeof(name) - 1) == 0)
      {
         const char *value = line + sizeof(name) - 1;
         while (value < next && (*value == ' ' || *value == '\t'))
            value++;
         if (seen++ || (size_t)(next - value) != sizeof("application/json") - 1 ||
             memcmp(value, "application/json", sizeof("application/json") - 1))
            return 0;
      }
      line = next + 2;
   }
   return seen == 1;
}

static int request_headers_valid(const char *headers)
{
   if (!headers || !headers[0])
      return 1;
   size_t total = strlen(headers);
   if (total < 2 || memcmp(headers + total - 2, "\r\n", 2) != 0 || strstr(headers, "\r\n\r\n"))
      return 0;
   const char *line = headers;
   while (*line)
   {
      const char *end = strstr(line, "\r\n"), *colon = end ? memchr(line, ':', end - line) : NULL;
      if (!end || !colon || colon == line ||
          ((size_t)(colon - line) == 4 && strncasecmp(line, "Host", 4) == 0) ||
          ((size_t)(colon - line) == 10 && strncasecmp(line, "Connection", 10) == 0) ||
          ((size_t)(colon - line) == 14 && strncasecmp(line, "Content-Length", 14) == 0) ||
          ((size_t)(colon - line) == 17 && strncasecmp(line, "Transfer-Encoding", 17) == 0))
         return 0;
      for (const char *p = line; p < end; p++)
         if (((unsigned char)*p < 0x20 && *p != '\t') || (unsigned char)*p == 0x7f)
            return 0;
      line = end + 2;
   }
   return 1;
}

void kb_mgmt_client_session_close(kb_mgmt_client_session_t *s)
{
   if (!s)
      return;
   if (s->ssl)
   {
      SSL_shutdown(s->ssl);
      SSL_free(s->ssl);
   }
   if (s->fd >= 0)
      close(s->fd);
   SSL_CTX_free(s->ctx);
   memset(s, 0, sizeof(*s));
   s->fd = -1;
}

static uint64_t monotonic_ms(void)
{
   struct timespec ts;
   return clock_gettime(CLOCK_MONOTONIC, &ts) == 0
              ? (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000
              : UINT64_MAX;
}

static int wait_ssl(SSL *ssl, int result, uint64_t deadline)
{
   int error = SSL_get_error(ssl, result);
   short events = error == SSL_ERROR_WANT_READ    ? POLLIN
                  : error == SSL_ERROR_WANT_WRITE ? POLLOUT
                                                  : 0;
   uint64_t now = monotonic_ms();
   if (!events || now >= deadline)
      return -1;
   uint64_t left = deadline - now;
   struct pollfd p = {.fd = SSL_get_fd(ssl), .events = events};
   int rc;
   do
      rc = poll(&p, 1, left > INT_MAX ? INT_MAX : (int)left);
   while (rc < 0 && errno == EINTR && monotonic_ms() < deadline);
   return rc > 0 && (p.revents & events) ? 0 : -1;
}

int kb_mgmt_client_session_open_deadline(kb_mgmt_client_session_t *s, const char *endpoint,
                                         const char *ca, const char *cc, const char *ck,
                                         const char *expected_issuer, const char *expected_serial,
                                         const char *expected_fp, uint64_t deadline, int trusted)
{
   if (!s || !ca || ((cc != NULL) != (ck != NULL)) || monotonic_ms() >= deadline)
      return -1;
   memset(s, 0, sizeof(*s));
   s->fd = -1;
   if (kb_mgmt_endpoint_parse(endpoint, &s->endpoint) != 0 ||
       !(s->ctx = kb_tls_client_ctx(ca, cc, ck)) ||
       (s->fd = kb_mgmt_endpoint_connect_deadline(&s->endpoint, deadline, trusted)) < 0 ||
       !(s->ssl = SSL_new(s->ctx)))
      goto fail;
   SSL_set_fd(s->ssl, s->fd);
   if (SSL_set_tlsext_host_name(s->ssl, s->endpoint.host) != 1 ||
       SSL_set1_host(s->ssl, s->endpoint.host) != 1)
      goto fail;
   int connect_rc;
   while ((connect_rc = SSL_connect(s->ssl)) != 1)
      if (wait_ssl(s->ssl, connect_rc, deadline))
         goto fail;
   if (monotonic_ms() >= deadline || SSL_get_verify_result(s->ssl) != X509_V_OK)
      goto fail;
   if (expected_issuer || expected_serial || expected_fp)
   {
      char issuer[601] = "", serial[129] = "", fp[65] = "";
      if (!expected_issuer || !expected_serial || !expected_fp || strnlen(expected_fp, 65) != 64 ||
          !expected_serial[0] || kb_tls_peer_issuer(s->ssl, issuer, sizeof(issuer)) != 0 ||
          kb_tls_peer_serial(s->ssl, serial, sizeof(serial)) != 0 ||
          kb_tls_peer_fingerprint(s->ssl, fp, sizeof(fp)) != 0)
         goto fail;
      for (size_t i = 0; serial[i]; i++)
         serial[i] = (char)tolower((unsigned char)serial[i]);
      if (strcmp(issuer, expected_issuer) || strcmp(serial, expected_serial) ||
          CRYPTO_memcmp(fp, expected_fp, 64) != 0)
         goto fail;
   }
   return 0;
fail:
   kb_mgmt_client_session_close(s);
   return -1;
}

int kb_mgmt_client_session_open(kb_mgmt_client_session_t *s, const char *endpoint, const char *ca,
                                const char *cc, const char *ck, const char *expected_issuer,
                                const char *expected_serial, const char *expected_fp)
{
   uint64_t now = monotonic_ms();
   return now == UINT64_MAX
              ? -1
              : kb_mgmt_client_session_open_deadline(s, endpoint, ca, cc, ck, expected_issuer,
                                                     expected_serial, expected_fp, now + 30000, 0);
}

static int session_request_deadline(kb_mgmt_client_session_t *s, const char *method,
                                    const char *path, const char *body, const char *extra_headers,
                                    uint64_t deadline, char *resp, size_t cap, int *status_out,
                                    int strict_action, size_t *bytes_sent)
{
   if (bytes_sent)
      *bytes_sent = 0;
   if (resp && cap)
      resp[0] = '\0';
   if (status_out)
      *status_out = 0;
   if (!s || !s->ssl || !method ||
       (strcmp(method, "GET") && strcmp(method, "HEAD") && strcmp(method, "POST")) || !path ||
       path[0] != '/' || strpbrk(path, "\r\n\t ") || !resp || !cap || cap > 1024 * 1024 ||
       monotonic_ms() >= deadline)
      return -1;
   const char *b = body ? body : "", *headers = extra_headers ? extra_headers : "";
   size_t blen = strlen(b);
   int bodyless = strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0;
   if ((bodyless && blen) || !request_headers_valid(headers))
      return -1;
   size_t req_cap = strlen(method) + strlen(path) + strlen(s->endpoint.host_header) +
                    strlen(headers) + blen + 192;
   char *req = malloc(req_cap);
   if (!req)
      return -1;
   int n = snprintf(req, req_cap,
                    "%s %s HTTP/1.1\r\nHost: %s\r\n%sContent-Type: application/json\r\n"
                    "Content-Length: %zu\r\nConnection: %s\r\n\r\n%s",
                    method, path, s->endpoint.host_header, headers, blen,
                    strict_action ? "close" : "keep-alive", b);
   int rc = -1;
   if (n <= 0 || (size_t)n >= req_cap || (strict_action && (size_t)n >= 8192U))
      goto done;
   size_t sent = 0;
   while (sent < (size_t)n)
   {
      int wr = SSL_write(s->ssl, req + sent, n - (int)sent);
      if (wr <= 0)
      {
         if (wait_ssl(s->ssl, wr, deadline) == 0)
            continue;
         goto done;
      }
      sent += (size_t)wr;
      if (bytes_sent)
         *bytes_sent = sent;
      if (monotonic_ms() >= deadline)
         goto done;
   }
   size_t raw_cap = cap + 8192, total = 0, content_length = SIZE_MAX;
   int reusable = 0;
   char *raw = malloc(raw_cap);
   if (!raw)
      goto done;
   while (total < raw_cap - 1)
   {
      int rd = SSL_read(s->ssl, raw + total, (int)(raw_cap - 1 - total));
      if (rd <= 0)
      {
         if (wait_ssl(s->ssl, rd, deadline) == 0)
            continue;
         break;
      }
      total += (size_t)rd;
      raw[total] = '\0';
      char *end = strstr(raw, "\r\n\r\n");
      if (end && content_length == SIZE_MAX)
      {
         if (response_content_length(raw, end, cap, &content_length, &reusable) != 0)
            break;
      }
      if (end && content_length != SIZE_MAX && total >= (size_t)(end + 4 - raw) + content_length)
      {
         size_t exact = (size_t)(end + 4 - raw) + content_length;
         int status = response_status(raw, end);
         if (status < 0 || (!strcmp(path, "/v1/management/challenge") && !reusable) ||
             (!strcmp(path, "/v1/management/action-checkpoint") && reusable) || total != exact ||
             memchr(end + 4, '\0', content_length) || SSL_pending(s->ssl) > 0 ||
             monotonic_ms() >= deadline)
            break;
         memcpy(resp, end + 4, content_length);
         resp[content_length] = '\0';
         if (status_out)
            *status_out = strict_action && !response_json_content_type(raw, end) ? 0 : status;
         rc = !strict_action && status >= 300 && status < 400 ? -1 : 0;
         break;
      }
   }
   free(raw);
done:
   free(req);
   if (rc != 0)
      kb_mgmt_client_session_close(s);
   return rc;
}

int kb_mgmt_client_session_checkpoint_deadline(kb_mgmt_client_session_t *s, const char *body,
                                               uint64_t deadline, char *resp, size_t cap,
                                               int *status_out)
{
   return session_request_deadline(s, "POST", "/v1/management/action-checkpoint", body, NULL,
                                   deadline, resp, cap, status_out, 1, NULL);
}

int kb_mgmt_client_session_request_deadline(kb_mgmt_client_session_t *s, const char *method,
                                            const char *path, const char *body,
                                            const char *extra_headers, uint64_t deadline,
                                            char *resp, size_t cap, int *status_out)
{
   return session_request_deadline(s, method, path, body, extra_headers, deadline, resp, cap,
                                   status_out, 0, NULL);
}

kb_mgmt_client_send_result_t kb_mgmt_client_session_action_deadline(kb_mgmt_client_session_t *s,
                                                                    const char *body,
                                                                    const char *extra_headers,
                                                                    uint64_t deadline, char *resp,
                                                                    size_t cap, int *status_out)
{
   size_t sent = 0;
   int rc = session_request_deadline(s, "POST", "/v1/management/action", body, extra_headers,
                                     deadline, resp, cap, status_out, 1, &sent);
   if (rc == 0)
      return KB_MGMT_CLIENT_SENT_RESPONSE;
   return sent ? KB_MGMT_CLIENT_SENT_AMBIGUOUS : KB_MGMT_CLIENT_NOT_SENT;
}

int kb_mgmt_client_session_request(kb_mgmt_client_session_t *s, const char *method,
                                   const char *path, const char *body, const char *extra_headers,
                                   char *resp, size_t cap, int *status_out)
{
   uint64_t now = monotonic_ms();
   return now == UINT64_MAX
              ? -1
              : kb_mgmt_client_session_request_deadline(s, method, path, body, extra_headers,
                                                        now + 30000, resp, cap, status_out);
}

int kb_mgmt_client_request_auth(const char *ep, const char *ca, const char *cc, const char *ck,
                                const char *m, const char *path, const char *b, const char *auth,
                                char *r, size_t n, int *st)
{
   if (kb_mgmt_endpoint_validate(ep) != 0 || !ca || !m || !path || !r || !n)
      return -1;
   kb_mgmt_client_session_t session;
   if (kb_mgmt_client_session_open(&session, ep, ca, cc, ck, NULL, NULL, NULL) != 0)
      return -1;
   int rc = kb_mgmt_client_session_request(&session, m, path, b, auth, r, n, st);
   kb_mgmt_client_session_close(&session);
   return rc;
}

int kb_mgmt_client_request(const char *ep, const char *ca, const char *cc, const char *ck,
                           const char *m, const char *path, const char *b, char *r, size_t n,
                           int *st)
{
   return kb_mgmt_client_request_auth(ep, ca, cc, ck, m, path, b, NULL, r, n, st);
}
