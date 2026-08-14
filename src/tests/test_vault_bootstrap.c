/* test_vault_bootstrap.c — boot-time delegate-vault provisioning (WP-A/B).
 *
 * Pins the behavior of server_vault_bootstrap(): it seals operator-supplied
 * delegate API keys from AIMEE_DELEGATE_KEY_<AGENT> first-boot env vars
 * into the SERVER-principal vault autonomously, is idempotent + non-destructive,
 * fails closed for unknown agents, no-ops with no source, scrubs the env,
 * and never leaves a plaintext secret under $AIMEE_HOME.
 *
 * The module resolves agent names through an injected resolver, so this test
 * provides a trivial one and needs no agent-config link. */
#include "server.h"
#include "config.h"
#include "oauth_flow.h"
#include "vault_service.h"
#include "vault_store.h"
#include "vault_kek_cache.h"
#include "vault_env_bootstrap.h"
#include "runtime_secret.h"
#include <openssl/crypto.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
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

/* True if `needle` appears verbatim in any file under $AIMEE_HOME. */
static int plaintext_under_home(const char *needle)
{
   char cmd[640];
   snprintf(cmd, sizeof(cmd), "grep -rqF -- '%s' '%s' 2>/dev/null", needle, g_home);
   return system(cmd) == 0;
}

/* Re-running is non-destructive by default; the overwrite flag forces an update. */
static void test_idempotent_and_overwrite(void)
{
   setenv("AIMEE_DELEGATE_KEY_MISTRAL", "sk-mistral-ALPHA", 1);
   assert(server_vault_bootstrap() == 1);

   /* Default: existing mistral cred is left untouched (provisioned count 0). */
   setenv("AIMEE_DELEGATE_KEY_MISTRAL", "sk-mistral-BETA", 1);
   assert(server_vault_bootstrap() == 0);
   char key[64];
   assert(vault_service_inject_api_key("", "mistral", key, sizeof(key), T0) == VAULT_OK);
   assert(strcmp(key, "sk-mistral-ALPHA") == 0); /* unchanged */

   /* Overwrite flag: the cred is replaced. */
   setenv("AIMEE_DELEGATE_KEY_MISTRAL", "sk-mistral-BETA", 1);
   setenv("AIMEE_DELEGATE_SECRETS_OVERWRITE", "1", 1);
   assert(server_vault_bootstrap() == 1);
   assert(vault_service_inject_api_key("", "mistral", key, sizeof(key), T0) == VAULT_OK);
   assert(strcmp(key, "sk-mistral-BETA") == 0);

   unsetenv("AIMEE_DELEGATE_SECRETS_OVERWRITE");
   printf("  PASS: test_idempotent_and_overwrite\n");
}

/* Env source: AIMEE_DELEGATE_KEY_<AGENT> provisions, and the var is scrubbed. */
static void test_env_source(void)
{
   setenv("AIMEE_DELEGATE_KEY_CLAUDE", "sk-claude-GAMMA", 1);
   assert(vault_env_has_credential_environment() == 1);
   assert(server_vault_bootstrap() == 1);
   assert(vault_env_has_credential_environment() == 0);

   char key[64];
   assert(vault_service_inject_api_key("", "claude", key, sizeof(key), T0) == VAULT_OK);
   assert(strcmp(key, "sk-claude-GAMMA") == 0);

   /* The plaintext env var is unset after ingestion. */
   assert(getenv("AIMEE_DELEGATE_KEY_CLAUDE") == NULL);
   printf("  PASS: test_env_source\n");
}

/* The server forge token is a first-boot input: it is sealed into its canonical
 * server Vault slot and removed from the process environment. */
static void test_forge_env_source(void)
{
   setenv("AIMEE_FORGE_TOKEN", "ghs-forge-DELTA", 1);
   assert(vault_env_bootstrap_init() == 1);
   assert(getenv("AIMEE_FORGE_TOKEN") == NULL);

   char token[64];
   assert(vault_service_get_server_principal("git", "forge_token", token, sizeof(token)) ==
          VAULT_OK);
   assert(strcmp(token, "ghs-forge-DELTA") == 0);
   memset(token, 0, sizeof(token));
   printf("  PASS: test_forge_env_source\n");
}

