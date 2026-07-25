/* kb_identity_token.c — builder for the kb-signed data-plane identity token.
 * See kb_identity_token.h and proposal per-user-remote-writes-authz.md §4.
 *
 * The JSON payload is assembled with cJSON so every string claim (notably
 * `sub`, which originates from an OIDC provider or a PAM username) is correctly
 * escaped — a hand-rolled builder could let a crafted `sub` inject claims into a
 * *signed* token. base64url encoding reuses the vetted oauth_pkce encoder; the
 * signature is produced by the caller's signer (the kb token authority). The
 * `typ` header is "aimee-id+jwt", distinct from the management JWT's "JWT", so
 * the two token types cannot be confused even before the audience check. */

#include "kb_identity_token.h"

#include <openssl/crypto.h>
#include <string.h>

#include "cJSON.h"
#include "oauth_pkce.h" /* oauth_pkce_base64url_encode */

/* RSA-4096 signatures are 512 bytes; 2048 -> 256. Size for the larger. */
#define SIG_MAX 512u
#define JSON_MAX 2048u

const char *kb_identity_tier_str(kb_identity_tier_t tier)
{
   switch (tier)
   {
   case KB_IDENTITY_TIER_OFF:
      return "off";
   case KB_IDENTITY_TIER_DATA:
      return "data";
   case KB_IDENTITY_TIER_FULL:
      return "full";
   default:
      return NULL;
   }
}

/* 1 iff s is NUL-terminated within cap AND non-empty. */
static int bounded_nonempty(const char *s, size_t cap)
{
   if (!s || cap == 0)
      return 0;
   size_t n = strnlen(s, cap);
   return n > 0 && n < cap;
}

static int claims_valid(const kb_identity_token_claims_t *c)
{
   if (!c)
      return 0;
   if (!bounded_nonempty(c->issuer, sizeof(c->issuer)) ||
       !bounded_nonempty(c->audience, sizeof(c->audience)) ||
       !bounded_nonempty(c->subject, sizeof(c->subject)) ||
       !bounded_nonempty(c->jti, sizeof(c->jti)) || !bounded_nonempty(c->kid, sizeof(c->kid)))
      return 0;
   if (c->team_id <= 0)
      return 0;
   if (!kb_identity_tier_str(c->tier))
      return 0;
   /* Reject a non-positive or non-increasing validity window (no clock read
    * here; the caller resolves the times). */
   if (c->issued_at < 0 || c->expires_at <= c->issued_at)
      return 0;
   return 1;
}

/* Serialize a cJSON object to buf (NUL-terminated). Returns length, or 0 on
 * failure / overflow. */
static size_t print_json(cJSON *obj, char *buf, size_t cap)
{
   if (!obj)
      return 0;
   char *s = cJSON_PrintUnformatted(obj);
   if (!s)
      return 0;
   size_t n = strlen(s);
   if (n == 0 || n >= cap)
   {
      OPENSSL_cleanse(s, n);
      cJSON_free(s);
      return 0;
   }
   memcpy(buf, s, n + 1);
   OPENSSL_cleanse(s, n);
   cJSON_free(s);
   return n;
}

