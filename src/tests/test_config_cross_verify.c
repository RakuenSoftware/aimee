/* test_config_cross_verify.c: cross_verify is flattened from the old
 * `cross_verify: {enabled: bool, verify_cmd, role, prompt}` object into flat
 * top-level keys (cross_verify bool + verify_cmd/verify_role/verify_prompt), so it
 * is get/set-able via config_set. The old object form is still parsed for
 * back-compat. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "aimee.h"
#include "platform_path.h"
#include "platform_test_util.h"

static char g_home[512];
static void fresh_home(void)
{
   snprintf(g_home, sizeof(g_home), "%s/aimee-cv-XXXXXX", platform_tmpdir());
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
static void put(const char *yaml)
{
   char p[800];
   snprintf(p, sizeof(p), "%s/.config/aimee/aimee.yaml", g_home);
   FILE *f = fopen(p, "w");
   assert(f);
   fputs(yaml, f);
   fclose(f);
}
static char *slurp(void)
{
   char p[800];
   snprintf(p, sizeof(p), "%s/.config/aimee/aimee.yaml", g_home);
   FILE *f = fopen(p, "r");
   assert(f);
   static char b[8192];
   size_t n = fread(b, 1, sizeof(b) - 1, f);
   b[n] = '\0';
   fclose(f);
   return b;
}

int main(void)
{

   /* New flat form parses. */
   fresh_home();
   put("cross_verify: true\nverify_cmd: /bin/check\nverify_role: reviewer\n");
   assert(config_cross_verify() == 1);
   assert(strcmp(config_verify_cmd(), "/bin/check") == 0);
   assert(strcmp(config_verify_role(), "reviewer") == 0);

   /* Old object form still parses (back-compat). */
   fresh_home();
   put("cross_verify:\n  enabled: true\n  verify_cmd: /old/cmd\n  role: qa\n  prompt: hi\n");
   assert(config_cross_verify() == 1 && "old object form must still set cross_verify");
   assert(strcmp(config_verify_cmd(), "/old/cmd") == 0 && "old object verify_cmd back-compat");
   assert(strcmp(config_verify_role(), "qa") == 0);
   assert(strcmp(config_verify_prompt(), "hi") == 0);

   /* config_set writes the flat form and it round-trips. */
   fresh_home();
   put("provider: claude\n");
   assert(config_set("cross_verify", "true") == 0);
   assert(config_set("verify_role", "security") == 0);
   char *y = slurp();
   assert(strstr(y, "cross_verify") && !strstr(y, "enabled") &&
          "config_set writes the flat cross_verify, not the object");
   assert(strstr(y, "claude") && "unrelated key preserved");
   assert(config_cross_verify() == 1);
   assert(strcmp(config_verify_role(), "security") == 0);

   printf("  PASS: cross_verify flat form + old-object back-compat + config_set\n");
   return 0;
}
