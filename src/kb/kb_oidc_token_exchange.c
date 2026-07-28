/* kb_oidc_token_exchange.c — see kb_oidc_token_exchange.h. */

#include "kb_oidc_token_exchange.h"

#include "cJSON.h"
#include "util.h" /* aimee_base64_encode, aimee_base64_encoded_len */

#include <openssl/crypto.h> /* OPENSSL_cleanse */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

/* RFC 3986 unreserved. Everything else is percent-encoded, including the
 * characters application/x-www-form-urlencoded would otherwise turn into '+':
 * %20 is accepted everywhere and needs no reader-side special case. */
static int unreserved(unsigned char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
          c == '_' || c == '.' || c == '~';
}

/* Append the percent-encoding of `s` at out[*pos]. Returns 0 on success, -1 if
 * it would not fit — never a truncated value, because a truncated code_verifier
 * would fail the exchange with a confusing IdP-side error. */
static int append_encoded(char *out, size_t cap, size_t *pos, const char *s)
{
   for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
   {
      if (unreserved(*p))
      {
         if (*pos + 1 >= cap)
            return -1;
         out[(*pos)++] = (char)*p;
         continue;
      }
      if (*pos + 3 >= cap)
         return -1;
      static const char hex[] = "0123456789ABCDEF";
      out[(*pos)++] = '%';
      out[(*pos)++] = hex[*p >> 4];
      out[(*pos)++] = hex[*p & 15];
   }
   return 0;
}

static int append_literal(char *out, size_t cap, size_t *pos, const char *s)
{
   size_t n = strlen(s);
   if (*pos + n >= cap)
      return -1;
   memcpy(out + *pos, s, n);
   *pos += n;
   return 0;
}

/* No control bytes and no CR/LF: these values go into a request line and a
 * header, so a newline would be a request-splitting primitive. */
static int safe_value(const char *s, size_t max)
{
   if (!s)
      return 0;
   size_t n = strnlen(s, max + 1);
   if (n == 0 || n > max)
      return 0;
   for (size_t i = 0; i < n; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      if (c < 0x20 || c == 0x7f)
         return 0;
   }
   return 1;
}

kb_oidc_token_exchange_result_t kb_oidc_token_request_body(const kb_oidc_login_pending_t *pending,
                                                           const char *code, const char *client_id,
                                                           char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   if (!pending || !out || cap == 0 || !safe_value(code, 2048) || !safe_value(client_id, 255))
      return KB_OIDC_TOKEN_EXCHANGE_INVALID;
   /* A pending login that was never started has no verifier to present. */
   if (strnlen(pending->code_verifier, sizeof(pending->code_verifier)) !=
           KB_OIDC_LOGIN_SECRET_LEN ||
       !safe_value(pending->redirect_uri, sizeof(pending->redirect_uri) - 1))
      return KB_OIDC_TOKEN_EXCHANGE_INVALID;

   size_t pos = 0;
   int ok = append_literal(out, cap, &pos, "grant_type=authorization_code&code=") == 0 &&
            append_encoded(out, cap, &pos, code) == 0 &&
            append_literal(out, cap, &pos, "&redirect_uri=") == 0 &&
            /* The redirect_uri from the PENDING login, so it matches the
             * authorization request byte for byte (RFC 6749 §4.1.3). */
            append_encoded(out, cap, &pos, pending->redirect_uri) == 0 &&
            append_literal(out, cap, &pos, "&client_id=") == 0 &&
            append_encoded(out, cap, &pos, client_id) == 0 &&
            append_literal(out, cap, &pos, "&code_verifier=") == 0 &&
            append_encoded(out, cap, &pos, pending->code_verifier) == 0;
   if (!ok)
   {
      OPENSSL_cleanse(out, cap);
      return KB_OIDC_TOKEN_EXCHANGE_TOO_LARGE;
   }
   out[pos] = '\0';
   return KB_OIDC_TOKEN_EXCHANGE_OK;
}

