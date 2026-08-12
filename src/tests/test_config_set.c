/* test_config_set.c: the surgical config write (Proposal B write side). config_set
 * edits the config YAML as a document — sets one key, preserves every other — and
 * republishes, instead of re-serialising config_t and rebuilding the whole file.
 * The load-bearing property: a write does NOT drop other keys, including a key that
 * a whole-struct rebuild (config_save) would omit. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "aimee.h"
#include "platform_path.h"
#include "platform_test_util.h"

static char g_home[512];
static int g_secret_writes;
static int g_secret_was_configured;

static int test_secret_writer(const char *name, const char *value)
{
   assert(name && strcmp(name, "AIMEE_DB2_URL") == 0);
   g_secret_writes++;
   g_secret_was_configured = value && value[0] ? 1 : 0;
   return 0;
}

static void set_home(void)
{
   snprintf(g_home, sizeof(g_home), "%s/aimee-cfgset-XXXXXX", platform_tmpdir());
   char *d = platform_mkdtemp(g_home);
   assert(d);
   setenv("HOME", d, 1);
   unsetenv("AIMEE_HOME");
   setenv("AIMEE_NO_CACHE", "1", 1);
   char dir[600];
   snprintf(dir, sizeof(dir), "%s/.config", d);
   mkdir(dir, 0755);
   snprintf(dir, sizeof(dir), "%s/.config/aimee", d);
   mkdir(dir, 0755);
}

static void write_yaml(const char *yaml)
{
   char path[800];
   snprintf(path, sizeof(path), "%s/.config/aimee/aimee.yaml", g_home);
   FILE *f = fopen(path, "w");
   assert(f);
   fputs(yaml, f);
   fclose(f);
}

static char *read_yaml(void)
{
   char path[800];
   snprintf(path, sizeof(path), "%s/.config/aimee/aimee.yaml", g_home);
   FILE *f = fopen(path, "r");
   assert(f);
   static char buf[8192];
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   buf[n] = '\0';
   fclose(f);
   return buf;
}

int main(void)
{
   set_home();
   /* A file with a known key, another known key, and an EXTRA key that a whole-struct
    * rebuild from config_fields[] would not re-emit. */
   write_yaml("provider: claude\n"
              "default_persona: architect\n"
              "max_iterations: 5\n"
              "custom_note: keep-me\n");

   /* Set one key. */
   assert(config_set("provider", "openai") == 0);

   /* The document is patched: the changed key updated, every other key preserved. */
   char *y = read_yaml();
   assert(strstr(y, "openai") && "provider must be updated");
   assert(strstr(y, "architect") && "unrelated known key must survive");
   assert(strstr(y, "keep-me") && "an extra/unknown key must survive (patch, not rebuild)");

   /* And it loads back correctly, other fields intact. */
   assert(strcmp(config_provider(), "openai") == 0);
   assert(strcmp(config_default_persona(), "architect") == 0);
   assert(config_max_iterations() == 5);

   /* Typed writes + validation. */
   assert(config_set("autonomous", "true") == 0);
   assert(config_set("max_iterations", "12") == 0);
   assert(config_set("autonomous", "maybe") < 0 && "invalid bool rejected");
   assert(config_set("no_such_key_zzz", "x") < 0 && "unknown key rejected");
   assert(config_autonomous() == 1);
   assert(config_max_iterations() == 12);
   assert(strcmp(config_provider(), "openai") == 0 && "earlier write still present");

   /* Credential-shaped fields bypass the YAML document entirely. */
   config_secret_writer_set(test_secret_writer);
   assert(config_set("db2_url", "postgres://user:never-write-me@example/db") == 0);
   assert(g_secret_writes == 1 && g_secret_was_configured == 1);
   y = read_yaml();
   assert(strstr(y, "never-write-me") == NULL);
   assert(strstr(y, "db2_url") == NULL);

   printf("  PASS: config_set preserves ordinary keys and sends credentials only to Vault\n");
   return 0;
}
