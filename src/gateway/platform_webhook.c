/* platform_webhook.c: inbound HTTP webhook adapter.
 *
 * Listens on a configurable TCP port, verifies HMAC-SHA256 signatures,
 * dispatches inbound JSON payloads to gateway_handle_message, and sends
 * outbound text via HTTP POST to the delivery target URL (chat_id).
 *
 * Config (environment variables):
 *   AIMEE_GATEWAY_WEBHOOK_PORT    TCP port to listen on (default: 9080)
 *   AIMEE_GATEWAY_WEBHOOK_SECRET  HMAC secret (required unless INSECURE=true)
 *   AIMEE_GATEWAY_WEBHOOK_INSECURE if "true", skip HMAC verification (testing only)
 */
#include "platform_webhook.h"
#include "gateway_ctx.h"
#include "aimee.h"
#include "agent_exec.h"
#include "log.h"
#include <cJSON.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define WEBHOOK_MAX_BODY 65536
#define WEBHOOK_BACKLOG  8

typedef struct webhook_state
{
   gateway_ctx_t *ctx;
   int listen_fd;
   int stop;
   pthread_t thread;
} webhook_state_t;

static webhook_state_t s_state;

/* ---- HMAC-SHA256 verification ------------------------------------------ */

/* Compute HMAC-SHA256 of body with secret and compare to expected_hex.
 * Uses constant-time XOR comparison to prevent timing side-channels.
 * Returns 0 if signatures match, -1 otherwise. */
static int hmac_verify(const char *secret, const char *body, size_t body_len,
                       const char *expected_hex)
{
   if (!secret || !secret[0] || !body || !expected_hex)
      return -1;

   unsigned char digest[32];
   unsigned int digest_len = 0;
   if (!HMAC(EVP_sha256(), secret, (int)strlen(secret), (const unsigned char *)body, body_len,
             digest, &digest_len))
   {
      aimee_log(LOG_WARN, "webhook", "HMAC computation failed");
      return -1;
   }

   /* Hex-encode the digest */
   char computed[65];
   for (unsigned int i = 0; i < digest_len; i++)
      snprintf(computed + i * 2, 3, "%02x", digest[i]);
   computed[digest_len * 2] = '\0';

   /* Constant-time comparison: XOR all bytes, OR the differences */
   int mismatch = 0;
   size_t exp_len = strlen(expected_hex);
   if (exp_len != (size_t)(digest_len * 2))
      return -1;
   for (size_t i = 0; i < exp_len; i++)
      mismatch |= ((unsigned char)computed[i] ^ (unsigned char)expected_hex[i]);
   return mismatch ? -1 : 0;
}

/* ---- HTTP helpers -------------------------------------------------------- */

static int read_http_request(int fd, char *out_buf, size_t out_cap, size_t *body_start,
                             size_t *body_len, char *sig_out, size_t sig_cap)
{
   size_t total = 0;
   while (total + 1 < out_cap)
   {
      ssize_t n = read(fd, out_buf + total, out_cap - total - 1);
      if (n <= 0)
         break;
      total += (size_t)n;
      out_buf[total] = '\0';
      /* Detect end of headers (double CRLF). */
      char *hdr_end = strstr(out_buf, "\r\n\r\n");
      if (hdr_end)
      {
         *body_start = (size_t)(hdr_end - out_buf) + 4;
         /* Parse Content-Length. */
         char *cl = strstr(out_buf, "Content-Length: ");
         *body_len = 0;
         if (cl)
            *body_len = (size_t)atol(cl + 16);
         /* Extract X-Aimee-Signature header if present. */
         char *sig = strstr(out_buf, "X-Aimee-Signature: sha256=");
         if (sig && sig_out && sig_cap > 0)
         {
            sig += strlen("X-Aimee-Signature: sha256=");
            size_t i = 0;
            while (i + 1 < sig_cap && sig[i] && sig[i] != '\r' && sig[i] != '\n')
            {
               sig_out[i] = sig[i];
               i++;
            }
            sig_out[i] = '\0';
         }
         /* Read the body if not yet fully received. Content-Length is
          * attacker-controlled, so the per-read length MUST be clamped to the
          * remaining buffer capacity — never to (*body_len - have_body), which a
          * malicious huge Content-Length would make far larger than out_buf,
          * letting a single read() overflow the heap allocation (one NUL byte is
          * reserved at out_buf[total]). */
         size_t have_body = total - *body_start;
         while (have_body < *body_len && total + 1 < out_cap)
         {
            size_t want = *body_len - have_body;
            size_t room = out_cap - total - 1;
            if (want > room)
               want = room;
            if (want == 0)
               break;
            ssize_t m = read(fd, out_buf + total, want);
            if (m <= 0)
               break;
            total += (size_t)m;
            have_body += (size_t)m;
         }
         out_buf[total] = '\0';
         return 0;
      }
   }
   return -1;
}