kb_oidc_token_exchange_result_t
kb_oidc_token_basic_auth(const char *client_id, const char *client_secret, char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   if (!out || cap == 0 || !safe_value(client_id, 255) || !safe_value(client_secret, 512))
      return KB_OIDC_TOKEN_EXCHANGE_INVALID;

   /* Form-urlencode each half BEFORE joining, per RFC 6749 §2.3.1. Skipping this
    * is the classic cause of a client that works until its secret contains a
    * colon, a plus, or a space. */
   char credential[1600];
   size_t pos = 0;
   int ok = append_encoded(credential, sizeof(credential), &pos, client_id) == 0 &&
            append_literal(credential, sizeof(credential), &pos, ":") == 0 &&
            append_encoded(credential, sizeof(credential), &pos, client_secret) == 0;
   if (!ok)
   {
      OPENSSL_cleanse(credential, sizeof(credential));
      return KB_OIDC_TOKEN_EXCHANGE_TOO_LARGE;
   }
   credential[pos] = '\0';

   char encoded[2200];
   kb_oidc_token_exchange_result_t rc = KB_OIDC_TOKEN_EXCHANGE_TOO_LARGE;
   if (aimee_base64_encoded_len(pos) <= sizeof(encoded) &&
       aimee_base64_encode((const unsigned char *)credential, pos, encoded, sizeof(encoded)) > 0)
   {
      size_t need = strlen("Basic ") + strlen(encoded);
      if (need < cap)
      {
         snprintf(out, cap, "Basic %s", encoded);
         rc = KB_OIDC_TOKEN_EXCHANGE_OK;
      }
   }
   /* The secret was in both buffers; neither may survive this call. */
   OPENSSL_cleanse(credential, sizeof(credential));
   OPENSSL_cleanse(encoded, sizeof(encoded));
   if (rc != KB_OIDC_TOKEN_EXCHANGE_OK)
      OPENSSL_cleanse(out, cap);
   return rc;
}

/* Three dot-separated non-empty segments and no fourth. Shape only: this says
 * nothing about the signature, and a JWE (five segments) is refused here because
 * the verifier does not accept one either. */
static int compact_jws_shape(const char *s)
{
   const char *d1 = strchr(s, '.');
   if (!d1 || d1 == s)
      return 0;
   const char *d2 = strchr(d1 + 1, '.');
   if (!d2 || d2 == d1 + 1 || !d2[1])
      return 0;
   return strchr(d2 + 1, '.') == NULL;
}

kb_oidc_token_exchange_result_t kb_oidc_token_response_id_token(const char *body, size_t body_len,
                                                                char *unverified_id_token_out,
                                                                size_t cap)
{
   if (unverified_id_token_out && cap)
      unverified_id_token_out[0] = '\0';
   if (!body || !unverified_id_token_out || cap == 0 || body_len == 0 ||
       body_len > KB_OIDC_TOKEN_EXCHANGE_RESPONSE_MAX)
      return KB_OIDC_TOKEN_EXCHANGE_INVALID;

   /* cJSON needs a terminated buffer and the body is not guaranteed to be one. */
   char *json = malloc(body_len + 1);
   if (!json)
      return KB_OIDC_TOKEN_EXCHANGE_INVALID;
   memcpy(json, body, body_len);
   json[body_len] = '\0';
   cJSON *root = cJSON_Parse(json);
   OPENSSL_cleanse(json, body_len + 1);
   free(json);
   if (!root)
      return KB_OIDC_TOKEN_EXCHANGE_MALFORMED;

   kb_oidc_token_exchange_result_t rc = KB_OIDC_TOKEN_EXCHANGE_MALFORMED;
   /* An OAuth error response is a distinct outcome, checked FIRST: some IdPs
    * return 200 with an error body, and treating that as "no id_token" would
    * lose the reason. */
   const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
   if (err)
   {
      cJSON_Delete(root);
      return KB_OIDC_TOKEN_EXCHANGE_DENIED;
   }
   /* token_type is case-insensitive per RFC 6749 §5.1. Absent is tolerated (some
    * IdPs omit it on a pure OIDC code exchange); present-and-not-Bearer is not. */
   const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "token_type");
   if (type && (!cJSON_IsString(type) || strcasecmp(type->valuestring, "Bearer") != 0))
   {
      cJSON_Delete(root);
      return KB_OIDC_TOKEN_EXCHANGE_MALFORMED;
   }
   const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id_token");
   if (cJSON_IsString(id) && id->valuestring && id->valuestring[0])
   {
      size_t n = strlen(id->valuestring);
      if (n >= cap || n > KB_OIDC_TOKEN_EXCHANGE_JWT_MAX)
         rc = KB_OIDC_TOKEN_EXCHANGE_TOO_LARGE;
      else if (compact_jws_shape(id->valuestring))
      {
         memcpy(unverified_id_token_out, id->valuestring, n + 1);
         rc = KB_OIDC_TOKEN_EXCHANGE_OK;
      }
   }
   cJSON_Delete(root);
   if (rc != KB_OIDC_TOKEN_EXCHANGE_OK)
      unverified_id_token_out[0] = '\0';
   return rc;
}
