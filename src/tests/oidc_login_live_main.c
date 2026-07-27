/* oidc_login_live_main.c — drive the relying-party login against a REAL OIDC
 * identity provider.
 *
 * The one part of increment 4a that every other test stubs. The unit tests sign
 * their id_tokens with a real keypair, which proves the verifier reads a real
 * signature, but they never send a byte to an identity provider: the token-endpoint
 * POST is replaced, so nothing exercises the real TLS handshake, the real
 * application/x-www-form-urlencoded round trip, Keycloak's actual response shape,
 * its actual JWKS (which carries an encryption key alongside the signing key, so
 * kid selection matters), or its enforcement of the PKCE verifier.
 *
 * This binary is that missing exercise. It links the production units and calls
 * them in the production order — kb_oidc_login_start, kb_oidc_token_exchange_post,
 * kb_oidc_verify_id_token, kb_oidc_login_check_nonce, kb_oidc_login_principal — so
 * what is being tested is the shipping code path and not a reimplementation of it.
 *
 * It is split into two subcommands because the middle of an authorization-code flow
 * is a BROWSER. `start` emits the authorization URL and the per-login secrets; a
 * shell script drives Keycloak's login form and recovers the code; `finish` takes
 * that code and completes the flow.
 *
 * RIG CONCESSION, stated plainly: `start` writes the code_verifier and nonce to a
 * file so `finish` can read them back. Production never does this — the pending
 * store is process-local precisely so those secrets never reach a disk. The file is
 * mode 0600 in a root-only directory and the harness deletes it. Nothing else about
 * the flow is faked.
 *
 * Usage:
 *   oidc-login-live start  <state-file>
 *   oidc-login-live finish <state-file> <code>
 *
 * Configuration comes from the same environment a deployment uses
 * (AIMEE_KB_OIDC_LOGIN_*, AIMEE_KB_OIDC_ISSUER) plus:
 *   AIMEE_KB_OIDC_JWKS_FILE      the IdP's JWKS, for the verifier
 *   AIMEE_KB_OIDC_AUDIENCE       kb as a RESOURCE SERVER — deliberately NOT the
 *                                client_id, so a live run proves the id_token
 *                                audience override really is applied
 *   AIMEE_OIDC_LIVE_CLIENT_SECRET the client secret (the vault is not involved here;
 *                                this driver tests the OIDC path, not custody)
 */
#include "kb_auth_oidc.h"
#include "kb_identity.h"
#include "kb_oidc_login.h"
#include "kb_oidc_token_exchange.h"

#include <fcntl.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *login_result_name(kb_oidc_login_result_t r)
{
   switch (r)
   {
   case KB_OIDC_LOGIN_OK:
      return "OK";
   case KB_OIDC_LOGIN_INVALID:
      return "INVALID (configuration or argument)";
   case KB_OIDC_LOGIN_UNAVAILABLE:
      return "UNAVAILABLE (CSPRNG, or the URL did not fit)";
   case KB_OIDC_LOGIN_STATE_MISMATCH:
      return "STATE_MISMATCH";
   case KB_OIDC_LOGIN_NONCE_MISMATCH:
      return "NONCE_MISMATCH";
   case KB_OIDC_LOGIN_DISABLED:
      return "DISABLED (no login profile configured)";
   case KB_OIDC_LOGIN_IDP_ERROR:
      return "IDP_ERROR";
   }
   return "UNKNOWN";
}

static const char *exchange_result_name(kb_oidc_token_exchange_result_t r)
{
   switch (r)
   {
   case KB_OIDC_TOKEN_EXCHANGE_OK:
      return "OK";
   case KB_OIDC_TOKEN_EXCHANGE_INVALID:
      return "INVALID (bad argument or unusable field)";
   case KB_OIDC_TOKEN_EXCHANGE_TOO_LARGE:
      return "TOO_LARGE";
   case KB_OIDC_TOKEN_EXCHANGE_MALFORMED:
      return "MALFORMED (not a token response)";
   case KB_OIDC_TOKEN_EXCHANGE_DENIED:
      return "DENIED (the IdP returned an OAuth error)";
   case KB_OIDC_TOKEN_EXCHANGE_UNAVAILABLE:
      return "UNAVAILABLE (never reached the IdP)";
   }
   return "UNKNOWN";
}

/* The pending login, persisted between the two invocations. See the rig-concession
 * note at the top of this file. */