static void test_generic_env_source(void)
{
   setenv("AIMEE_DB2_URL", "postgresql://user:db-password@db/aimee", 1);
   setenv("AIMEE_VAULT_PKCS11_PIN", "vaulted-hsm-pin", 1);
   setenv("AIMEE_KB_CLIENT_PAM_USERNAME", "aimee-server", 1);
   assert(vault_env_has_credential_environment() == 1);
   assert(vault_env_bootstrap_init() == 3);
   assert(vault_env_has_credential_environment() == 0);
   assert(getenv("AIMEE_DB2_URL") == NULL);
   assert(getenv("AIMEE_VAULT_PKCS11_PIN") == NULL);
   char value[128];
   assert(runtime_secret_get("AIMEE_DB2_URL", value, sizeof(value)) == 1);
   assert(strcmp(value, "postgresql://user:db-password@db/aimee") == 0);
   runtime_secret_wipe(value, sizeof(value));
   assert(runtime_secret_get("AIMEE_VAULT_PKCS11_PIN", value, sizeof(value)) == 1);
   assert(strcmp(value, "vaulted-hsm-pin") == 0);
   runtime_secret_wipe(value, sizeof(value));
   assert(runtime_secret_get("AIMEE_KB_CLIENT_PAM_USERNAME", value, sizeof(value)) == 1);
   assert(strcmp(value, "aimee-server") == 0);
   runtime_secret_wipe(value, sizeof(value));
   printf("  PASS: test_generic_env_source\n");
}

static void test_server_tls_key_first_boot_source(void)
{
   const char *name = "AIMEE_SERVER_TLS_PRIVATE_KEY";
   const char *pem =
       "-----BEGIN PRIVATE KEY-----\nvault-only-test-key\n-----END PRIVATE KEY-----\n";
   assert(vault_env_name_is_credential(name) == 1);
   setenv(name, pem, 1);
   assert(vault_env_bootstrap_init() == 1);
   assert(getenv(name) == NULL);
   char value[256];
   assert(vault_service_get_server_principal("__pki_server__", VAULT_API_KEY_CRED, value,
                                             sizeof(value)) == VAULT_OK);
   assert(strcmp(value, pem) == 0);
   OPENSSL_cleanse(value, sizeof(value));
   printf("  PASS: test_server_tls_key_first_boot_source\n");
}

static void test_management_tls_key_first_boot_sources(void)
{
   const char *server_name = "AIMEE_SERVER_MGMT_TLS_PRIVATE_KEY";
   const char *client_name = "AIMEE_SERVER_MGMT_STATUS_CLIENT_PRIVATE_KEY";
   assert(vault_env_name_is_credential(server_name) == 1);
   assert(vault_env_name_is_credential(client_name) == 1);
   setenv(server_name, "management-server-key-pem", 1);
   setenv(client_name, "management-status-client-key-pem", 1);
   assert(vault_env_bootstrap_init() == 2);
   assert(getenv(server_name) == NULL && getenv(client_name) == NULL);
   char value[128];
   assert(runtime_secret_get(server_name, value, sizeof(value)) == 1);
   assert(strcmp(value, "management-server-key-pem") == 0);
   OPENSSL_cleanse(value, sizeof(value));
   assert(runtime_secret_get(client_name, value, sizeof(value)) == 1);
   assert(strcmp(value, "management-status-client-key-pem") == 0);
   OPENSSL_cleanse(value, sizeof(value));
   printf("  PASS: test_management_tls_key_first_boot_sources\n");
}

static void test_webchat_first_boot_env_source(void)
{
   assert(vault_env_name_is_credential("AIMEE_WEBCHAT_USER") == 1);
   assert(vault_env_name_is_credential("AIMEE_WEBCHAT_USERS") == 1);
   assert(vault_env_name_is_credential("AIMEE_AGENT_SERVICE_BEARER") == 1);
   assert(vault_env_name_is_credential("AIMEE_TLS_CLIENT_P12_PASS") == 1);
   assert(vault_env_name_is_credential("AIMEE_VAULT_PKCS11_PIN") == 1);
   assert(vault_env_name_is_credential("AWS_SECRET_ACCESS_KEY") == 1);
   assert(vault_env_name_is_credential("GOOGLE_APPLICATION_CREDENTIALS") == 1);
   assert(vault_env_name_is_credential("DATABASE_URL") == 1);

   assert(vault_env_check_webchat_bootstrap() == -1);
   setenv("AIMEE_WEBCHAT_USER", "vault-admin", 1);
   assert(vault_env_bootstrap_init() == 1);
   assert(vault_env_check_webchat_bootstrap() == -1);
   setenv("AIMEE_WEBCHAT_PASSWORD", "vault-only-browser-password", 1);
   setenv("AIMEE_WEBCHAT_USERS", "alice:extra-password", 1);
   assert(vault_env_bootstrap_init() == 2);
   assert(vault_env_check_webchat_bootstrap() == 0);
   assert(getenv("AIMEE_WEBCHAT_USER") == NULL);
   assert(getenv("AIMEE_WEBCHAT_PASSWORD") == NULL);
   assert(getenv("AIMEE_WEBCHAT_USERS") == NULL);

   char value[128];
   assert(vault_service_get_server_principal("environment", "AIMEE_WEBCHAT_USER", value,
                                             sizeof(value)) == VAULT_OK);
   assert(strcmp(value, "vault-admin") == 0);
   OPENSSL_cleanse(value, sizeof(value));
   assert(vault_service_get_server_principal("environment", "AIMEE_WEBCHAT_PASSWORD", value,
                                             sizeof(value)) == VAULT_OK);
   assert(strcmp(value, "vault-only-browser-password") == 0);
   OPENSSL_cleanse(value, sizeof(value));
   assert(vault_service_get_server_principal("environment", "AIMEE_WEBCHAT_USERS", value,
                                             sizeof(value)) == VAULT_OK);
   assert(strcmp(value, "alice:extra-password") == 0);
   OPENSSL_cleanse(value, sizeof(value));
   printf("  PASS: test_webchat_first_boot_env_source\n");
}

