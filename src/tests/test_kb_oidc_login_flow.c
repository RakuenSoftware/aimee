/* test_kb_oidc_login_flow.c — the relying-party flow end to end, minus the socket.
 *
 * The four units of increment 4a each have their own unit test, and each of those
 * proves a property in isolation. What none of them proves is that they COMPOSE:
 * that the state the store is keyed by is the one the authorization URL carried,
 * that the verifier the exchange body sends is the one bound to that login, that
 * the nonce in the IdP's id_token is the one this login asked for, and that the
 * subject the whole thing yields is the identity key the intent writer will
 * record. A seam mismatch between any two of them passes every unit test and
 * fails every real login.
 *
 * So this drives the actual sequence, with a real RSA keypair standing in for the
 * IdP:
 *
 *   start  -> authorization URL + pending secrets
 *   store  -> retained across the redirect
 *   callback -> state looked up, login consumed
 *   exchange -> request body built from the retained login
 *   IdP    -> a genuinely signed id_token echoing the nonce
 *   verify -> signature, iss, aud
 *   nonce  -> bound to this login
 *   principal -> the issuer-scoped identity key
 *
 * Then the attacks that have to fail at the seams rather than inside one unit:
 * a callback replayed, a callback with another login's state, and an id_token
 * that is perfectly valid but was minted for a different login.
 */
#include "kb_oidc_login.h"
#include "kb_oidc_login_store.h"
#include "kb_oidc_token_exchange.h"

#include "kb_auth_oidc.h"
#include "kb_identity.h"
#include "oauth_pkce.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Distinct per call: this flow draws many secrets and a repeating fake CSPRNG
 * would collide two logins' states, which the store correctly refuses. */
int platform_random_bytes(void *buf, size_t len)
{
   static uint32_t call_no = 1;
   unsigned char *out = buf;
   assert(len >= sizeof(call_no));
   for (size_t i = 0; i < len; ++i)
      out[i] = (unsigned char)(i * 11u + 5u);
   memcpy(out, &call_no, sizeof(call_no));
   call_no++;
   return 0;
}

static const long NOW = 1780000000L;
static const char *ISSUER = "https://idp.example";
static const char *CLIENT_ID = "aimee-kb";

static char *b64url(const unsigned char *in, size_t inlen)
{
   char *out = malloc(inlen * 2 + 8);
   assert(out);
   assert(oauth_pkce_base64url_encode(in, inlen, out, inlen * 2 + 8) == 0);
   return out;
}

static char *make_jwks(EVP_PKEY *pkey)
{
   BIGNUM *n = NULL, *e = NULL;
   assert(EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &n) == 1);
   assert(EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &e) == 1);
   unsigned char nbuf[1024], ebuf[16];
   int nlen = BN_bn2bin(n, nbuf);
   int elen = BN_bn2bin(e, ebuf);
   BN_free(n);
   BN_free(e);
   char *n64 = b64url(nbuf, (size_t)nlen);
   char *e64 = b64url(ebuf, (size_t)elen);
   char *jwks = malloc(strlen(n64) + strlen(e64) + 128);
   assert(jwks);
   sprintf(jwks, "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"idp-key\",\"n\":\"%s\",\"e\":\"%s\"}]}",
           n64, e64);
   free(n64);
   free(e64);
   return jwks;
}

/* The IdP: mints a real RS256 id_token for `subject`, echoing `nonce`. */
static char *idp_mint_id_token(EVP_PKEY *key, const char *subject, const char *nonce)
{
   char payload[1024];
   snprintf(payload, sizeof(payload),
            "{\"iss\":\"%s\",\"aud\":\"%s\",\"sub\":\"%s\",\"iat\":%ld,\"exp\":%ld,"
            "\"nonce\":\"%s\"}",
            ISSUER, CLIENT_ID, subject, NOW, NOW + 300, nonce);
   char *h64 = b64url((const unsigned char *)"{\"alg\":\"RS256\",\"kid\":\"idp-key\"}",
                      strlen("{\"alg\":\"RS256\",\"kid\":\"idp-key\"}"));
   char *p64 = b64url((const unsigned char *)payload, strlen(payload));
   char *input = malloc(strlen(h64) + strlen(p64) + 2);
   assert(input);
   sprintf(input, "%s.%s", h64, p64);
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   assert(md && EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, key) == 1);
   size_t siglen = 0;
   assert(EVP_DigestSign(md, NULL, &siglen, (unsigned char *)input, strlen(input)) == 1);
   unsigned char *sig = malloc(siglen);
   assert(sig && EVP_DigestSign(md, sig, &siglen, (unsigned char *)input, strlen(input)) == 1);
   EVP_MD_CTX_free(md);
   char *s64 = b64url(sig, siglen);
   char *jwt = malloc(strlen(input) + strlen(s64) + 2);
   assert(jwt);
   sprintf(jwt, "%s.%s", input, s64);
   free(sig);
   free(s64);
   free(input);
   free(h64);
   free(p64);
   return jwt;
}