static int pending_write(const char *path, const kb_oidc_login_pending_t *p)
{
   int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
   if (fd < 0)
      return -1;
   FILE *f = fdopen(fd, "w");
   if (!f)
   {
      close(fd);
      return -1;
   }
   fprintf(f, "%s\n%s\n%s\n%s\n%s\n%lld\n", p->state, p->code_verifier, p->nonce, p->redirect_uri,
           p->target_server_id, (long long)p->team_id);
   return fclose(f) == 0 ? 0 : -1;
}

static int pending_read(const char *path, kb_oidc_login_pending_t *p)
{
   memset(p, 0, sizeof(*p));
   FILE *f = fopen(path, "r");
   if (!f)
      return -1;
   long long team = 0;
   char *fields[] = {p->state, p->code_verifier, p->nonce, p->redirect_uri, p->target_server_id};
   size_t caps[] = {sizeof(p->state), sizeof(p->code_verifier), sizeof(p->nonce),
                    sizeof(p->redirect_uri), sizeof(p->target_server_id)};
   for (size_t i = 0; i < 5; i++)
   {
      /* A GENEROUS line buffer, then copy. fgets reads at most cap-1 characters, so
       * an exactly-sized destination consumes the field but leaves its newline in the
       * stream — the next fgets then returns just that newline, every later field
       * shifts by one, and the trailing number fails to parse. That is what happened
       * the first time this ran. */
      char line[1024];
      if (!fgets(line, (int)sizeof(line), f))
      {
         fclose(f);
         return -1;
      }
      line[strcspn(line, "\r\n")] = '\0';
      if (strlen(line) >= caps[i])
      {
         fclose(f);
         return -1;
      }
      snprintf(fields[i], caps[i], "%s", line);
   }
   if (fscanf(f, "%lld", &team) != 1)
   {
      fclose(f);
      return -1;
   }
   fclose(f);
   p->team_id = (int64_t)team;
   return 0;
}

static int do_start(const char *state_file)
{
   kb_oidc_login_config_t cfg;
   kb_oidc_login_result_t rc = kb_oidc_login_config_from_env(&cfg);
   if (rc != KB_OIDC_LOGIN_OK)
   {
      fprintf(stderr, "oidc-login-live: no usable login profile: %s\n", login_result_name(rc));
      return 65;
   }
   printf("profile: issuer=%s client=%s\n", cfg.issuer, cfg.client_id);
   printf("         authorize=%s\n", cfg.authorize_url);
   printf("         token=%s\n", cfg.token_url);

   kb_oidc_login_pending_t pending;
   char url[KB_OIDC_LOGIN_URL_MAX];
   rc = kb_oidc_login_start(&cfg, "mintsrv", 770001, &pending, url, sizeof(url));
   if (rc != KB_OIDC_LOGIN_OK)
   {
      fprintf(stderr, "oidc-login-live: start refused: %s\n", login_result_name(rc));
      return 66;
   }
   if (pending_write(state_file, &pending) != 0)
   {
      fprintf(stderr, "oidc-login-live: could not persist the pending login\n");
      OPENSSL_cleanse(&pending, sizeof(pending));
      return 73;
   }
   /* The URL is what a browser is sent to, so it is safe to print. The state and
    * nonce appear in it by design; the VERIFIER does not, and that is the property
    * worth seeing confirmed against a live IdP. */
   printf("AUTHORIZE_URL %s\n", url);
   printf("  code_challenge present: %s\n", strstr(url, "code_challenge=") ? "yes" : "NO");
   printf("  method S256: %s\n", strstr(url, "code_challenge_method=S256") ? "yes" : "NO");
   printf("  verifier absent from the URL: %s\n",
          strstr(url, pending.code_verifier) ? "NO — LEAKED" : "yes");
   OPENSSL_cleanse(&pending, sizeof(pending));
   return 0;
}

