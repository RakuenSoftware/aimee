/* kb_client_ws.c: aimee-server's /v1/events WebSocket subscriber. See header.
 *
 * Connects to the remote kb (AIMEE_KB_API_URL), upgrades GET /v1/events to a
 * WebSocket, and on each {"type":"invalidation"} frame flushes the kb result
 * cache. Reconnects with capped backoff. http:// uses a plain socket; https://
 * uses OpenSSL with system-CA verification (mirroring agent_bridge). */
#include "kb_client_cache.h"
#include "kb_client_internal.h"
#include "kb_client_ws.h"
#include "log.h"

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

typedef struct
{
   int fd;
   SSL *ssl;
   SSL_CTX *ctx;
} ws_conn_t;

static void conn_close(ws_conn_t *c)
{
   if (c->ssl)
   {
      SSL_shutdown(c->ssl);
      SSL_free(c->ssl);
      c->ssl = NULL;
   }
   if (c->ctx)
   {
      SSL_CTX_free(c->ctx);
      c->ctx = NULL;
   }
   if (c->fd >= 0)
   {
      close(c->fd);
      c->fd = -1;
   }
}

static int conn_write(ws_conn_t *c, const char *buf, int len)
{
   return c->ssl ? SSL_write(c->ssl, buf, len) : (int)write(c->fd, buf, (size_t)len);
}

static int conn_read(ws_conn_t *c, void *buf, int len)
{
   return c->ssl ? SSL_read(c->ssl, buf, len) : (int)read(c->fd, buf, (size_t)len);
}

static int read_n(ws_conn_t *c, unsigned char *buf, size_t n)
{
   size_t got = 0;
   while (got < n)
   {
      int r = conn_read(c, buf + got, (int)(n - got));
      if (r <= 0)
         return 0;
      got += (size_t)r;
   }
   return 1;
}

/* Parse scheme://host[:port] (ignores any path). Returns 0 on success. */
static int parse_url(const char *url, int *is_tls, char *host, size_t host_cap, int *port)
{
   if (!url)
      return -1;
   *is_tls = 0;
   const char *p = url;
   if (strncmp(p, "https://", 8) == 0)
   {
      *is_tls = 1;
      *port = 443;
      p += 8;
   }
   else if (strncmp(p, "http://", 7) == 0)
   {
      *port = 80;
      p += 7;
   }
   else
      return -1;
   const char *end = p;
   while (*end && *end != '/' && *end != ':')
      end++;
   size_t hlen = (size_t)(end - p);
   if (hlen == 0 || hlen >= host_cap)
      return -1;
   memcpy(host, p, hlen);
   host[hlen] = '\0';
   if (*end == ':')
      *port = atoi(end + 1);
   return *port > 0 ? 0 : -1;
}

static int tcp_connect(const char *host, int port)
{
   char ports[16];
   snprintf(ports, sizeof(ports), "%d", port);
   struct addrinfo hints, *res = NULL;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   if (getaddrinfo(host, ports, &hints, &res) != 0 || !res)
      return -1;
   int fd = -1;
   for (struct addrinfo *ai = res; ai; ai = ai->ai_next)
   {
      fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd < 0)
         continue;
      if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
         break;
      close(fd);
      fd = -1;
   }
   freeaddrinfo(res);
   return fd;
}