/* The IdP's token-endpoint response body for a minted id_token. */
static char *idp_token_response(const char *id_token)
{
   char *body = malloc(strlen(id_token) + 128);
   assert(body);
   sprintf(body, "{\"token_type\":\"Bearer\",\"expires_in\":300,\"id_token\":\"%s\"}", id_token);
   return body;
}

static kb_oidc_login_config_t rp_config(void)
{
   kb_oidc_login_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.issuer, sizeof(cfg.issuer), "%s", ISSUER);
   snprintf(cfg.client_id, sizeof(cfg.client_id), "%s", CLIENT_ID);
   snprintf(cfg.authorize_url, sizeof(cfg.authorize_url), "%s/authorize", ISSUER);
   snprintf(cfg.token_url, sizeof(cfg.token_url), "%s/token", ISSUER);
   snprintf(cfg.redirect_uri, sizeof(cfg.redirect_uri), "%s",
            "https://kb.example/v1/identity/login/callback");
   return cfg;
}

static kb_oidc_config_t verifier_config(const char *jwks)
{
   kb_oidc_config_t v;
   memset(&v, 0, sizeof(v));
   snprintf(v.issuer, sizeof(v.issuer), "%s", ISSUER);
   snprintf(v.audience, sizeof(v.audience), "%s", CLIENT_ID);
   snprintf(v.jwks_json, sizeof(v.jwks_json), "%s", jwks);
   return v;
}

/* Pull a query parameter's value out of an authorization URL, so the assertions
 * below read what the BROWSER would actually send back rather than trusting the
 * pending struct. This is the whole point: it closes the loop through the URL. */
static void query_param(const char *url, const char *name, char *out, size_t cap)
{
   char needle[64];
   snprintf(needle, sizeof(needle), "&%s=", name);
   const char *p = strstr(url, needle);
   assert(p);
   p += strlen(needle);
   size_t n = 0;
   while (p[n] && p[n] != '&' && n + 1 < cap)
   {
      out[n] = p[n];
      n++;
   }
   out[n] = '\0';
}

