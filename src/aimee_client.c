/* aimee_client.c: transport-agnostic client to aimee-server's /v1 HTTP API.
 *
 * See headers/aimee_client.h for the contract. This file contains no socket
 * headers — the TCP path goes through platform_net (posix/ + windows/), and the
 * local UDS path delegates to http_uds_request (POSIX only). It therefore builds
 * identically on Linux, macOS, and Windows.
 */
#include "aimee_client.h"
#include "platform.h"
#include "platform_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AIMEE_POSIX
#include "http_uds_client.h"
#endif

#ifdef WITH_TLS
#include "aimee_tls.h"
#else
/* No TLS in this build: the handle type still exists (always NULL) so the I/O
 * helpers below compile unchanged; https:// is refused in tcp_request. */
typedef void aimee_tls_t;
#endif

/* Remote target configured for this process. Set once at startup (e.g. after
 * parsing a `--server` flag) before any request is issued, so plain statics
 * suffice — aimee_client_set_remote is documented as not thread-safe. */
static char g_remote_url[512];
static char g_remote_token[256];

void aimee_client_set_remote(const char *url, const char *token)
{
   if (url && *url)
      snprintf(g_remote_url, sizeof(g_remote_url), "%s", url);
   else
      g_remote_url[0] = '\0';
   if (token && *token)
      snprintf(g_remote_token, sizeof(g_remote_token), "%s", token);
   else
      g_remote_token[0] = '\0';
}

/* True when a remote aimee-server target is configured for this process — either
 * programmatically (aimee_client_set_remote, e.g. from --server) or via
 * AIMEE_SERVER_URL. Mirrors resolve_remote()'s precedence. On Windows (no UDS)
 * the dispatcher uses this to skip the local-socket preflight. */
int aimee_client_has_remote(void)
{
   if (g_remote_url[0])
      return 1;
   const char *env = getenv("AIMEE_SERVER_URL");
   return (env && *env) ? 1 : 0;
}

int aimee_client_parse_flag(char **argv, int *i, int argc, const char **url, const char **token)
{
   const char *a = argv[*i];
   if (strncmp(a, "--server=", 9) == 0)
   {
      *url = a + 9;
      return 1;
   }
   if (strcmp(a, "--server") == 0 && *i + 1 < argc)
   {
      *url = argv[++(*i)];
      return 1;
   }
   if (strncmp(a, "--server-token=", 15) == 0)
   {
      *token = a + 15;
      return 1;
   }
   return 0;
}

void aimee_client_apply_override(const char *url, const char *token)
{
   if (!url && !token)
      return;
   if (!url)
      url = getenv("AIMEE_SERVER_URL");
   if (!token)
      token = getenv("AIMEE_SERVER_TOKEN");
   aimee_client_set_remote(url, token);
}

/* Resolve the effective remote URL + token: explicit set_remote wins, else env.
 * Returns 1 if a remote is in effect (url_out filled), 0 if none. */
static int resolve_remote(char *url_out, size_t url_sz, char *tok_out, size_t tok_sz)
{
   if (g_remote_url[0])
   {
      snprintf(url_out, url_sz, "%s", g_remote_url);
      snprintf(tok_out, tok_sz, "%s", g_remote_token);
      return 1;
   }
   const char *env = getenv("AIMEE_SERVER_URL");
   if (env && *env)
   {
      snprintf(url_out, url_sz, "%s", env);
      const char *t = getenv("AIMEE_SERVER_TOKEN");
      snprintf(tok_out, tok_sz, "%s", (t && *t) ? t : "");
      return 1;
   }
   return 0;
}

/* Parse "http[s]://host[:port]" (or bare "host[:port]"). Fills host and port
 * (decimal string); *is_https set to 1 for https. Returns 0 on success. */
static int parse_url(const char *url, char *host, size_t host_sz, char *port, size_t port_sz,
                     int *is_https)
{
   *is_https = 0;
   const char *p = url;
   if (strncmp(p, "https://", 8) == 0)
   {
      *is_https = 1;
      p += 8;
   }
   else if (strncmp(p, "http://", 7) == 0)
   {
      p += 7;
   }

   /* host part ends at ':', '/', or end-of-string */
   const char *colon = NULL;
   const char *slash = NULL;
   for (const char *q = p; *q; q++)
   {
      if (*q == ':' && !colon && !slash)
         colon = q;
      else if (*q == '/')
      {
         slash = q;
         break;
      }
   }
   const char *host_end = colon ? colon : (slash ? slash : p + strlen(p));
   size_t hlen = (size_t)(host_end - p);
   if (hlen == 0 || hlen >= host_sz)
      return -1;
   memcpy(host, p, hlen);
   host[hlen] = '\0';

   if (colon)
   {
      const char *pe = slash ? slash : colon + 1 + strlen(colon + 1);
      size_t plen = (size_t)(pe - (colon + 1));
      if (plen == 0 || plen >= port_sz)
         return -1;
      memcpy(port, colon + 1, plen);
      port[plen] = '\0';
   }
   else
   {
      snprintf(port, port_sz, "%s", *is_https ? "443" : "80");
   }
   return 0;
}

/* Read a full HTTP response (Connection: close) from |fd| into a heap buffer.
 * Returns the buffer (caller frees) and sets *out_len, or NULL on alloc error. */
/* Transport I/O over either a plain socket (tls == NULL) or a TLS session. */
static int io_write_all(int fd, aimee_tls_t *tls, const void *buf, size_t len)
{
#ifdef WITH_TLS
   if (tls)
      return aimee_tls_write_all(tls, buf, len);
#endif
   (void)tls;
   return platform_net_send_all(fd, buf, len);
}