static void send_response(int fd, int status, const char *body)
{
   char resp[256];
   size_t body_len = body ? strlen(body) : 0;
   int n = snprintf(resp, sizeof(resp),
                    "HTTP/1.0 %d OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n\r\n%s",
                    status, body_len, body ? body : "");
   (void)write(fd, resp, (size_t)n);
}

/* ---- accept loop --------------------------------------------------------- */

static void *accept_loop(void *arg)
{
   webhook_state_t *ws = (webhook_state_t *)arg;
   const char *insecure = getenv("AIMEE_GATEWAY_WEBHOOK_INSECURE");
   const char *secret = getenv("AIMEE_GATEWAY_WEBHOOK_SECRET");
   const char *deliver_only_env = getenv("AIMEE_GATEWAY_WEBHOOK_DELIVER_ONLY");
   int is_insecure = insecure && strcmp(insecure, "true") == 0;
   /* deliver_only=true: accept inbound webhooks, validate HMAC, but route
    * to outbound delivery only — do not dispatch to the agent. */
   int deliver_only = deliver_only_env && strcmp(deliver_only_env, "true") == 0;

   while (!ws->stop)
   {
      struct sockaddr_in client_addr;
      socklen_t addr_len = sizeof(client_addr);
      int client_fd = accept(ws->listen_fd, (struct sockaddr *)&client_addr, &addr_len);
      if (client_fd < 0)
      {
         if (!ws->stop)
            aimee_log(LOG_WARN, "webhook", "accept failed: %s", strerror(errno));
         break;
      }

      char *buf = malloc(WEBHOOK_MAX_BODY + 1);
      if (!buf)
      {
         close(client_fd);
         continue;
      }
      buf[0] = '\0';

      size_t body_start = 0, body_len = 0;
      char sig_hex[128] = {0};
      int ok = read_http_request(client_fd, buf, WEBHOOK_MAX_BODY, &body_start, &body_len, sig_hex,
                                 sizeof(sig_hex));
      if (ok != 0 || body_len == 0)
      {
         send_response(client_fd, 400, "bad request");
         free(buf);
         close(client_fd);
         continue;
      }

      const char *body = buf + body_start;

      /* Verify HMAC if required. */
      if (!is_insecure && secret && secret[0])
      {
         if (sig_hex[0] == '\0' || hmac_verify(secret, body, body_len, sig_hex) != 0)
         {
            aimee_log(LOG_WARN, "webhook", "HMAC verification failed");
            send_response(client_fd, 403, "forbidden");
            free(buf);
            close(client_fd);
            continue;
         }
      }

      /* Parse JSON body. */
      cJSON *root = cJSON_ParseWithLength(body, body_len);
      if (!root)
      {
         aimee_log(LOG_WARN, "webhook", "JSON parse failed");
         send_response(client_fd, 400, "bad json");
         free(buf);
         close(client_fd);
         continue;
      }

      const char *text = NULL;
      cJSON *j_text = cJSON_GetObjectItem(root, "text");
      if (j_text && cJSON_IsString(j_text))
         text = j_text->valuestring;

      session_source_t src;
      memset(&src, 0, sizeof(src));
      snprintf(src.platform, sizeof(src.platform), "webhook");

      cJSON *j_platform = cJSON_GetObjectItem(root, "platform");
      if (j_platform && cJSON_IsString(j_platform))
         snprintf(src.platform, sizeof(src.platform), "%s", j_platform->valuestring);

      cJSON *j_chat_id = cJSON_GetObjectItem(root, "chat_id");
      if (j_chat_id && cJSON_IsString(j_chat_id))
         snprintf(src.chat_id, sizeof(src.chat_id), "%s", j_chat_id->valuestring);

      cJSON *j_user_id = cJSON_GetObjectItem(root, "user_id");
      if (j_user_id && cJSON_IsString(j_user_id))
         snprintf(src.user_id, sizeof(src.user_id), "%s", j_user_id->valuestring);

      snprintf(src.chat_type, sizeof(src.chat_type), "webhook");

      cJSON_Delete(root);

      if (text && text[0] && !deliver_only)
         gateway_handle_message(ws->ctx, &src, text, NULL, 0);

      send_response(client_fd, 200, "ok");
      free(buf);
      close(client_fd);
   }
   return NULL;
}