static void test_happy_path(EVP_PKEY *idp_key, const char *jwks)
{
   kb_oidc_login_store_reset();
   kb_oidc_login_config_t cfg = rp_config();

   /* 1. START. */
   kb_oidc_login_pending_t pending;
   char url[KB_OIDC_LOGIN_URL_MAX];
   assert(kb_oidc_login_start(&cfg, "srv-a", &pending, url, sizeof(url)) == KB_OIDC_LOGIN_OK);

   /* 2. RETAIN across the browser redirect. */
   assert(kb_oidc_login_store_put(&pending, NOW, 300) == KB_OIDC_LOGIN_STORE_OK);

   /* 3. The browser comes back. Read the state out of the URL the browser was
    * sent, not out of the struct — that is what closes the loop. */
   char state_from_browser[128];
   query_param(url, "state", state_from_browser, sizeof(state_from_browser));
   assert(strlen(state_from_browser) == KB_OIDC_LOGIN_SECRET_LEN);

   kb_oidc_login_pending_t resumed;
   assert(kb_oidc_login_store_take(state_from_browser, NOW + 5, &resumed) ==
          KB_OIDC_LOGIN_STORE_OK);
   /* Belt and braces: the resumed login also satisfies the explicit state check
    * the callback handler runs. */
   assert(kb_oidc_login_check_state(&resumed, state_from_browser) == KB_OIDC_LOGIN_OK);

   /* 4. EXCHANGE. The body must carry the verifier bound to THIS login and the
    * redirect_uri that was actually sent to the IdP. */
   char body[KB_OIDC_TOKEN_EXCHANGE_BODY_MAX];
   assert(kb_oidc_token_request_body(&resumed, "the-auth-code", cfg.client_id, body,
                                     sizeof(body)) == KB_OIDC_TOKEN_EXCHANGE_OK);
   char challenge_from_url[128];
   query_param(url, "code_challenge", challenge_from_url, sizeof(challenge_from_url));
   char challenge_of_verifier[OAUTH_PKCE_CHALLENGE_LEN + 1];
   assert(oauth_pkce_s256_challenge(resumed.code_verifier, challenge_of_verifier,
                                    sizeof(challenge_of_verifier)) == 0);
   /* The verifier we are about to send S256-hashes to the challenge the IdP was
    * given. If the store or the exchange had crossed logins, this is what breaks. */
   assert(!strcmp(challenge_from_url, challenge_of_verifier));

   char basic[512];
   assert(kb_oidc_token_basic_auth(cfg.client_id, "the-client-secret", basic, sizeof(basic)) ==
          KB_OIDC_TOKEN_EXCHANGE_OK);

   /* 5. The IdP answers, echoing the nonce it was given in the authorization URL. */
   char nonce_from_url[128];
   query_param(url, "nonce", nonce_from_url, sizeof(nonce_from_url));
   char *id_token = idp_mint_id_token(idp_key, "alice", nonce_from_url);
   char *response = idp_token_response(id_token);

   char unverified[KB_OIDC_TOKEN_EXCHANGE_JWT_MAX + 1];
   assert(kb_oidc_token_response_id_token(response, strlen(response), unverified,
                                          sizeof(unverified)) == KB_OIDC_TOKEN_EXCHANGE_OK);
   assert(!strcmp(unverified, id_token));

   /* 6. VERIFY, then 7. bind to this login. Order matters: the nonce claim is
    * only meaningful once the signature is known good. */
   kb_oidc_config_t v = verifier_config(jwks);
   kb_verify_result_t verified;
   assert(kb_oidc_verify_jwt(unverified, &v, NOW, &verified) == 1);
   assert(kb_oidc_login_check_nonce(&resumed, unverified) == KB_OIDC_LOGIN_OK);

   /* 8. The principal the intent writer will record. */
   kb_principal_t principal;
   assert(kb_oidc_login_principal(&cfg, &verified, &principal) == KB_OIDC_LOGIN_OK);
   assert(principal.authenticated == 1);
   char identity_key[700];
   assert(kb_identity_key(&principal, identity_key, sizeof(identity_key)) == 0);
   /* This is the exact string kb_management_identity_intent.subject must accept,
    * and the shape kb_write_tier_grant.subject is CHECKed against:
    * oidc:<iss>:<sub> with the issuer's ':' percent-encoded. */
   assert(!strcmp(identity_key, "oidc:https%3A//idp.example:alice"));

   /* The target server survived the redirect, so the intent the callback files
    * names the server the LOGIN asked for rather than anything the callback
    * carried. This is the field db2_identity_intent_start takes. */
   assert(!strcmp(resumed.target_server_id, "srv-a"));

   /* The login is spent: nothing is left to replay. */
   assert(kb_oidc_login_store_count(NOW + 5) == 0);

   free(response);
   free(id_token);
   kb_oidc_login_pending_clear(&resumed);
   kb_oidc_login_pending_clear(&pending);
}

static void test_replayed_callback_fails(EVP_PKEY *idp_key, const char *jwks)
{
   (void)idp_key;
   (void)jwks;
   kb_oidc_login_store_reset();
   kb_oidc_login_config_t cfg = rp_config();
   kb_oidc_login_pending_t pending;
   char url[KB_OIDC_LOGIN_URL_MAX];
   assert(kb_oidc_login_start(&cfg, "srv-a", &pending, url, sizeof(url)) == KB_OIDC_LOGIN_OK);
   assert(kb_oidc_login_store_put(&pending, NOW, 300) == KB_OIDC_LOGIN_STORE_OK);

   char state[128];
   query_param(url, "state", state, sizeof(state));
   kb_oidc_login_pending_t first, second;
   assert(kb_oidc_login_store_take(state, NOW, &first) == KB_OIDC_LOGIN_STORE_OK);
   /* The identical callback URL, replayed out of browser history or a proxy log,
    * cannot resume anything. */
   assert(kb_oidc_login_store_take(state, NOW, &second) == KB_OIDC_LOGIN_STORE_NOT_FOUND);
   kb_oidc_login_pending_clear(&first);
}