static void test_streamed_first_boot_source(void)
{
   static const unsigned char records[] = "AIMEE_STREAM_API_KEY=stream-only-secret\0"
                                          "AIMEE_STREAM_MODE=non-secret-setting\0";
   FILE *input = tmpfile();
   assert(input != NULL);
   assert(fwrite(records, 1, sizeof(records) - 1, input) == sizeof(records) - 1);
   rewind(input);
   assert(vault_env_import_stream(input) == 1);
   fclose(input);

   /* The importer uses the same classifier as ordinary first-boot env. It does
    * not put unrelated host settings into the helper environment. */
   assert(getenv("AIMEE_STREAM_API_KEY") != NULL);
   assert(getenv("AIMEE_STREAM_MODE") == NULL);
   assert(vault_env_bootstrap_init() == 1);
   assert(getenv("AIMEE_STREAM_API_KEY") == NULL);
   char value[128];
   assert(runtime_secret_get("AIMEE_STREAM_API_KEY", value, sizeof(value)) == 1);
   assert(strcmp(value, "stream-only-secret") == 0);
   OPENSSL_cleanse(value, sizeof(value));

   /* A truncated stream is not accepted as a partial bootstrap. */
   static const char truncated[] = "AIMEE_TRUNCATED_TOKEN=must-not-import";
   input = tmpfile();
   assert(input != NULL);
   assert(fwrite(truncated, 1, sizeof(truncated) - 1, input) == sizeof(truncated) - 1);
   rewind(input);
   assert(vault_env_import_stream(input) == -1);
   fclose(input);
   assert(getenv("AIMEE_TRUNCATED_TOKEN") == NULL);
   printf("  PASS: test_streamed_first_boot_source\n");
}

static void test_oversized_credential_name_fails_closed(void)
{
   char name[180];
   memset(name, 'A', sizeof(name));
   memcpy(name + sizeof(name) - 10, "_API_KEY", 9);
   name[sizeof(name) - 1] = '\0';
   assert(setenv(name, "must-not-be-ignored", 1) == 0);
   assert(vault_env_has_credential_environment() == -1);
   assert(vault_env_bootstrap_init() == -1);
   assert(getenv(name) != NULL); /* failed input remains available for diagnosis/retry */
   unsetenv(name);
   printf("  PASS: test_oversized_credential_name_fails_closed\n");
}

static void test_kb_ingests_delegate_shaped_env_generically(void)
{
   const char *name = "AIMEE_DELEGATE_KEY_KB_ONLY";
   setenv(name, "must-live-only-in-kb-vault", 1);
   assert(vault_env_bootstrap_init_all() == 1);
   assert(getenv(name) == NULL);
   char value[128];
   assert(vault_service_get_server_principal("environment", name, value, sizeof(value)) ==
          VAULT_OK);
   assert(strcmp(value, "must-live-only-in-kb-vault") == 0);
   OPENSSL_cleanse(value, sizeof(value));
   printf("  PASS: test_kb_ingests_delegate_shaped_env_generically\n");
}