/* ---- adapter callbacks --------------------------------------------------- */

static int webhook_check_config(platform_adapter_t *self, char *err_out, size_t err_len)
{
   (void)self;
   const char *secret = getenv("AIMEE_GATEWAY_WEBHOOK_SECRET");
   const char *insecure = getenv("AIMEE_GATEWAY_WEBHOOK_INSECURE");
   if ((!secret || !secret[0]) && !(insecure && strcmp(insecure, "true") == 0))
   {
      if (err_out && err_len > 0)
         snprintf(err_out, err_len,
                  "AIMEE_GATEWAY_WEBHOOK_SECRET is required (or set "
                  "AIMEE_GATEWAY_WEBHOOK_INSECURE=true)");
      return -1;
   }
   if (insecure && strcmp(insecure, "true") == 0)
      aimee_log(LOG_WARN, "webhook", "HMAC verification disabled — testing mode only");
   return 0;
}

static int webhook_startup(platform_adapter_t *self, gateway_ctx_t *ctx)
{
   (void)self;
   const char *port_str = getenv("AIMEE_GATEWAY_WEBHOOK_PORT");
   int port = port_str ? atoi(port_str) : 9080;
   if (port <= 0 || port > 65535)
      port = 9080;

   int fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0)
   {
      aimee_log(LOG_ERROR, "webhook", "socket: %s", strerror(errno));
      return -1;
   }

   int opt = 1;
   setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

   struct sockaddr_in addr;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   addr.sin_port = htons((uint16_t)port);

   if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
   {
      aimee_log(LOG_ERROR, "webhook", "bind port %d: %s", port, strerror(errno));
      close(fd);
      return -1;
   }

   if (listen(fd, WEBHOOK_BACKLOG) != 0)
   {
      aimee_log(LOG_ERROR, "webhook", "listen: %s", strerror(errno));
      close(fd);
      return -1;
   }

   s_state.ctx = ctx;
   s_state.listen_fd = fd;
   s_state.stop = 0;

   if (pthread_create(&s_state.thread, NULL, accept_loop, &s_state) != 0)
   {
      aimee_log(LOG_ERROR, "webhook", "pthread_create: %s", strerror(errno));
      close(fd);
      return -1;
   }

   aimee_log(LOG_INFO, "webhook", "listening on 127.0.0.1:%d", port);
   return 0;
}

static void webhook_shutdown(platform_adapter_t *self)
{
   (void)self;
   s_state.stop = 1;
   if (s_state.listen_fd >= 0)
   {
      close(s_state.listen_fd);
      s_state.listen_fd = -1;
   }
   pthread_join(s_state.thread, NULL);
}

static int webhook_send_text(platform_adapter_t *self, const delivery_target_t *target,
                             const char *text)
{
   (void)self;
   if (!target || !target->chat_id[0] || !text)
      return -1;
   /* chat_id is treated as the callback URL for outbound delivery. */
   char *response = NULL;
   int status = agent_http_post(target->chat_id, NULL, text, &response, 10000, NULL);
   free(response);
   return (status >= 200 && status < 300) ? 0 : -1;
}

static int webhook_send_attachment(platform_adapter_t *self, const delivery_target_t *target,
                                   const char *path, const char *mime)
{
   (void)self;
   (void)path;
   (void)mime;
   if (!target || !target->chat_id[0])
      return -1;
   aimee_log(LOG_WARN, "webhook", "attachment delivery not supported");
   return -1;
}

static int webhook_authorize_user(platform_adapter_t *self, const char *platform,
                                  const char *chat_id, const char *user_id)
{
   (void)self;
   (void)platform;
   (void)chat_id;
   (void)user_id;
   return 0;
}

static int webhook_set_typing(platform_adapter_t *self, const delivery_target_t *target, int typing)
{
   (void)self;
   (void)target;
   (void)typing;
   return 0;
}

static platform_adapter_t s_webhook_adapter = {
    .name = "webhook",
    .display_name = "Webhook",
    .enabled = 0,
    .startup = webhook_startup,
    .shutdown = webhook_shutdown,
    .check_config = webhook_check_config,
    .send_text = webhook_send_text,
    .send_attachment = webhook_send_attachment,
    .authorize_user = webhook_authorize_user,
    .set_typing = webhook_set_typing,
    .user = NULL,
};

platform_adapter_t *webhook_adapter_get(void)
{
   return &s_webhook_adapter;
}
