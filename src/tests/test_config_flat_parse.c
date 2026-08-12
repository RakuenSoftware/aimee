/* test_config_flat_parse.c: locks the string-guard behaviour preserved by the
 * table-driven flat parse (Proposal A, step 3). Genericised string fields use the
 * non-empty guard (an explicit "" leaves the default), matching the majority inline
 * form; css_render_command keeps its bespoke block because its default is non-empty
 * and an explicit "" is meaningful (disable rendering). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "aimee.h"
#include "platform_path.h"
#include "platform_test_util.h"

static void load_with(const char *yaml, config_t *out)
{
   char home[512];
   snprintf(home, sizeof(home), "%s/aimee-flatparse-XXXXXX", platform_tmpdir());
   char *d = platform_mkdtemp(home);
   assert(d);
   setenv("HOME", d, 1);
   unsetenv("AIMEE_HOME");
   setenv("AIMEE_NO_CACHE", "1", 1);
   char dir[600], path[800];
   snprintf(dir, sizeof(dir), "%s/.config", d);
   mkdir(dir, 0755);
   snprintf(dir, sizeof(dir), "%s/.config/aimee", d);
   mkdir(dir, 0755);
   snprintf(path, sizeof(path), "%s/aimee.yaml", dir);
   FILE *f = fopen(path, "w");
   assert(f);
   fputs(yaml, f);
   fclose(f);
   config_load(out);
}

int main(void)
{
   /* A present, non-empty value is applied. */
   config_t a;
   load_with("provider: openai\nmax_iterations: 9\nautonomous: true\n", &a);
   assert(strcmp(a.provider, "openai") == 0);
   assert(a.max_iterations == 9);
   assert(a.autonomous == 1);

   /* An explicit empty string for a genericised field leaves the default
    * (non-empty guard) — default_persona stays "engineer", not "".
    *
    * This used to demonstrate the property with `provider`, whose default was
    * "claude". That default is now EMPTY on purpose (a fresh install has no
    * primary until one is chosen), so provider can no longer show the guard —
    * an empty default is indistinguishable from a wiped one. The guard itself is
    * unchanged; this just picks a field that still has a non-empty default. */
   config_t b;
   load_with("default_persona: \"\"\n", &b);
   assert(strcmp(b.default_persona, "engineer") == 0 &&
          "empty-string yaml must not wipe a genericised string default");

   /* css_render_command keeps its inline any-string block: an explicit "" DOES
    * set it empty (the disable path), which the generic non-empty guard would not. */
   config_t c;
   load_with("css_render_command: \"\"\n", &c);
   assert(c.css_render_command[0] == '\0' &&
          "css_render_command inline block must still honour an explicit empty string");

   printf("  PASS: flat-parse string-guard behaviour preserved\n");
   return 0;
}
