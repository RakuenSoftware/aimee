/* test_config_set_section.c: config_set_concurrency writes just the `concurrency:`
 * section of the config document (from cfg's arrays), preserving every other key —
 * the structured-write partner to config_set. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "aimee.h"
#include "platform_path.h"
#include "platform_test_util.h"

static char g_home[512];
static void fresh(void)
{
   snprintf(g_home, sizeof(g_home), "%s/aimee-setsec-XXXXXX", platform_tmpdir());
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
   char p[800];
   snprintf(p, sizeof(p), "%s/.config/aimee/aimee.yaml", d);
   FILE *f = fopen(p, "w");
   assert(f);
   fputs("provider: claude\ncustom_note: keep-me\n", f);
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
   fresh();
   config_t cfg;
   config_load(&cfg);
   snprintf(cfg.concurrency_per_model[0].key, sizeof(cfg.concurrency_per_model[0].key),
            "claude-opus");
   cfg.concurrency_per_model[0].limit = 3;
   cfg.concurrency_per_model_count = 1;

   assert(config_set_concurrency(&cfg) == 0);

   /* The concurrency section is written; unrelated keys are preserved. */
   char *y = slurp();
   assert(strstr(y, "concurrency") && "concurrency section written");
   assert(strstr(y, "per_model") && strstr(y, "claude-opus") && "per_model entry written");
   assert(strstr(y, "claude") && "provider preserved");
   assert(strstr(y, "keep-me") && "unknown key preserved (surgical, not whole-file rebuild)");

   /* It loads back. */
   config_t back;
   config_load(&back);
   assert(back.concurrency_per_model_count == 1);
   assert(strcmp(back.concurrency_per_model[0].key, "claude-opus") == 0);
   assert(back.concurrency_per_model[0].limit == 3);
   assert(strcmp(back.provider, "claude") == 0);

   printf("  PASS: config_set_concurrency writes the section surgically, preserves other keys\n");
   return 0;
}
