/* kb_oidc_token_exchange_post.c — the one network step of the OIDC login.
 *
 * Separate from kb_oidc_token_exchange.c so that linking the codec does not drag
 * in TLS and the HTTP client: the codec is where the security-relevant decisions
 * live and it stays testable without either. This file adds no decisions — it
 * assembles the request the codec built, hands it to kb_http_tls_exchange, and
 * hands the body back to the codec to read. See kb_oidc_token_exchange.h. */

#include "kb_oidc_token_exchange.h"

#include "http/kb_http_client.h"
#include "log.h"

#include <openssl/crypto.h> /* OPENSSL_cleanse */
#include <string.h>

/* Connect and total budgets for the token exchange. A user is waiting on a
 * browser redirect, so a hung IdP has to fail rather than hold the request
 * thread: the total is well under the pending login's TTL, which means an
 * exchange can never outlive the login it belongs to. */
#define TOKEN_CONNECT_TIMEOUT_MS 5000
#define TOKEN_TOTAL_TIMEOUT_MS   15000

typedef struct
{
   int status;
   char body[KB_OIDC_TOKEN_EXCHANGE_RESPONSE_MAX];
   size_t body_len;
   int overflow;
} collect_t;

static kb_http_gate_t on_headers(const kb_http_response_t *response, void *context)
{
   collect_t *c = context;
   c->status = response ? response->status : 0;
   /* The body is wanted whatever the status: an OAuth error response carries the
    * reason, and a 400 with a readable `error` is more useful than a bare code. */
   return KB_HTTP_GATE_DELIVER;
}

static kb_http_body_action_t on_body(const unsigned char *bytes, size_t length, void *context)
{
   collect_t *c = context;
   if (length > sizeof(c->body) - c->body_len)
   {
      /* Refuse rather than keep a prefix: half a JSON document would parse as
       * malformed anyway, and truncating is how a "surprisingly missing
       * id_token" bug gets written. */
      c->overflow = 1;
      return KB_HTTP_BODY_CALLER_ABORT;
   }
   memcpy(c->body + c->body_len, bytes, length);
   c->body_len += length;
   return KB_HTTP_BODY_CONTINUE;
}

kb_oidc_token_exchange_result_t
kb_oidc_token_exchange_post(const kb_oidc_login_config_t *cfg,
                            const kb_oidc_login_pending_t *pending, const char *code,
                            const char *client_secret, char *unverified_id_token_out, size_t cap)
{
   if (unverified_id_token_out && cap)
      unverified_id_token_out[0] = '\0';
   if (!cfg || !pending || !unverified_id_token_out || cap == 0 || !kb_oidc_login_config_valid(cfg))
      return KB_OIDC_TOKEN_EXCHANGE_INVALID;

   char host[256] = "", path[512] = "";
   if (kb_oidc_token_url_split(cfg->token_url, host, sizeof(host), path, sizeof(path)) !=
       KB_OIDC_LOGIN_OK)
      return KB_OIDC_TOKEN_EXCHANGE_INVALID;

   char body[KB_OIDC_TOKEN_EXCHANGE_BODY_MAX] = "";
   char basic[2400] = "";
   kb_oidc_token_exchange_result_t rc =
       kb_oidc_token_request_body(pending, code, cfg->client_id, body, sizeof(body));
   if (rc == KB_OIDC_TOKEN_EXCHANGE_OK)
      rc = kb_oidc_token_basic_auth(cfg->client_id, client_secret, basic, sizeof(basic));
   if (rc != KB_OIDC_TOKEN_EXCHANGE_OK)
      goto done;

   {
      const kb_http_header_t headers[] = {
          {"host", host},
          {"authorization", basic},
          {"content-type", "application/x-www-form-urlencoded"},
          /* RFC 6749 §5.1 says a token response must not be cached; saying so on
           * the request keeps an intermediary from serving a stale one. */
          {"cache-control", "no-store"},
          {"accept", "application/json"},
      };
      const kb_http_request_t request = {
          .authority = host,
          .method = "POST",
          .target = path,
          .headers = headers,
          .header_count = sizeof(headers) / sizeof(headers[0]),
          .body = (const unsigned char *)body,
          .body_len = strlen(body),
          .response_body_max = KB_OIDC_TOKEN_EXCHANGE_RESPONSE_MAX,
          .connect_timeout_ms = TOKEN_CONNECT_TIMEOUT_MS,
          .total_timeout_ms = TOKEN_TOTAL_TIMEOUT_MS,
          /* RFC 6749 §5.2: the token endpoint reports failure as a 400 whose BODY
           * carries {"error":"invalid_grant", ...}. Without this the parser discards
           * every non-2xx body, so _DENIED was unreachable and every IdP refusal —
           * a spent code, a wrong verifier — surfaced as _MALFORMED. Found by running
           * against a real identity provider; no stub could show it. */
          .deliver_error_body = 1,
      };
      collect_t collected;
      memset(&collected, 0, sizeof(collected));
      kb_http_response_t response;
      memset(&response, 0, sizeof(response));
      kb_http_result_t transport =
          kb_http_tls_exchange(&request, &response, on_headers, on_body, &collected);
      if (transport != KB_HTTP_OK || collected.overflow)
      {
         /* Never log the URL with the request on it, and never the status alone
          * as if it were the outcome — the caller decides what the user sees. */
         LOG_INFO("kb.oidc.login", "token exchange transport failed (host=%s rc=%d)", host,
                  (int)transport);
         rc = KB_OIDC_TOKEN_EXCHANGE_UNAVAILABLE;
         OPENSSL_cleanse(&collected, sizeof(collected));
         goto done;
      }
      rc = kb_oidc_token_response_id_token(collected.body, collected.body_len,
                                           unverified_id_token_out, cap);
      /* A non-2xx that nonetheless parsed as a token response is still a
       * refusal: the id_token in it was not issued by a successful exchange. */
      if (collected.status < 200 || collected.status > 299)
         rc = rc == KB_OIDC_TOKEN_EXCHANGE_DENIED ? KB_OIDC_TOKEN_EXCHANGE_DENIED
                                                  : KB_OIDC_TOKEN_EXCHANGE_MALFORMED;
      OPENSSL_cleanse(&collected, sizeof(collected));
   }

done:
   /* `body` carried the code and the verifier; `basic` carried the client secret.
    * Neither may survive this call on either path. */
   OPENSSL_cleanse(body, sizeof(body));
   OPENSSL_cleanse(basic, sizeof(basic));
   if (rc != KB_OIDC_TOKEN_EXCHANGE_OK && cap)
      unverified_id_token_out[0] = '\0';
   return rc;
}