/* Open + WS-handshake GET /v1/events. Returns 0 on success (conn filled). */
static int ws_open(const char *base_url, ws_conn_t *c)
{
   memset(c, 0, sizeof(*c));
   c->fd = -1;
   int is_tls, port;
   char host[256];
   if (parse_url(base_url, &is_tls, host, sizeof(host), &port) != 0)
      return -1;

   c->fd = tcp_connect(host, port);
   if (c->fd < 0)
      return -1;

   if (is_tls)
   {
      c->ctx = SSL_CTX_new(TLS_client_method());
      if (!c->ctx)
      {
         conn_close(c);
         return -1;
      }
      SSL_CTX_set_default_verify_paths(c->ctx);
      SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, NULL);
      SSL_CTX_set_min_proto_version(c->ctx, TLS1_2_VERSION);
      const char *ca = getenv("AIMEE_KB_API_CA_BUNDLE");
      if (ca && ca[0])
         SSL_CTX_load_verify_file(c->ctx, ca);
      c->ssl = SSL_new(c->ctx);
      if (!c->ssl)
      {
         conn_close(c);
         return -1;
      }
      SSL_set_fd(c->ssl, c->fd);
      SSL_set_tlsext_host_name(c->ssl, host);
      X509_VERIFY_PARAM_set1_host(SSL_get0_param(c->ssl), host, 0);
      if (SSL_connect(c->ssl) != 1 || SSL_get_verify_result(c->ssl) != X509_V_OK)
      {
         conn_close(c);
         return -1;
      }
   }

   /* Random 16-byte Sec-WebSocket-Key, base64. */
   unsigned char rnd[16];
   if (RAND_bytes(rnd, sizeof(rnd)) != 1)
   {
      conn_close(c);
      return -1;
   }
   char key[32] = {0};
   EVP_EncodeBlock((unsigned char *)key, rnd, sizeof(rnd));

   const char *tok = getenv("AIMEE_KB_API_BEARER_TOKEN");
   char req[768];
   int rn = snprintf(req, sizeof(req),
                     "GET /v1/events HTTP/1.1\r\nHost: %s:%d\r\n"
                     "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n%s%s%s\r\n",
                     host, port, key, tok && tok[0] ? "Authorization: Bearer " : "",
                     tok && tok[0] ? tok : "", tok && tok[0] ? "\r\n" : "");
   if (conn_write(c, req, rn) != rn)
   {
      conn_close(c);
      return -1;
   }

   /* Read the handshake response headers; require 101. */
   char buf[1024];
   int total = 0;
   while (total < (int)sizeof(buf) - 1)
   {
      int r = conn_read(c, buf + total, (int)sizeof(buf) - 1 - total);
      if (r <= 0)
      {
         conn_close(c);
         return -1;
      }
      total += r;
      buf[total] = '\0';
      if (strstr(buf, "\r\n\r\n"))
         break;
   }
   if (!strstr(buf, " 101 "))
   {
      conn_close(c);
      return -1;
   }
   return 0;
}

/* Read one server frame's text payload into out. Returns >0 len, 0 on
 * close/eof. Control frames (close) return 0; pings are ignored-but-continue
 * via a -2 sentinel mapped to a retry by the caller. */
static int ws_read_text(ws_conn_t *c, char *out, int cap)
{
   for (;;)
   {
      unsigned char h[2];
      if (!read_n(c, h, 2))
         return 0;
      unsigned char opcode = h[0] & 0x0f;
      uint64_t len = h[1] & 0x7f; /* server frames are unmasked */
      if (len == 126)
      {
         unsigned char e[2];
         if (!read_n(c, e, 2))
            return 0;
         len = ((uint64_t)e[0] << 8) | e[1];
      }
      else if (len == 127)
      {
         unsigned char e[8];
         if (!read_n(c, e, 8))
            return 0;
         len = 0;
         for (int i = 0; i < 8; i++)
            len = (len << 8) | e[i];
      }
      if (len > (uint64_t)(cap - 1))
      {
         unsigned char drop[512];
         uint64_t rem = len;
         while (rem > 0)
         {
            size_t chunk = rem < sizeof(drop) ? (size_t)rem : sizeof(drop);
            if (!read_n(c, drop, chunk))
               return 0;
            rem -= chunk;
         }
         continue; /* skip oversized frame */
      }
      if (len > 0 && !read_n(c, (unsigned char *)out, (size_t)len))
         return 0;
      out[len] = '\0';
      if (opcode == 0x8) /* close */
         return 0;
      if (opcode == 0x9 || opcode == 0xA) /* ping/pong: ignore, keep reading */
         continue;
      return (int)len;
   }
}

static void *ws_thread(void *arg)
{
   (void)arg;
   int backoff = 1;
   for (;;)
   {
      const char *base = kb_client_v1_base_url();
      if (!base)
      {
         sleep(5);
         continue;
      }
      ws_conn_t c;
      if (ws_open(base, &c) != 0)
      {
         sleep(backoff);
         if (backoff < 30)
            backoff *= 2;
         continue;
      }
      aimee_log(LOG_INFO, "kbws.client", "subscribed to %s/v1/events", base);
      backoff = 1;
      char frame[2048];
      for (;;)
      {
         int n = ws_read_text(&c, frame, sizeof(frame));
         if (n <= 0)
            break; /* disconnected → reconnect */
         if (strstr(frame, "\"type\":\"invalidation\""))
            kb_cache_invalidate_all();
      }
      conn_close(&c);
      aimee_log(LOG_WARN, "kbws.client", "events stream dropped; reconnecting");
      sleep(backoff);
      if (backoff < 30)
         backoff *= 2;
   }
   return NULL;
}

static pthread_once_t g_started = PTHREAD_ONCE_INIT;

static void start_once(void)
{
   pthread_t t;
   if (pthread_create(&t, NULL, ws_thread, NULL) == 0)
      pthread_detach(t);
}

void kb_client_ws_start(void)
{
   if (!kb_cache_enabled())
      return;
   if (!kb_client_v1_base_url())
      return; /* only meaningful over the HTTP(S) transport */
   pthread_once(&g_started, start_once);
}
