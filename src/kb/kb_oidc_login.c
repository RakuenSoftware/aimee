/* kb_oidc_login.c — the pure relying-party core. See kb_oidc_login.h. */

#include "kb_oidc_login.h"

#include "kb_auth_oidc.h" /* kb_oidc_id_token_nonce */
#include "platform_random.h"

#include <openssl/crypto.h> /* OPENSSL_cleanse */
#include <stdio.h>
#include <stdlib.h> /* getenv */
#include <string.h>

static int hex_value(char c)
{
   if (c >= '0' && c <= '9')
      return c - '0';
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
   return -1;
}

/* No control bytes, no whitespace, within bounds. These land in a URL and in an
 * audit record, so a newline would be a log-injection primitive. */
static int plain_field(const char *s, size_t max, int required)
{
   if (!s)
      return !required;
   size_t n = strnlen(s, max + 1);
   if (n > max)
      return 0;
   if (n == 0)
      return !required;
   for (size_t i = 0; i < n; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      if (c <= 0x20 || c == 0x7f)
         return 0;
   }
   return 1;
}

/* The scope is the one field where a space is meaningful rather than suspicious:
 * OIDC scopes are a space-delimited list ("openid email profile"). Interior
 * spaces are allowed, control bytes still are not, and a leading, trailing or
 * doubled space is refused so the value that reaches the IdP is exactly the list
 * the operator wrote. */
static int scope_field(const char *s, size_t max)
{
   if (!s)
      return 1; /* optional */
   size_t n = strnlen(s, max + 1);
   if (n > max)
      return 0;
   if (n == 0)
      return 1;
   if (s[0] == ' ' || s[n - 1] == ' ')
      return 0;
   for (size_t i = 0; i < n; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      if (c < 0x20 || c == 0x7f)
         return 0;
      if (c == ' ' && s[i + 1] == ' ')
         return 0;
   }
   return 1;
}

/* Only https, with one exception: a loopback redirect_uri may be http, because
 * that is how a browser on the operator's own machine completes the flow and
 * there is no network to eavesdrop. Anything else is refused, so a profile
 * cannot quietly downgrade the IdP hop to cleartext. */
static int https_url(const char *s, size_t max, int allow_loopback_http)
{
   if (!plain_field(s, max, 1))
      return 0;
   if (!strncmp(s, "https://", 8))
      return s[8] != '\0';
   if (allow_loopback_http &&
       (!strncmp(s, "http://127.0.0.1", 16) || !strncmp(s, "http://localhost", 16) ||
        !strncmp(s, "http://[::1]", 12)))
      return 1;
   return 0;
}

int kb_oidc_login_config_valid(const kb_oidc_login_config_t *cfg)
{
   return cfg && plain_field(cfg->issuer, sizeof(cfg->issuer) - 1, 1) &&
          plain_field(cfg->client_id, sizeof(cfg->client_id) - 1, 1) &&
          https_url(cfg->authorize_url, sizeof(cfg->authorize_url) - 1, 0) &&
          /* The token endpoint is a server-to-server hop with the client secret
           * on it, so there is no loopback-http exception and it must be
           * splittable into the host/path the pinned-443 egress client takes. */
          https_url(cfg->token_url, sizeof(cfg->token_url) - 1, 0) &&
          kb_oidc_token_url_split(cfg->token_url, NULL, 0, NULL, 0) == KB_OIDC_LOGIN_OK &&
          https_url(cfg->redirect_uri, sizeof(cfg->redirect_uri) - 1, 1) &&
          scope_field(cfg->scope, sizeof(cfg->scope) - 1);
}

/* base64url(32 random bytes) — 43 chars, the RFC 7636 minimum verifier length,
 * and 256 bits of entropy for state and nonce too. */
static int draw_secret(char out[KB_OIDC_LOGIN_SECRET_LEN + 1])
{
   unsigned char raw[32];
   if (platform_random_bytes(raw, sizeof(raw)) != 0)
      return -1;
   char encoded[64];
   if (oauth_pkce_base64url_encode(raw, sizeof(raw), encoded, sizeof(encoded)) != 0 ||
       strlen(encoded) != KB_OIDC_LOGIN_SECRET_LEN)
   {
      OPENSSL_cleanse(raw, sizeof(raw));
      OPENSSL_cleanse(encoded, sizeof(encoded));
      return -1;
   }
   memcpy(out, encoded, KB_OIDC_LOGIN_SECRET_LEN + 1);
   OPENSSL_cleanse(raw, sizeof(raw));
   OPENSSL_cleanse(encoded, sizeof(encoded));
   return 0;
}

