/* test_kb_oidc_token_exchange.c — the code-for-id_token codec.
 *
 * These are the rules where a mistake is security-relevant, and all of them are
 * checkable without a socket:
 *   - the redirect_uri sent comes from the PENDING login, not from configuration
 *   - the code_verifier is sent and the challenge is not
 *   - every value is percent-encoded, so a code containing '&' cannot inject a
 *     parameter, and a newline cannot split the request
 *   - Basic credentials form-urlencode each half BEFORE joining, so a secret
 *     containing ':' or '+' does not silently produce the wrong credential
 *   - an OAuth error response is DENIED, distinct from a malformed one
 *   - a response with no id_token, a non-Bearer token_type, or something that is
 *     not a compact JWS is refused rather than best-effort accepted
 */
#include "kb_oidc_token_exchange.h"

#include "kb_oidc_login.h"
#include "util.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int platform_random_bytes(void *buf, size_t len)
{
   static unsigned char sequence = 5;
   unsigned char *out = buf;
   for (size_t i = 0; i < len; ++i)
      out[i] = (unsigned char)(sequence++ * 13u + 7u);
   return 0;
}

static kb_oidc_login_pending_t make_pending(const char *redirect)
{
   kb_oidc_login_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.issuer, sizeof(cfg.issuer), "%s", "https://idp.example");
   snprintf(cfg.client_id, sizeof(cfg.client_id), "%s", "aimee-kb");
   snprintf(cfg.authorize_url, sizeof(cfg.authorize_url), "%s", "https://idp.example/authorize");
   snprintf(cfg.token_url, sizeof(cfg.token_url), "%s", "https://idp.example/token");
   snprintf(cfg.redirect_uri, sizeof(cfg.redirect_uri), "%s", redirect);
   kb_oidc_login_pending_t p;
   char url[KB_OIDC_LOGIN_URL_MAX];
   assert(kb_oidc_login_start(&cfg, &p, url, sizeof(url)) == KB_OIDC_LOGIN_OK);
   return p;
}

static void test_request_body(void)
{
   kb_oidc_login_pending_t p = make_pending("https://kb.example/oidc/callback");
   char body[KB_OIDC_TOKEN_EXCHANGE_BODY_MAX];
   assert(kb_oidc_token_request_body(&p, "authcode123", "aimee-kb", body, sizeof(body)) ==
          KB_OIDC_TOKEN_EXCHANGE_OK);

   assert(!strncmp(body, "grant_type=authorization_code&", 30));
   assert(strstr(body, "&code=authcode123"));
   assert(strstr(body, "&client_id=aimee-kb"));
   /* The verifier is sent... */
   char expect_verifier[256];
   snprintf(expect_verifier, sizeof(expect_verifier), "&code_verifier=%s", p.code_verifier);
   assert(strstr(body, expect_verifier));
   /* ...and the challenge is not: the IdP already holds it. */
   assert(!strstr(body, "code_challenge"));
   /* The redirect_uri is the pending login's, percent-encoded. */
   assert(strstr(body, "&redirect_uri=https%3A%2F%2Fkb.example%2Foidc%2Fcallback"));

   /* A code carrying '&' or '=' must not be able to inject a parameter. */
   assert(kb_oidc_token_request_body(&p, "a&client_id=evil", "aimee-kb", body, sizeof(body)) ==
          KB_OIDC_TOKEN_EXCHANGE_OK);
   assert(strstr(body, "&code=a%26client_id%3Devil"));
   assert(!strstr(body, "client_id=evil"));

   /* CR/LF is refused outright — it would split the request, not merely encode. */
   assert(kb_oidc_token_request_body(&p, "code\r\nX-Evil: 1", "aimee-kb", body, sizeof(body)) ==
          KB_OIDC_TOKEN_EXCHANGE_INVALID);
   assert(body[0] == '\0');

   /* THE property: the redirect_uri comes from the pending login, so a
    * configuration change mid-login cannot turn into a silent RFC 6749 §4.1.3
    * mismatch. A second pending record with a different redirect proves the
    * value tracks the login, not the config in hand. */
   kb_oidc_login_pending_t other = make_pending("https://kb.example/other/cb");
   assert(kb_oidc_token_request_body(&other, "c", "aimee-kb", body, sizeof(body)) ==
          KB_OIDC_TOKEN_EXCHANGE_OK);
   assert(strstr(body, "redirect_uri=https%3A%2F%2Fkb.example%2Fother%2Fcb"));

   /* Bad arguments, and a pending login that was never started. */
   assert(kb_oidc_token_request_body(NULL, "c", "id", body, sizeof(body)) ==
          KB_OIDC_TOKEN_EXCHANGE_INVALID);
   assert(kb_oidc_token_request_body(&p, NULL, "id", body, sizeof(body)) ==
          KB_OIDC_TOKEN_EXCHANGE_INVALID);
   assert(kb_oidc_token_request_body(&p, "", "id", body, sizeof(body)) ==
          KB_OIDC_TOKEN_EXCHANGE_INVALID);
   assert(kb_oidc_token_request_body(&p, "c", NULL, body, sizeof(body)) ==
          KB_OIDC_TOKEN_EXCHANGE_INVALID);
   assert(kb_oidc_token_request_body(&p, "c", "id", NULL, sizeof(body)) ==
          KB_OIDC_TOKEN_EXCHANGE_INVALID);
   kb_oidc_login_pending_t empty;
   memset(&empty, 0, sizeof(empty));
   assert(kb_oidc_token_request_body(&empty, "c", "id", body, sizeof(body)) ==
          KB_OIDC_TOKEN_EXCHANGE_INVALID);

   /* A buffer too small is TOO_LARGE with nothing written — never a truncated
    * verifier, which would fail at the IdP with a confusing error. */
   char tiny[40];
   assert(kb_oidc_token_request_body(&p, "authcode123", "aimee-kb", tiny, sizeof(tiny)) ==
          KB_OIDC_TOKEN_EXCHANGE_TOO_LARGE);
   assert(tiny[0] == '\0');
}