static long io_read(int fd, aimee_tls_t *tls, void *buf, size_t len)
{
#ifdef WITH_TLS
   if (tls)
      return aimee_tls_read(tls, buf, len);
#endif
   (void)tls;
   return platform_net_recv(fd, buf, len);
}

static char *read_response(int fd, aimee_tls_t *tls, size_t *out_len)
{
   size_t cap = 8192, len = 0;
   char *resp = malloc(cap);
   if (!resp)
      return NULL;
   for (;;)
   {
      if (len + 4096 + 1 > cap)
      {
         cap *= 2;
         char *grown = realloc(resp, cap);
         if (!grown)
         {
            free(resp);
            return NULL;
         }
         resp = grown;
      }
      long n = io_read(fd, tls, resp + len, 4096);
      if (n <= 0)
         break;
      len += (size_t)n;
   }
   resp[len] = '\0';
   if (out_len)
      *out_len = len;
   return resp;
}

static char *tcp_request(const char *url, const char *token, const char *method, const char *path,
                         const char *body, int *status_out)
{
   char host[256], port[16];
   int is_https = 0;
   if (parse_url(url, host, sizeof(host), port, sizeof(port), &is_https) != 0)
   {
      fprintf(stderr, "aimee: invalid server URL: %s\n", url);
      return NULL;
   }
#ifndef WITH_TLS
   if (is_https)
   {
      /* This build has no TLS: front remote servers with a TLS-terminating
       * proxy and point at its http:// address. */
      fprintf(stderr, "aimee: https:// is not supported in this build; use http:// "
                      "behind a TLS-terminating proxy\n");
      return NULL;
   }
#endif

   int fd = platform_net_connect(host, port, 10000);
   if (fd < 0)
      return NULL;

   aimee_tls_t *tls = NULL;
#ifdef WITH_TLS
   if (is_https)
   {
      tls = aimee_tls_connect(fd, host);
      if (!tls)
      {
         fprintf(stderr, "aimee: TLS handshake with %s failed\n", host);
         platform_net_close(fd);
         return NULL;
      }
   }
#endif

   int blen = body ? (int)strlen(body) : 0;
   char head[1024];
   int hlen;
   if (token && *token)
      hlen = snprintf(head, sizeof(head),
                      "%s %s HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\n"
                      "Content-Type: application/json\r\nContent-Length: %d\r\n"
                      "Connection: close\r\n\r\n",
                      method, path, host, token, blen);
   else
      hlen = snprintf(head, sizeof(head),
                      "%s %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
                      "Content-Length: %d\r\nConnection: close\r\n\r\n",
                      method, path, host, blen);
   if (hlen <= 0 || hlen >= (int)sizeof(head))
   {
      platform_net_close(fd);
      return NULL;
   }

   if (io_write_all(fd, tls, head, (size_t)hlen) != 0 ||
       (blen > 0 && io_write_all(fd, tls, body, (size_t)blen) != 0))
   {
#ifdef WITH_TLS
      if (tls)
         aimee_tls_free(tls);
#endif
      platform_net_close(fd);
      return NULL;
   }

   char *resp = read_response(fd, tls, NULL);
#ifdef WITH_TLS
   if (tls)
      aimee_tls_free(tls);
#endif
   platform_net_close(fd);
   if (!resp)
      return NULL;

   int status = 0;
   if (sscanf(resp, "HTTP/1.%*d %d", &status) != 1)
   {
      free(resp);
      return NULL;
   }
   if (status_out)
      *status_out = status;
   char *bstart = strstr(resp, "\r\n\r\n");
   char *out = bstart ? strdup(bstart + 4) : strdup("");
   free(resp);
   return out;
}

int aimee_client_remote_active_scheme(char *desc_out, unsigned long desc_sz, int *is_https_out)
{
   if (is_https_out)
      *is_https_out = 0;
   char url[512], tok[256];
   if (!resolve_remote(url, sizeof(url), tok, sizeof(tok)))
      return 0;
   if (desc_out && desc_sz)
   {
      char host[256], port[16];
      int is_https = 0;
      if (parse_url(url, host, sizeof(host), port, sizeof(port), &is_https) == 0)
      {
         snprintf(desc_out, desc_sz, "%s:%s", host, port);
         if (is_https_out)
            *is_https_out = is_https;
      }
      else
         snprintf(desc_out, desc_sz, "%s", url);
   }
   return 1;
}

int aimee_client_remote_active(char *desc_out, unsigned long desc_sz)
{
   return aimee_client_remote_active_scheme(desc_out, desc_sz, NULL);
}

int aimee_client_remote_token(char *tok_out, unsigned long tok_sz)
{
   char url[512], tok[256];
   if (!resolve_remote(url, sizeof(url), tok, sizeof(tok)))
      return 0;
   if (tok_out && tok_sz)
      snprintf(tok_out, tok_sz, "%s", tok);
   return 1;
}

char *aimee_client_request(const char *method, const char *path, const char *body, int *status_out)
{
   if (status_out)
      *status_out = 0;
   if (!method || !path)
      return NULL;

   char url[512], tok[256];
   if (resolve_remote(url, sizeof(url), tok, sizeof(tok)))
      return tcp_request(url, tok, method, path, body, status_out);

#ifdef AIMEE_POSIX
   /* Default local transport: HTTP over the server's Unix domain socket. */
   return http_uds_request(method, path, body, status_out);
#else
   /* Windows has no UDS default — a remote target is required for connectivity. */
   fprintf(stderr, "aimee: no server configured; set AIMEE_SERVER_URL or use --server\n");
   return NULL;
#endif
}