kb_identity_token_result_t kb_identity_token_build(const kb_identity_token_claims_t *c,
                                                   kb_identity_token_sign_fn signer,
                                                   void *signer_ctx, char *jwt_out, size_t jwt_cap,
                                                   size_t *jwt_len)
{
   if (jwt_len)
      *jwt_len = 0;
   if (jwt_out && jwt_cap)
      jwt_out[0] = '\0';
   if (!jwt_out || !jwt_len || !signer || !claims_valid(c))
      return KB_IDENTITY_TOKEN_INVALID;

   kb_identity_token_result_t result = KB_IDENTITY_TOKEN_INVALID;
   char header_json[JSON_MAX], payload_json[JSON_MAX];
   size_t hj = 0, pj = 0;
   cJSON *header = cJSON_CreateObject();
   cJSON *payload = cJSON_CreateObject();
   if (header && payload && cJSON_AddStringToObject(header, "alg", "RS256") &&
       cJSON_AddStringToObject(header, "typ", "aimee-id+jwt") &&
       cJSON_AddStringToObject(header, "kid", c->kid) && cJSON_AddNumberToObject(payload, "v", 1) &&
       cJSON_AddStringToObject(payload, "iss", c->issuer) &&
       cJSON_AddStringToObject(payload, "aud", c->audience) &&
       cJSON_AddStringToObject(payload, "sub", c->subject) &&
       cJSON_AddNumberToObject(payload, "team_id", (double)c->team_id) &&
       cJSON_AddStringToObject(payload, "tier", kb_identity_tier_str(c->tier)) &&
       cJSON_AddStringToObject(payload, "jti", c->jti) &&
       cJSON_AddNumberToObject(payload, "iat", (double)c->issued_at) &&
       cJSON_AddNumberToObject(payload, "exp", (double)c->expires_at))
   {
      hj = print_json(header, header_json, sizeof(header_json));
      pj = print_json(payload, payload_json, sizeof(payload_json));
   }
   cJSON_Delete(header);
   cJSON_Delete(payload);
   if (hj == 0 || pj == 0)
      return KB_IDENTITY_TOKEN_INVALID;

   /* signing_input = b64url(header) "." b64url(payload) */
   char signing[KB_IDENTITY_TOKEN_WIRE_MAX + 1];
   if (oauth_pkce_base64url_encode((const unsigned char *)header_json, hj, signing,
                                   sizeof(signing)) != 0)
   {
      OPENSSL_cleanse(payload_json, sizeof(payload_json));
      return KB_IDENTITY_TOKEN_INVALID;
   }
   size_t hn = strlen(signing);
   if (hn + 1 >= sizeof(signing))
   {
      OPENSSL_cleanse(payload_json, sizeof(payload_json));
      return KB_IDENTITY_TOKEN_INVALID;
   }
   signing[hn] = '.';
   if (oauth_pkce_base64url_encode((const unsigned char *)payload_json, pj, signing + hn + 1,
                                   sizeof(signing) - hn - 1) != 0)
   {
      OPENSSL_cleanse(payload_json, sizeof(payload_json));
      OPENSSL_cleanse(signing, sizeof(signing));
      return KB_IDENTITY_TOKEN_INVALID;
   }
   OPENSSL_cleanse(payload_json, sizeof(payload_json));
   size_t input_n = strlen(signing);

   /* Preflight worst-case wire size: signing_input "." b64url(signature). */
   size_t sig_b64_max = ((SIG_MAX + 2) / 3) * 4; /* no-pad upper bound */
   if (input_n + 1 + sig_b64_max >= KB_IDENTITY_TOKEN_WIRE_MAX)
   {
      OPENSSL_cleanse(signing, sizeof(signing));
      return KB_IDENTITY_TOKEN_INVALID;
   }
   if (jwt_cap <= input_n + 1 + sig_b64_max)
   {
      OPENSSL_cleanse(signing, sizeof(signing));
      return KB_IDENTITY_TOKEN_OUTPUT_TOO_SMALL;
   }

   unsigned char signature[SIG_MAX];
   memset(signature, 0, sizeof(signature));
   size_t sig_n = 0;
   int signed_ok =
       signer(signer_ctx, (const unsigned char *)signing, input_n, signature, sizeof(signature),
              &sig_n);
   if (!signed_ok || sig_n < 256 || sig_n > sizeof(signature))
   {
      OPENSSL_cleanse(signature, sizeof(signature));
      OPENSSL_cleanse(signing, sizeof(signing));
      return KB_IDENTITY_TOKEN_SIGN_UNAVAILABLE;
   }

   memcpy(jwt_out, signing, input_n);
   jwt_out[input_n] = '.';
   if (oauth_pkce_base64url_encode(signature, sig_n, jwt_out + input_n + 1,
                                   jwt_cap - input_n - 1) != 0)
   {
      OPENSSL_cleanse(signature, sizeof(signature));
      OPENSSL_cleanse(signing, sizeof(signing));
      OPENSSL_cleanse(jwt_out, jwt_cap);
      return KB_IDENTITY_TOKEN_OUTPUT_TOO_SMALL;
   }
   *jwt_len = strlen(jwt_out);
   result = KB_IDENTITY_TOKEN_OK;

   OPENSSL_cleanse(signature, sizeof(signature));
   OPENSSL_cleanse(signing, sizeof(signing));
   return result;
}