static void test_basic_auth(void)
{
   char out[512];
   assert(kb_oidc_token_basic_auth("aimee-kb", "s3cret", out, sizeof(out)) ==
          KB_OIDC_TOKEN_EXCHANGE_OK);
   assert(!strncmp(out, "Basic ", 6));
   unsigned char decoded[512];
   size_t n = aimee_base64_decode(out + 6, decoded, sizeof(decoded) - 1);
   decoded[n] = '\0';
   assert(!strcmp((char *)decoded, "aimee-kb:s3cret"));

   /* THE property: each half is form-urlencoded BEFORE joining. A secret with a
    * colon must not produce a credential the IdP splits in the wrong place, and
    * one with '+' or a space must not arrive altered. */
   assert(kb_oidc_token_basic_auth("cli:ent", "pa:ss word+plus", out, sizeof(out)) ==
          KB_OIDC_TOKEN_EXCHANGE_OK);
   n = aimee_base64_decode(out + 6, decoded, sizeof(decoded) - 1);
   decoded[n] = '\0';
   assert(!strcmp((char *)decoded, "cli%3Aent:pa%3Ass%20word%2Bplus"));
   /* Exactly one unencoded ':' separates the halves. */
   assert(strchr((char *)decoded, ':') == strrchr((char *)decoded, ':'));

   /* Refusals. A control byte in a header value would be request splitting. */
   assert(kb_oidc_token_basic_auth(NULL, "s", out, sizeof(out)) == KB_OIDC_TOKEN_EXCHANGE_INVALID);
   assert(kb_oidc_token_basic_auth("id", NULL, out, sizeof(out)) == KB_OIDC_TOKEN_EXCHANGE_INVALID);
   assert(kb_oidc_token_basic_auth("", "s", out, sizeof(out)) == KB_OIDC_TOKEN_EXCHANGE_INVALID);
   assert(kb_oidc_token_basic_auth("id", "", out, sizeof(out)) == KB_OIDC_TOKEN_EXCHANGE_INVALID);
   assert(kb_oidc_token_basic_auth("id\r\n", "s", out, sizeof(out)) ==
          KB_OIDC_TOKEN_EXCHANGE_INVALID);
   assert(kb_oidc_token_basic_auth("id", "s\ns", out, sizeof(out)) ==
          KB_OIDC_TOKEN_EXCHANGE_INVALID);

   /* A buffer too small leaves nothing behind — least of all part of a secret. */
   char tiny[8];
   assert(kb_oidc_token_basic_auth("aimee-kb", "s3cret", tiny, sizeof(tiny)) ==
          KB_OIDC_TOKEN_EXCHANGE_TOO_LARGE);
   assert(tiny[0] == '\0');
}

#define JWS "eyJhbGciOiJSUzI1NiJ9.eyJzdWIiOiJhbGljZSJ9.c2ln"

static kb_oidc_token_exchange_result_t parse(const char *json, char *out, size_t cap)
{
   return kb_oidc_token_response_id_token(json, strlen(json), out, cap);
}