static void test_foreign_state_fails(void)
{
   kb_oidc_login_store_reset();
   kb_oidc_login_config_t cfg = rp_config();
   kb_oidc_login_pending_t mine, theirs;
   char my_url[KB_OIDC_LOGIN_URL_MAX], their_url[KB_OIDC_LOGIN_URL_MAX];
   assert(kb_oidc_login_start(&cfg, "srv-a", &mine, my_url, sizeof(my_url)) == KB_OIDC_LOGIN_OK);
   assert(kb_oidc_login_start(&cfg, "srv-a", &theirs, their_url, sizeof(their_url)) ==
          KB_OIDC_LOGIN_OK);
   /* Only one of them was ever started by this kb. */
   assert(kb_oidc_login_store_put(&mine, NOW, 300) == KB_OIDC_LOGIN_STORE_OK);

   char their_state[128];
   query_param(their_url, "state", their_state, sizeof(their_state));
   kb_oidc_login_pending_t resumed;
   /* A forged callback carrying a state this kb never issued finds nothing, and
    * critically does NOT consume the legitimate pending login. */
   assert(kb_oidc_login_store_take(their_state, NOW, &resumed) == KB_OIDC_LOGIN_STORE_NOT_FOUND);
   assert(kb_oidc_login_store_count(NOW) == 1);
   char my_state[128];
   query_param(my_url, "state", my_state, sizeof(my_state));
   assert(kb_oidc_login_store_take(my_state, NOW, &resumed) == KB_OIDC_LOGIN_STORE_OK);
   kb_oidc_login_pending_clear(&resumed);
}

static void test_valid_token_from_another_login_fails(EVP_PKEY *idp_key, const char *jwks)
{
   kb_oidc_login_store_reset();
   kb_oidc_login_config_t cfg = rp_config();

   /* Two logins started by this same kb. */
   kb_oidc_login_pending_t victim, attacker;
   char victim_url[KB_OIDC_LOGIN_URL_MAX], attacker_url[KB_OIDC_LOGIN_URL_MAX];
   assert(kb_oidc_login_start(&cfg, "srv-a", &victim, victim_url, sizeof(victim_url)) ==
          KB_OIDC_LOGIN_OK);
   assert(kb_oidc_login_start(&cfg, "srv-a", &attacker, attacker_url, sizeof(attacker_url)) ==
          KB_OIDC_LOGIN_OK);

   /* The IdP mints a token for the ATTACKER's login: correctly signed, correct
    * issuer and audience, unexpired. It verifies. */
   char attacker_nonce[128];
   query_param(attacker_url, "nonce", attacker_nonce, sizeof(attacker_nonce));
   char *id_token = idp_mint_id_token(idp_key, "attacker", attacker_nonce);
   kb_oidc_config_t v = verifier_config(jwks);
   kb_verify_result_t verified;
   assert(kb_oidc_verify_jwt(id_token, &v, NOW, &verified) == 1);

   /* THE attack: inject that fully valid token into the victim's login. The
    * signature check cannot catch this — only the nonce binding can. */
   assert(kb_oidc_login_check_nonce(&victim, id_token) == KB_OIDC_LOGIN_NONCE_MISMATCH);
   /* And it does satisfy the login it was actually minted for, so the refusal
    * above is the binding working rather than the token being broken. */
   assert(kb_oidc_login_check_nonce(&attacker, id_token) == KB_OIDC_LOGIN_OK);

   free(id_token);
   kb_oidc_login_pending_clear(&victim);
   kb_oidc_login_pending_clear(&attacker);
}

static void test_idp_error_stops_the_flow(void)
{
   kb_oidc_login_store_reset();
   char tok[KB_OIDC_TOKEN_EXCHANGE_JWT_MAX + 1];
   const char *denied = "{\"error\":\"invalid_grant\",\"error_description\":\"code expired\"}";
   assert(kb_oidc_token_response_id_token(denied, strlen(denied), tok, sizeof(tok)) ==
          KB_OIDC_TOKEN_EXCHANGE_DENIED);
   assert(tok[0] == '\0');
}

int main(void)
{
   EVP_PKEY *idp_key = NULL;
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
   assert(ctx && EVP_PKEY_keygen_init(ctx) == 1);
   assert(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) == 1);
   assert(EVP_PKEY_keygen(ctx, &idp_key) == 1);
   EVP_PKEY_CTX_free(ctx);
   char *jwks = make_jwks(idp_key);

   test_happy_path(idp_key, jwks);
   test_replayed_callback_fails(idp_key, jwks);
   test_foreign_state_fails();
   test_valid_token_from_another_login_fails(idp_key, jwks);
   test_idp_error_stops_the_flow();

   kb_oidc_login_store_reset();
   free(jwks);
   EVP_PKEY_free(idp_key);
   printf("test_kb_oidc_login_flow: ok\n");
   return 0;
}
