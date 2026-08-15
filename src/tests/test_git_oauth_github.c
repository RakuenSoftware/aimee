/* test_git_oauth_github.c — completing a GitHub sign-in means the token is
 * STORED, not merely received.
 *
 * GitHub handing over an access token and the vault keeping it are independent
 * failures. Reporting completion on the first alone leaves the host with no
 * credential while the user is told they are connected; every later forge call
 * then reports "no github credential" and points back at a sign-in that looked
 * like it worked. Both completion paths are covered here: the device poll and
 * the web (redirect) callback.
 *
 * The HTTP and vault externals are stubbed and scripted; no network. */

#include "modules/git/git_oauth_github.h"
#include "vault_service.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Scripted externals ─────────────────────────────────────────────────────── */

/* Canned POST reply for the next call, so start -> poll can be walked. */
static const char *g_http_reply = NULL;
int agent_http_post_content_type(const char *url, const char *auth_header, const char *content_type,
                                 const char *body, char **response_buf, int timeout_ms,
                                 const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)content_type;
   (void)body;
   (void)timeout_ms;
   (void)extra_headers;
   if (g_http_reply && response_buf)
   {
      *response_buf = strdup(g_http_reply);
      return 200;
   }
   if (response_buf)
      *response_buf = NULL;
   return 0;
}

/* The store outcome is the whole point of these tests. */
static int g_cred_set_rc = 0;
static int g_cred_set_calls = 0;
int git_host_cred_set(const char *host, const char *token)
{
   (void)host;
   (void)token;
   g_cred_set_calls++;
   return g_cred_set_rc;
}

vault_status_t vault_service_set_server(const char *agent, const char *cred, const char *secret)
{
   (void)agent;
   (void)cred;
   (void)secret;
   return VAULT_OK;
}

/* A configured OAuth client id. The built-in default is empty in most builds,
 * so without this the flow stops at "sign-in is not configured". */
static const char *g_vault_client_id = NULL;
vault_status_t vault_service_get_server_principal(const char *agent, const char *cred, char *out,
                                                  size_t out_len)
{
   (void)agent;
   (void)cred;
   if (out && out_len)
      out[0] = '\0';
   if (g_vault_client_id && out && out_len)
   {
      snprintf(out, out_len, "%s", g_vault_client_id);
      return VAULT_OK;
   }
   return VAULT_NO_ENTRY;
}

/* ── Link-only stubs (not exercised by these tests) ─────────────────────────── */

int platform_random_bytes(void *buf, size_t len)
{
   memset(buf, 0x5A, len); /* deterministic: these tests never check the state token */
   return 0;
}

char *cli_v1_pct_encode(const char *s)
{
   return s ? strdup(s) : NULL;
}

int oauth_pkce_base64url_encode(const unsigned char *in, size_t in_len, char *out, size_t out_cap)
{
   (void)in;
   if (!out || out_cap == 0)
      return -1;
   size_t n = in_len < out_cap - 1 ? in_len : out_cap - 1;
   memset(out, 'A', n);
   out[n] = '\0';
   return 0;
}

const char *runtime_secret_get(const char *name)
{
   (void)name;
   return NULL;
}

/* ── Tests ──────────────────────────────────────────────────────────────────── */

static void test_device_poll_requires_the_token_to_be_stored(void)
{
   char user_code[64], verify[256], err[256];
   int interval = 0;

   g_vault_client_id = "CID-TEST";
   g_http_reply = "{\"device_code\":\"DEV-CODE\",\"user_code\":\"UC-1\","
                  "\"verification_uri\":\"https://github.com/login/device\",\"interval\":5}";
   assert(git_oauth_github_start("alice", user_code, sizeof(user_code), verify, sizeof(verify),
                                 &interval, err, sizeof(err)) == 0);

   /* Stored → the sign-in completes. */
   g_http_reply = "{\"access_token\":\"TOK-1\"}";
   g_cred_set_rc = 0;
   g_cred_set_calls = 0;
   err[0] = '\0';
   assert(git_oauth_github_poll("alice", err, sizeof(err)) == 1);
   assert(g_cred_set_calls == 1);
   assert(err[0] == '\0');

   /* Not stored → must FAIL and say so. Before the store was checked this
    * returned 1 and the credential was silently gone. */
   g_http_reply = "{\"device_code\":\"DEV-CODE\",\"user_code\":\"UC-2\","
                  "\"verification_uri\":\"https://github.com/login/device\",\"interval\":5}";
   assert(git_oauth_github_start("alice", user_code, sizeof(user_code), verify, sizeof(verify),
                                 &interval, err, sizeof(err)) == 0);
   g_http_reply = "{\"access_token\":\"TOK-2\"}";
   g_cred_set_rc = -1;
   g_cred_set_calls = 0;
   err[0] = '\0';
   assert(git_oauth_github_poll("alice", err, sizeof(err)) == -1);
   assert(g_cred_set_calls == 1);
   assert(strstr(err, "could not store") != NULL);

   g_http_reply = NULL;
   g_cred_set_rc = 0;
   g_vault_client_id = NULL;
   printf("  device poll requires the token to be stored: OK\n");
}

int main(void)
{
   printf("test_git_oauth_github:\n");
   test_device_poll_requires_the_token_to_be_stored();
   printf("ALL PASS\n");
   return 0;
}