static void test_response_parsing(void)
{
   char tok[KB_OIDC_TOKEN_EXCHANGE_JWT_MAX + 1];

   assert(parse("{\"token_type\":\"Bearer\",\"id_token\":\"" JWS "\"}", tok, sizeof(tok)) ==
          KB_OIDC_TOKEN_EXCHANGE_OK);
   assert(!strcmp(tok, JWS));

   /* token_type is case-insensitive (RFC 6749 §5.1) and may be omitted on a pure
    * OIDC code exchange; present-and-wrong is refused. */
   assert(parse("{\"token_type\":\"bearer\",\"id_token\":\"" JWS "\"}", tok, sizeof(tok)) ==
          KB_OIDC_TOKEN_EXCHANGE_OK);
   assert(parse("{\"id_token\":\"" JWS "\"}", tok, sizeof(tok)) == KB_OIDC_TOKEN_EXCHANGE_OK);
   assert(parse("{\"token_type\":\"mac\",\"id_token\":\"" JWS "\"}", tok, sizeof(tok)) ==
          KB_OIDC_TOKEN_EXCHANGE_MALFORMED);
   assert(parse("{\"token_type\":7,\"id_token\":\"" JWS "\"}", tok, sizeof(tok)) ==
          KB_OIDC_TOKEN_EXCHANGE_MALFORMED);

   /* An OAuth error is its own outcome, and is checked before the id_token —
    * some IdPs return 200 with an error body, and calling that "malformed" would
    * lose the reason. Note the error body here ALSO carries a valid id_token. */
   assert(parse("{\"error\":\"invalid_grant\"}", tok, sizeof(tok)) ==
          KB_OIDC_TOKEN_EXCHANGE_DENIED);
   assert(tok[0] == '\0');
   assert(parse("{\"error\":\"invalid_grant\",\"id_token\":\"" JWS "\"}", tok, sizeof(tok)) ==
          KB_OIDC_TOKEN_EXCHANGE_DENIED);
   assert(tok[0] == '\0');

   /* No id_token, or one that is not a usable string. */
   assert(parse("{\"access_token\":\"abc\"}", tok, sizeof(tok)) ==
          KB_OIDC_TOKEN_EXCHANGE_MALFORMED);
   assert(parse("{\"id_token\":null}", tok, sizeof(tok)) == KB_OIDC_TOKEN_EXCHANGE_MALFORMED);
   assert(parse("{\"id_token\":123}", tok, sizeof(tok)) == KB_OIDC_TOKEN_EXCHANGE_MALFORMED);
   assert(parse("{\"id_token\":\"\"}", tok, sizeof(tok)) == KB_OIDC_TOKEN_EXCHANGE_MALFORMED);
   assert(parse("{\"id_token\":{}}", tok, sizeof(tok)) == KB_OIDC_TOKEN_EXCHANGE_MALFORMED);

   /* Shape: three non-empty dot-separated segments, and not five (a JWE, which
    * the verifier would not accept either). */
   assert(parse("{\"id_token\":\"notajwt\"}", tok, sizeof(tok)) ==
          KB_OIDC_TOKEN_EXCHANGE_MALFORMED);
   assert(parse("{\"id_token\":\"a.b\"}", tok, sizeof(tok)) == KB_OIDC_TOKEN_EXCHANGE_MALFORMED);
   assert(parse("{\"id_token\":\".b.c\"}", tok, sizeof(tok)) == KB_OIDC_TOKEN_EXCHANGE_MALFORMED);
   assert(parse("{\"id_token\":\"a..c\"}", tok, sizeof(tok)) == KB_OIDC_TOKEN_EXCHANGE_MALFORMED);
   assert(parse("{\"id_token\":\"a.b.\"}", tok, sizeof(tok)) == KB_OIDC_TOKEN_EXCHANGE_MALFORMED);
   assert(parse("{\"id_token\":\"a.b.c.d.e\"}", tok, sizeof(tok)) ==
          KB_OIDC_TOKEN_EXCHANGE_MALFORMED);

   /* Not JSON at all, and an empty/oversize body. */
   assert(parse("not json", tok, sizeof(tok)) == KB_OIDC_TOKEN_EXCHANGE_MALFORMED);
   assert(parse("", tok, sizeof(tok)) == KB_OIDC_TOKEN_EXCHANGE_INVALID);
   assert(kb_oidc_token_response_id_token(NULL, 10, tok, sizeof(tok)) ==
          KB_OIDC_TOKEN_EXCHANGE_INVALID);
   assert(kb_oidc_token_response_id_token("{}", 2, NULL, sizeof(tok)) ==
          KB_OIDC_TOKEN_EXCHANGE_INVALID);
   assert(kb_oidc_token_response_id_token("{}", 2, tok, 0) == KB_OIDC_TOKEN_EXCHANGE_INVALID);
   assert(kb_oidc_token_response_id_token("{}", KB_OIDC_TOKEN_EXCHANGE_RESPONSE_MAX + 1, tok,
                                          sizeof(tok)) == KB_OIDC_TOKEN_EXCHANGE_INVALID);

   /* A body that is not NUL-terminated is handled from its length alone. */
   const char framed[] = "{\"id_token\":\"" JWS "\"}TRAILING-GARBAGE";
   assert(kb_oidc_token_response_id_token(framed, strlen("{\"id_token\":\"" JWS "\"}"), tok,
                                          sizeof(tok)) == KB_OIDC_TOKEN_EXCHANGE_OK);
   assert(!strcmp(tok, JWS));

   /* A token too long for the caller's buffer is TOO_LARGE, not truncated. */
   char small[16];
   assert(parse("{\"id_token\":\"" JWS "\"}", small, sizeof(small)) ==
          KB_OIDC_TOKEN_EXCHANGE_TOO_LARGE);
   assert(small[0] == '\0');
}

int main(void)
{
   test_request_body();
   test_basic_auth();
   test_response_parsing();
   printf("test_kb_oidc_token_exchange: ok\n");
   return 0;
}