static void test_legacy_oauth_migration(void)
{
   char codex_dir[400], claude_dir[400], codex_path[480], claude_path[480];
   snprintf(codex_dir, sizeof(codex_dir), "%s/.codex", g_home);
   snprintf(claude_dir, sizeof(claude_dir), "%s/.claude", g_home);
   assert(mkdir(codex_dir, 0700) == 0);
   assert(mkdir(claude_dir, 0700) == 0);
   snprintf(codex_path, sizeof(codex_path), "%s/auth.json", codex_dir);
   snprintf(claude_path, sizeof(claude_path), "%s/.credentials.json", claude_dir);
   FILE *f = fopen(codex_path, "wb");
   assert(f != NULL);
   fputs("{\"access_token\":\"legacy-codex-token\"}", f);
   fclose(f);
   f = fopen(claude_path, "wb");
   assert(f != NULL);
   fputs("{\"accessToken\":\"legacy-claude-token\"}", f);
   fclose(f);

   assert(server_vault_bootstrap() == 2);
   char value[128];
   assert(vault_service_get_server_principal("codex", "oauth", value, sizeof(value)) == VAULT_OK);
   assert(strstr(value, "legacy-codex-token") != NULL);
   OPENSSL_cleanse(value, sizeof(value));
   assert(vault_service_get_server_principal("claude", "oauth", value, sizeof(value)) == VAULT_OK);
   assert(strstr(value, "legacy-claude-token") != NULL);
   OPENSSL_cleanse(value, sizeof(value));
   assert(access(codex_path, F_OK) != 0);
   assert(access(claude_path, F_OK) != 0);
   printf("  PASS: test_legacy_oauth_migration\n");
}

static void test_legacy_db1_oauth_migration(void)
{
   const char *client = "retired-mcp";
   char access_name[256], refresh_name[256], access_path[512], refresh_path[512];
   snprintf(access_name, sizeof(access_name), OAUTH_KEY_ACCESS_TOKEN, client);
   snprintf(refresh_name, sizeof(refresh_name), OAUTH_KEY_REFRESH_TOKEN, client);
   snprintf(access_path, sizeof(access_path), "%s/%s", config_output_dir(), access_name);
   snprintf(refresh_path, sizeof(refresh_path), "%s/%s", config_output_dir(), refresh_name);
   FILE *f = fopen(access_path, "wb");
   assert(f != NULL);
   fputs("legacy-db1-access-token\n", f);
   fclose(f);
   f = fopen(refresh_path, "wb");
   assert(f != NULL);
   fputs("legacy-db1-refresh-token\n", f);
   fclose(f);

   assert(server_vault_bootstrap() == 2);
   char value[128];
   assert(vault_service_get_server_principal(client, "oauth_access_token", value, sizeof(value)) ==
          VAULT_OK);
   assert(strcmp(value, "legacy-db1-access-token") == 0);
   OPENSSL_cleanse(value, sizeof(value));
   assert(vault_service_get_server_principal(client, "oauth_refresh_token", value, sizeof(value)) ==
          VAULT_OK);
   assert(strcmp(value, "legacy-db1-refresh-token") == 0);
   OPENSSL_cleanse(value, sizeof(value));
   assert(access(access_path, F_OK) != 0);
   assert(access(refresh_path, F_OK) != 0);
   printf("  PASS: test_legacy_db1_oauth_migration\n");
}

/* No secret source configured -> no-op (and no crash). */
static void test_no_source_noop(void)
{
   assert(getenv("AIMEE_DELEGATE_KEY_CLAUDE") == NULL);
   assert(getenv("AIMEE_FORGE_TOKEN") == NULL);
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
   assert(!plaintext_under_home("ghs-forge-DELTA"));
   assert(!plaintext_under_home("db-password"));
   assert(!plaintext_under_home("legacy-codex-token"));
   assert(!plaintext_under_home("legacy-claude-token"));
   assert(!plaintext_under_home("legacy-db1-access-token"));
   assert(!plaintext_under_home("legacy-db1-refresh-token"));
   assert(!plaintext_under_home("must-live-only-in-kb-vault"));
   assert(!plaintext_under_home("vault-only-browser-password"));
   assert(!plaintext_under_home("extra-password"));
   assert(!plaintext_under_home("vault-only-test-key"));
   assert(!plaintext_under_home("management-server-key-pem"));
   assert(!plaintext_under_home("management-status-client-key-pem"));
   assert(!plaintext_under_home("stream-only-secret"));
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

   test_idempotent_and_overwrite();
   test_env_source();
   test_forge_env_source();
   test_generic_env_source();
   test_server_tls_key_first_boot_source();
   test_management_tls_key_first_boot_sources();
   test_webchat_first_boot_env_source();
   test_streamed_first_boot_source();
   test_oversized_credential_name_fails_closed();
   test_kb_ingests_delegate_shaped_env_generically();
   test_legacy_oauth_migration();
   test_legacy_db1_oauth_migration();
   test_no_source_noop();
   test_no_plaintext_at_rest();

   vault_kek_cache_clear();
   runtime_secret_clear();
   char rm[400];
   snprintf(rm, sizeof(rm), "rm -rf %s", g_root);
   (void)system(rm);
   printf("vault_bootstrap: all tests passed\n");
   return 0;
}
