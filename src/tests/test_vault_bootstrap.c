/* test_vault_bootstrap.c — boot-time delegate-vault provisioning (WP-A/B).
 *
 * Pins the behavior of server_vault_bootstrap(): it seals operator-supplied
 * delegate API keys (a JSON secrets file or AIMEE_DELEGATE_KEY_<AGENT> env vars)
 * into the SERVER-principal vault autonomously, is idempotent + non-destructive,
 * skips unknown agents without failing, no-ops with no source, scrubs the env,
 * and never leaves a plaintext secret under $AIMEE_HOME.
 *
 * The module resolves agent names through an injected resolver, so this test
 * provides a trivial one and needs no agent-config link. */
#include "server.h"
#include "vault_service.h"
#include "vault_store.h"
#include "vault_kek_cache.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static char g_root[256]; /* test sandbox: <root>/home is AIMEE_HOME */
static char g_home[320];
static const long T0 = 100000;

/* Trivial resolver: knows "mistral" and "claude" (case-insensitive), canonical
 * form is lowercase. Everything else is unknown. */
static int fake_resolver(const char *name, char *canon, size_t cap)
{
   if (strcasecmp(name, "mistral") == 0)
   {
      snprintf(canon, cap, "mistral");
      return 1;
   }
   if (strcasecmp(name, "claude") == 0)
   {
      snprintf(canon, cap, "claude");
      return 1;
   }
   return 0;
}

static void write_file(const char *path, const char *content)
{
   FILE *f = fopen(path, "wb");
   assert(f);
   fputs(content, f);
   fclose(f);
}

/* True if `needle` appears verbatim in any file under $AIMEE_HOME. */
static int plaintext_under_home(const char *needle)
{
   char cmd[640];
   snprintf(cmd, sizeof(cmd), "grep -rqF -- '%s' '%s' 2>/dev/null", needle, g_home);
   return system(cmd) == 0;
}

/* File source: known agent is sealed + resolves; unknown agent is skipped. */
static void test_file_source(void)
{
   char secrets[400];
   snprintf(secrets, sizeof(secrets), "%s/secrets.json", g_root); /* OUTSIDE home */
   write_file(secrets, "{\"mistral\":\"sk-mistral-ALPHA\",\"ghostagent\":\"sk-nope\"}");
   setenv("AIMEE_DELEGATE_SECRETS_FILE", secrets, 1);

   int n = server_vault_bootstrap();
   assert(n == 1); /* only mistral provisioned; ghostagent unknown */

   assert(vault_store_has_entry(VAULT_SERVER_PRINCIPAL, "mistral", VAULT_API_KEY_CRED) == 1);
   assert(vault_store_has_entry(VAULT_SERVER_PRINCIPAL, "ghostagent", VAULT_API_KEY_CRED) == 0);

   char key[64] = "PRESET";
   assert(vault_service_inject_api_key("", "mistral", key, sizeof(key), T0) == VAULT_OK);
   assert(strcmp(key, "sk-mistral-ALPHA") == 0);

   unlink(secrets);
   unsetenv("AIMEE_DELEGATE_SECRETS_FILE");
   printf("  PASS: test_file_source\n");
}

/* Re-running is non-destructive by default; the overwrite flag forces an update. */
static void test_idempotent_and_overwrite(void)
{
   char secrets[400];
   snprintf(secrets, sizeof(secrets), "%s/secrets2.json", g_root);
   write_file(secrets, "{\"mistral\":\"sk-mistral-BETA\"}");
   setenv("AIMEE_DELEGATE_SECRETS_FILE", secrets, 1);

   /* Default: existing mistral cred is left untouched (provisioned count 0). */
   assert(server_vault_bootstrap() == 0);
   char key[64];
   assert(vault_service_inject_api_key("", "mistral", key, sizeof(key), T0) == VAULT_OK);
   assert(strcmp(key, "sk-mistral-ALPHA") == 0); /* unchanged */

   /* Overwrite flag: the cred is replaced. */
   setenv("AIMEE_DELEGATE_SECRETS_OVERWRITE", "1", 1);
   assert(server_vault_bootstrap() == 1);
   assert(vault_service_inject_api_key("", "mistral", key, sizeof(key), T0) == VAULT_OK);
   assert(strcmp(key, "sk-mistral-BETA") == 0);

   unsetenv("AIMEE_DELEGATE_SECRETS_OVERWRITE");
   unlink(secrets);
   unsetenv("AIMEE_DELEGATE_SECRETS_FILE");
   printf("  PASS: test_idempotent_and_overwrite\n");
}

/* Env source: AIMEE_DELEGATE_KEY_<AGENT> provisions, and the var is scrubbed. */
static void test_env_source(void)
{
   setenv("AIMEE_DELEGATE_KEY_CLAUDE", "sk-claude-GAMMA", 1);
   assert(server_vault_bootstrap() == 1);

   char key[64];
   assert(vault_service_inject_api_key("", "claude", key, sizeof(key), T0) == VAULT_OK);
   assert(strcmp(key, "sk-claude-GAMMA") == 0);

   /* The plaintext env var is unset after ingestion. */
   assert(getenv("AIMEE_DELEGATE_KEY_CLAUDE") == NULL);
   printf("  PASS: test_env_source\n");
}

/* No secret source configured -> no-op (and no crash). */
static void test_no_source_noop(void)
{
   assert(getenv("AIMEE_DELEGATE_SECRETS_FILE") == NULL);
   assert(getenv("AIMEE_DELEGATE_KEY_CLAUDE") == NULL);
   assert(server_vault_bootstrap() == 0);
   printf("  PASS: test_no_source_noop\n");
}

/* Hygiene: no provisioned secret value is present in plaintext under AIMEE_HOME
 * (the vault stores ciphertext; the secrets file lives outside home). */
static void test_no_plaintext_at_rest(void)
{
   assert(!plaintext_under_home("sk-mistral-ALPHA"));
   assert(!plaintext_under_home("sk-mistral-BETA"));
   assert(!plaintext_under_home("sk-claude-GAMMA"));
   printf("  PASS: test_no_plaintext_at_rest\n");
}

int main(void)
{
   snprintf(g_root, sizeof(g_root), "/tmp/aimee-vaultboot-test-%d", (int)getpid());
   snprintf(g_home, sizeof(g_home), "%s/home", g_root);
   char mk[700];
   snprintf(mk, sizeof(mk), "rm -rf %s && mkdir -p %s", g_root, g_home);
   assert(system(mk) == 0);
   setenv("AIMEE_HOME", g_home, 1);
   vault_kek_cache_clear();
   server_vault_bootstrap_set_resolver(fake_resolver);

   test_file_source();
   test_idempotent_and_overwrite();
   test_env_source();
   test_no_source_noop();
   test_no_plaintext_at_rest();

   vault_kek_cache_clear();
   char rm[400];
   snprintf(rm, sizeof(rm), "rm -rf %s", g_root);
   (void)system(rm);
   printf("vault_bootstrap: all tests passed\n");
   return 0;
}