static int do_finish(const char *state_file, const char *code)
{
   kb_oidc_login_config_t cfg;
   if (kb_oidc_login_config_from_env(&cfg) != KB_OIDC_LOGIN_OK)
   {
      fprintf(stderr, "oidc-login-live: no usable login profile\n");
      return 65;
   }
   kb_oidc_login_pending_t pending;
   if (pending_read(state_file, &pending) != 0)
   {
      fprintf(stderr, "oidc-login-live: could not read the pending login\n");
      return 66;
   }

   /* Register the verifier from the IdP's real JWKS. The audience configured here is
    * kb-as-resource-server, NOT the client id, so a successful verification below
    * proves kb_oidc_verify_id_token really does override it. */
   if (kb_oidc_register_from_env() != 0)
   {
      fprintf(stderr, "oidc-login-live: could not register the OIDC verifier\n");
      OPENSSL_cleanse(&pending, sizeof(pending));
      return 65;
   }

   const char *secret = getenv("AIMEE_OIDC_LIVE_CLIENT_SECRET");
   if (!secret || !secret[0])
   {
      fprintf(stderr, "oidc-login-live: AIMEE_OIDC_LIVE_CLIENT_SECRET unset\n");
      OPENSSL_cleanse(&pending, sizeof(pending));
      return 65;
   }

   /* THE REAL NETWORK CALL. Real TLS to a real IdP on 443, real form encoding, real
    * client-secret-basic, real response parsing. */
   char id_token[KB_OIDC_TOKEN_EXCHANGE_JWT_MAX] = "";
   kb_oidc_token_exchange_result_t xrc =
       kb_oidc_token_exchange_post(&cfg, &pending, code, secret, id_token, sizeof(id_token));
   printf("exchange: %s\n", exchange_result_name(xrc));
   if (xrc != KB_OIDC_TOKEN_EXCHANGE_OK)
   {
      OPENSSL_cleanse(&pending, sizeof(pending));
      return 69;
   }
   const char *d1 = strchr(id_token, '.');
   const char *d2 = d1 ? strchr(d1 + 1, '.') : NULL;
   printf("  id_token from the IdP: header=%zu payload=%zu signature=%zu bytes\n",
          d1 ? (size_t)(d1 - id_token) : 0, (d1 && d2) ? (size_t)(d2 - d1 - 1) : 0,
          d2 ? strlen(d2 + 1) : 0);

   int exit_code = 0;
   kb_verify_result_t verified;
   memset(&verified, 0, sizeof(verified));
   /* The audience passed is the CLIENT ID, per OIDC Core 3.1.3.7. */
   if (!kb_oidc_verify_id_token(id_token, cfg.client_id, (long)time(NULL), &verified))
   {
      fprintf(stderr, "oidc-login-live: the id_token FAILED verification\n");
      exit_code = 70;
   }
   else
   {
      printf("verified: signature, iss and aud check out against the live JWKS\n");
      kb_oidc_login_result_t nrc = kb_oidc_login_check_nonce(&pending, id_token);
      printf("nonce: %s\n", login_result_name(nrc));
      if (nrc != KB_OIDC_LOGIN_OK)
         exit_code = 71;
      else
      {
         kb_principal_t principal;
         kb_oidc_login_result_t prc = kb_oidc_login_principal(&cfg, &verified, &principal);
         if (prc != KB_OIDC_LOGIN_OK)
         {
            fprintf(stderr, "oidc-login-live: no usable principal: %s\n", login_result_name(prc));
            exit_code = 72;
         }
         else
         {
            char subject[576] = "";
            if (kb_identity_key(&principal, subject, sizeof(subject)) != 0)
            {
               fprintf(stderr, "oidc-login-live: could not derive the identity key\n");
               exit_code = 72;
            }
            else
            {
               printf("SUBJECT %s\n", subject);
               printf("  (issuer-scoped from the CONFIGURED issuer, not the token's)\n");
            }
         }
         OPENSSL_cleanse(&principal, sizeof(principal));
      }
   }

   /* Prove the code is single-use AT THE IdP, which no stub can establish: a second
    * exchange of the same code with the same verifier must be refused by Keycloak. */
   if (exit_code == 0)
   {
      char again[KB_OIDC_TOKEN_EXCHANGE_JWT_MAX] = "";
      kb_oidc_token_exchange_result_t r2 =
          kb_oidc_token_exchange_post(&cfg, &pending, code, secret, again, sizeof(again));
      printf("replayed code: %s\n", exchange_result_name(r2));
      if (r2 == KB_OIDC_TOKEN_EXCHANGE_OK)
      {
         fprintf(stderr, "oidc-login-live: the IdP ACCEPTED a replayed code\n");
         exit_code = 74;
      }
      OPENSSL_cleanse(again, sizeof(again));
   }

   OPENSSL_cleanse(&verified, sizeof(verified));
   OPENSSL_cleanse(id_token, sizeof(id_token));
   OPENSSL_cleanse(&pending, sizeof(pending));
   return exit_code;
}

int main(int argc, char **argv)
{
   if (argc >= 3 && strcmp(argv[1], "start") == 0)
      return do_start(argv[2]);
   if (argc >= 4 && strcmp(argv[1], "finish") == 0)
      return do_finish(argv[2], argv[3]);
   fprintf(stderr, "usage: oidc-login-live start <state-file>\n"
                   "       oidc-login-live finish <state-file> <code>\n");
   return 64;
}