kb_oidc_login_result_t kb_oidc_token_url_split(const char *url, char *host_out, size_t host_cap,
                                               char *path_out, size_t path_cap)
{
   if (host_out && host_cap)
      host_out[0] = '\0';
   if (path_out && path_cap)
      path_out[0] = '\0';
   if (!url || strncmp(url, "https://", 8) != 0)
      return KB_OIDC_LOGIN_INVALID;
   const char *host = url + 8;
   /* The host runs to the first '/', and everything from there is the path. */
   const char *slash = strchr(host, '/');
   size_t host_len = slash ? (size_t)(slash - host) : strlen(host);
   if (host_len == 0)
      return KB_OIDC_LOGIN_INVALID;
   for (size_t i = 0; i < host_len; ++i)
   {
      unsigned char c = (unsigned char)host[i];
      /* Letters, digits, '-' and '.' only. This rejects userinfo ('@'), an
       * explicit port (':'), IPv6 literals ('[') and anything non-ASCII, each of
       * which the egress client would refuse anyway — better to refuse the
       * profile than to fail every login. */
      if (c >= 0x80 || !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') || c == '-' || c == '.'))
         return KB_OIDC_LOGIN_INVALID;
   }
   if (host[0] == '.' || host[host_len - 1] == '.')
      return KB_OIDC_LOGIN_INVALID;

   const char *path = slash ? slash : "/";
   size_t path_len = strlen(path);
   /* An origin-form target carries no query or fragment, and "//" at the start
    * would be read as an authority rather than a path. */
   if (path[0] != '/' || path[1] == '/' || strchr(path, '?') || strchr(path, '#'))
      return KB_OIDC_LOGIN_INVALID;
   for (size_t i = 0; i < path_len; ++i)
   {
      unsigned char c = (unsigned char)path[i];
      if (c < 0x20 || c >= 0x80 || c == ' ' || c == 0x7f)
         return KB_OIDC_LOGIN_INVALID;
   }

   if (host_out)
   {
      if (host_len >= host_cap)
         return KB_OIDC_LOGIN_INVALID;
      memcpy(host_out, host, host_len);
      host_out[host_len] = '\0';
   }
   if (path_out)
   {
      if (path_len >= path_cap)
      {
         if (host_out && host_cap)
            host_out[0] = '\0';
         return KB_OIDC_LOGIN_INVALID;
      }
      memcpy(path_out, path, path_len + 1);
   }
   return KB_OIDC_LOGIN_OK;
}

/* ── Callback query parsing ───────────────────────────────────────────────── */

/* Percent-decode one form-urlencoded value into out[cap]. Refuses rather than
 * repairing: a truncated escape, a non-hex digit, an embedded NUL, any control
 * byte, or a value that does not fit. Control bytes matter beyond tidiness — the
 * decoded value reaches a log line and an error response, so a CR or LF here
 * would be a log-injection primitive that the raw query string does not offer.
 * Returns 0 on success. */
static int form_decode(const char *begin, const char *end, char *out, size_t cap)
{
   size_t n = 0;
   for (const char *p = begin; p < end; ++p)
   {
      unsigned char c = (unsigned char)*p;
      if (c == '+')
         c = ' ';
      else if (c == '%')
      {
         if (end - p < 3)
            return -1;
         int hi = hex_value(p[1]), lo = hex_value(p[2]);
         if (hi < 0 || lo < 0)
            return -1;
         c = (unsigned char)((hi << 4) | lo);
         p += 2;
      }
      if (c == 0 || c < 0x20 || c == 0x7F)
         return -1;
      if (n + 1 >= cap)
         return -1;
      out[n++] = (char)c;
   }
   out[n] = '\0';
   return 0;
}

/* Find `key` in the query string at a KEY BOUNDARY, decode its value into
 * out[cap], and refuse a duplicate. Returns 1 when found, 0 when absent, -1 when
 * present but unusable (duplicated, undecodable, or too long for out).
 *
 * A duplicate is -1 and not "first wins" or "last wins": when a callback carries
 * two states, one of them is somebody else's, and every choice of which to
 * believe is a choice to believe an attacker's. */
static int form_field(const char *qs, const char *key, char *out, size_t cap)
{
   if (cap)
      out[0] = '\0';
   if (!qs || !key)
      return 0;
   size_t keylen = strlen(key);
   int found = 0;
   for (const char *p = qs; *p;)
   {
      const char *amp = strchr(p, '&');
      const char *pair_end = amp ? amp : p + strlen(p);
      const char *eq = memchr(p, '=', (size_t)(pair_end - p));
      if (eq && (size_t)(eq - p) == keylen && memcmp(p, key, keylen) == 0)
      {
         if (found)
            return -1; /* a second occurrence of the same key */
         found = 1;
         if (form_decode(eq + 1, pair_end, out, cap) != 0)
         {
            if (cap)
               out[0] = '\0';
            return -1;
         }
      }
      if (!amp)
         break;
      p = amp + 1;
   }
   return found;
}

/* base64url, exactly the fixed secret length. Checked before the value can reach
 * the pending store, so the store's constant-time scan is never handed something
 * that could not be a state in the first place. */
static int state_shape_valid(const char *s)
{
   if (strnlen(s, KB_OIDC_LOGIN_SECRET_LEN + 1) != KB_OIDC_LOGIN_SECRET_LEN)
      return 0;
   for (size_t i = 0; i < KB_OIDC_LOGIN_SECRET_LEN; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_';
      if (!ok)
         return 0;
   }
   return 1;
}

void kb_oidc_login_callback_clear(kb_oidc_login_callback_t *cb)
{
   if (cb)
      OPENSSL_cleanse(cb, sizeof(*cb));
}

kb_oidc_login_result_t kb_oidc_login_callback_parse(const char *query_string,
                                                    kb_oidc_login_callback_t *out)
{
   if (!out)
      return KB_OIDC_LOGIN_INVALID;
   memset(out, 0, sizeof(*out));
   if (!query_string)
      return KB_OIDC_LOGIN_INVALID;

   /* The error branch is taken FIRST, and on its own: an IdP that returns an
    * error returns no code, and a callback carrying both is not something to
    * pick the favourable half of. */
   char idp_error[KB_OIDC_LOGIN_IDP_ERR_MAX + 1] = "";
   int had_error = form_field(query_string, "error", idp_error, sizeof(idp_error));
   if (had_error != 0)
   {
      /* An unusable error value is still an IdP refusal — the login has failed
       * either way, and reporting _INVALID would send an operator looking for a
       * bug in kb instead of at their IdP. */
      snprintf(out->idp_error, sizeof(out->idp_error), "%s",
               (had_error == 1 && idp_error[0]) ? idp_error : "unspecified");
      return KB_OIDC_LOGIN_IDP_ERROR;
   }

   char code[KB_OIDC_LOGIN_CODE_MAX + 1] = "";
   char state[KB_OIDC_LOGIN_SECRET_LEN + 1] = "";
   if (form_field(query_string, "code", code, sizeof(code)) != 1 || !code[0] ||
       form_field(query_string, "state", state, sizeof(state)) != 1 || !state_shape_valid(state))
   {
      OPENSSL_cleanse(code, sizeof(code));
      OPENSSL_cleanse(state, sizeof(state));
      return KB_OIDC_LOGIN_INVALID;
   }
   snprintf(out->code, sizeof(out->code), "%s", code);
   snprintf(out->state, sizeof(out->state), "%s", state);
   OPENSSL_cleanse(code, sizeof(code));
   OPENSSL_cleanse(state, sizeof(state));
   return KB_OIDC_LOGIN_OK;
}

void kb_oidc_login_pending_clear(kb_oidc_login_pending_t *pending)
{
   if (pending)
      OPENSSL_cleanse(pending, sizeof(*pending));
}

/* The server_id grammar the identity tables CHECK:
 * ^[A-Za-z0-9][A-Za-z0-9._-]{0,126}$ */
static int server_id_valid(const char *s)
{
   if (!s)
      return 0;
   size_t n = strnlen(s, KB_OIDC_LOGIN_SERVER_MAX + 1);
   if (n == 0 || n > KB_OIDC_LOGIN_SERVER_MAX)
      return 0;
   for (size_t i = 0; i < n; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      int alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
      if (i == 0 ? !alnum : !(alnum || c == '.' || c == '_' || c == '-'))
         return 0;
   }
   return 1;
}

kb_oidc_login_result_t kb_oidc_login_start(const kb_oidc_login_config_t *cfg,
                                           const char *target_server_id,
                                           kb_oidc_login_pending_t *pending, char *url_out,
                                           size_t url_cap)
{
   if (url_out && url_cap)
      url_out[0] = '\0';
   if (pending)
      memset(pending, 0, sizeof(*pending));
   if (!cfg || !pending || !url_out || url_cap == 0 || !kb_oidc_login_config_valid(cfg) ||
       !server_id_valid(target_server_id))
      return KB_OIDC_LOGIN_INVALID;

   kb_oidc_login_pending_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   /* Three independent draws. Deriving the nonce from the state (or either from
    * the verifier) would mean one leaked value compromised the others. */
   if (draw_secret(candidate.state) || draw_secret(candidate.code_verifier) ||
       draw_secret(candidate.nonce))
   {
      OPENSSL_cleanse(&candidate, sizeof(candidate));
      return KB_OIDC_LOGIN_UNAVAILABLE;
   }
   snprintf(candidate.redirect_uri, sizeof(candidate.redirect_uri), "%s", cfg->redirect_uri);
   snprintf(candidate.target_server_id, sizeof(candidate.target_server_id), "%s", target_server_id);

   char challenge[OAUTH_PKCE_CHALLENGE_LEN + 1] = "";
   if (oauth_pkce_s256_challenge(candidate.code_verifier, challenge, sizeof(challenge)) != 0)
   {
      OPENSSL_cleanse(&candidate, sizeof(candidate));
      return KB_OIDC_LOGIN_UNAVAILABLE;
   }

   /* "openid" is not optional for an OIDC request: without it the IdP runs a
    * plain OAuth flow and returns no id_token, which is the only thing this
    * login can authenticate anybody from. */
   const oauth_pkce_auth_request_t req = {
       .authorize_url = cfg->authorize_url,
       .client_id = cfg->client_id,
       .redirect_uri = cfg->redirect_uri,
       .scope = cfg->scope[0] ? cfg->scope : "openid",
       .state = candidate.state,
       .code_challenge = challenge,
       .nonce = candidate.nonce,
   };
   int n = oauth_pkce_build_auth_url(&req, url_out, url_cap);
   OPENSSL_cleanse(challenge, sizeof(challenge));
   if (n <= 0)
   {
      OPENSSL_cleanse(&candidate, sizeof(candidate));
      if (url_cap)
         url_out[0] = '\0';
      return KB_OIDC_LOGIN_UNAVAILABLE;
   }
   *pending = candidate;
   OPENSSL_cleanse(&candidate, sizeof(candidate));
   return KB_OIDC_LOGIN_OK;
}

kb_oidc_login_result_t kb_oidc_login_check_state(const kb_oidc_login_pending_t *pending,
                                                 const char *state_from_callback)
{
   if (!pending || !state_from_callback)
      return KB_OIDC_LOGIN_INVALID;
   /* A pending login whose state was never drawn cannot match anything. */
   if (strnlen(pending->state, sizeof(pending->state)) != KB_OIDC_LOGIN_SECRET_LEN)
      return KB_OIDC_LOGIN_INVALID;
   /* Length is public and fixed, so checking it first leaks nothing; the
    * comparison itself is constant time so a wrong state cannot be recovered a
    * character at a time. */
   if (strnlen(state_from_callback, KB_OIDC_LOGIN_SECRET_LEN + 1) != KB_OIDC_LOGIN_SECRET_LEN)
      return KB_OIDC_LOGIN_STATE_MISMATCH;
   return CRYPTO_memcmp(pending->state, state_from_callback, KB_OIDC_LOGIN_SECRET_LEN) == 0
              ? KB_OIDC_LOGIN_OK
              : KB_OIDC_LOGIN_STATE_MISMATCH;
}

kb_oidc_login_result_t kb_oidc_login_check_nonce(const kb_oidc_login_pending_t *pending,
                                                 const char *verified_id_token)
{
   if (!pending || !verified_id_token)
      return KB_OIDC_LOGIN_INVALID;
   if (strnlen(pending->nonce, sizeof(pending->nonce)) != KB_OIDC_LOGIN_SECRET_LEN)
      return KB_OIDC_LOGIN_INVALID;
   char claimed[KB_OIDC_LOGIN_NONCE_MAX] = "";
   if (kb_oidc_id_token_nonce(verified_id_token, claimed, sizeof(claimed)) != 0)
      return KB_OIDC_LOGIN_NONCE_MISMATCH; /* absent claim is a refusal, not a pass */
   kb_oidc_login_result_t rc = KB_OIDC_LOGIN_NONCE_MISMATCH;
   if (strlen(claimed) == KB_OIDC_LOGIN_SECRET_LEN &&
       CRYPTO_memcmp(pending->nonce, claimed, KB_OIDC_LOGIN_SECRET_LEN) == 0)
      rc = KB_OIDC_LOGIN_OK;
   OPENSSL_cleanse(claimed, sizeof(claimed));
   return rc;
}

/* Copy an environment value into a fixed field, refusing rather than truncating:
 * a silently shortened authorize_url or redirect_uri would produce a login that
 * fails at the IdP for no visible reason. */
static int copy_env(char *dst, size_t cap, const char *value)
{
   if (!value)
      return 0;
   size_t n = strnlen(value, cap);
   if (n >= cap)
      return -1;
   memcpy(dst, value, n + 1);
   return 0;
}

kb_oidc_login_result_t kb_oidc_login_config_from_env(kb_oidc_login_config_t *out)
{
   if (!out)
      return KB_OIDC_LOGIN_INVALID;
   memset(out, 0, sizeof(*out));
   const char *client_id = getenv("AIMEE_KB_OIDC_LOGIN_CLIENT_ID");
   if (!client_id || !client_id[0])
      return KB_OIDC_LOGIN_DISABLED;

   /* The issuer is the verifier's own AIMEE_KB_OIDC_ISSUER, not a login-specific
    * one. Two separate knobs could drift, and then the issuer a login trusts
    * would not be the issuer a bearer is checked against — which is exactly the
    * confusion the issuer-scoped identity key exists to prevent. */
   if (copy_env(out->client_id, sizeof(out->client_id), client_id) ||
       copy_env(out->issuer, sizeof(out->issuer), getenv("AIMEE_KB_OIDC_ISSUER")) ||
       copy_env(out->authorize_url, sizeof(out->authorize_url),
                getenv("AIMEE_KB_OIDC_LOGIN_AUTHORIZE_URL")) ||
       copy_env(out->token_url, sizeof(out->token_url), getenv("AIMEE_KB_OIDC_LOGIN_TOKEN_URL")) ||
       copy_env(out->redirect_uri, sizeof(out->redirect_uri),
                getenv("AIMEE_KB_OIDC_LOGIN_REDIRECT_URI")) ||
       copy_env(out->scope, sizeof(out->scope), getenv("AIMEE_KB_OIDC_LOGIN_SCOPE")))
   {
      memset(out, 0, sizeof(*out));
      return KB_OIDC_LOGIN_INVALID;
   }
   /* Set-but-broken is INVALID, never a quiet fall back to DISABLED: an operator
    * who configured a login and typo'd the endpoint must find out at startup, not
    * discover that logins silently do not exist. */
   if (!kb_oidc_login_config_valid(out))
   {
      memset(out, 0, sizeof(*out));
      return KB_OIDC_LOGIN_INVALID;
   }
   return KB_OIDC_LOGIN_OK;
}

kb_oidc_login_result_t kb_oidc_login_principal(const kb_oidc_login_config_t *cfg,
                                               const kb_verify_result_t *verified,
                                               kb_principal_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!cfg || !verified || !out || !kb_oidc_login_config_valid(cfg) || !verified->subject[0])
      return KB_OIDC_LOGIN_INVALID;
   /* The issuer is the CONFIGURED one. Taking it from the token would let a
    * token nominate its own identity namespace and collide with another IdP's
    * subject. */
   if (kb_principal_from_verify(verified, cfg->issuer, out) != 0)
      return KB_OIDC_LOGIN_INVALID;
   return KB_OIDC_LOGIN_OK;
}
