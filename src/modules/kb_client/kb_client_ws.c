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
#include "runtime_secret.h"
#include <aimee/core/connection/auth.h>
#include <aimee/core/connection/endpoint.h>
#include <aimee/core/connection/socket.h>
#include <aimee/core/connection/tls_openssl.h>

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
      aimee_core_tls_session_free(c->ssl);
      c->ssl = NULL;
   }
   if (c->ctx)
   {
      SSL_CTX_free(c->ctx);
      c->ctx = NULL;
   }
   if (c->fd >= 0)
   {
      aimee_core_socket_close(c->fd);
      c->fd = -1;
   }
}

static int conn_write_all(ws_conn_t *c, const char *buf, size_t len)
{
   return c->ssl ? aimee_core_tls_write_all(c->ssl, buf, len)
                 : aimee_core_socket_write_all(c->fd, buf, len);
}

static int conn_read(ws_conn_t *c, void *buf, int len)
{
   long result = c->ssl ? aimee_core_tls_read(c->ssl, buf, (size_t)len)
                        : aimee_core_socket_read(c->fd, buf, (size_t)len);
   return result > INT32_MAX ? -1 : (int)result;
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

/* Open + WS-handshake GET /v1/events. Returns 0 on success (conn filled). */
static int ws_open(const char *base_url, ws_conn_t *c)
{
   memset(c, 0, sizeof(*c));
   c->fd = -1;
   aimee_core_endpoint_t endpoint;
   if (aimee_core_endpoint_parse(base_url, &endpoint) != 0)
      return -1;
   int port = atoi(endpoint.port);
   if (port <= 0 || port > 65535)
      return -1;
   const char *host = endpoint.host;
   int is_tls = endpoint.secure;

   c->fd = aimee_core_socket_connect(host, endpoint.port, 10000);
   if (c->fd < 0)
      return -1;

   if (is_tls)
   {
      c->ctx = aimee_core_tls_client_context();
      if (!c->ctx)
      {
         conn_close(c);
         return -1;
      }
      SSL_CTX_set_default_verify_paths(c->ctx);
      SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, NULL);
      const char *ca = getenv("AIMEE_KB_API_CA_BUNDLE");
      if (ca && ca[0] && aimee_core_tls_trust_file(c->ctx, ca) != 0)
      {
         conn_close(c);
         return -1;
      }
      c->ssl = aimee_core_tls_client_session_new(c->ctx, c->fd, host, 1);
      if (!c->ssl)
      {
         conn_close(c);
         return -1;
      }
      if (aimee_core_tls_handshake_client(c->ssl) != 0 ||
          SSL_get_verify_result(c->ssl) != X509_V_OK)
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

   char tok[512] = "";
   (void)runtime_secret_get("AIMEE_KB_API_BEARER_TOKEN", tok, sizeof(tok));
   if (aimee_core_would_leak_credential(is_tls, host, tok))
   {
      runtime_secret_wipe(tok, sizeof(tok));
      conn_close(c);
      return -1;
   }
   char authorization[sizeof(tok) + 16] = "";
   if (tok[0] && aimee_core_bearer_value(authorization, sizeof(authorization), tok) != 0)
   {
      runtime_secret_wipe(tok, sizeof(tok));
      conn_close(c);
      return -1;
   }
   char req[768];
   int rn = snprintf(req, sizeof(req),
                     "GET /v1/events HTTP/1.1\r\nHost: %s:%d\r\n"
                     "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n%s%s%s\r\n",
                     host, port, key, authorization[0] ? "Authorization: " : "", authorization,
                     authorization[0] ? "\r\n" : "");
   runtime_secret_wipe(tok, sizeof(tok));
   if (rn <= 0 || (size_t)rn >= sizeof(req) || conn_write_all(c, req, (size_t)rn) != 0)
   {
      runtime_secret_wipe(req, sizeof(req));
      conn_close(c);
      return -1;
   }
   runtime_secret_wipe(req, sizeof(req));

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
