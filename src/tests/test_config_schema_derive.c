/* test_config_schema_derive.c: guards Proposal A step 2 — flat scalar keys are
 * validated from config_fields[] (the single source of truth) instead of a
 * duplicate config_schema[] row. Behavioral: load a yaml exercising
 *   - a flat scalar removed from config_schema[] (provider/autonomous/max_iterations),
 *   - a flat scalar that was never in config_schema[] (kb_evidence_emit_enabled),
 *   - cross_verify as a flat bool (flattened from the old object form),
 *   - a genuinely unknown key,
 * capture config_validate's stderr, and assert only the genuinely-unknown key is
 * reported. This catches a regression where removing a schema row makes a valid
 * flat key read as "unknown key".
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aimee.h"
#include "platform_path.h"
#include "platform_test_util.h"

static const char *YAML = "provider: openai\n"
                          "autonomous: true\n"
                          "max_iterations: 7\n"
                          "kb_evidence_emit_enabled: true\n"
                          "cross_verify: true\n"
                          "zzz_not_a_real_key: 1\n";

int main(void)
{
   char home[512];
   snprintf(home, sizeof(home), "%s/aimee-schemaderive-XXXXXX", platform_tmpdir());
   char *d = platform_mkdtemp(home);
   assert(d);
   setenv("HOME", d, 1);
   unsetenv("AIMEE_HOME");           /* config path is then $HOME/.config/aimee/aimee.yaml */
   setenv("AIMEE_NO_CACHE", "1", 1); /* bypass the stat-keyed config cache */
   char dir[600], path[800];
   snprintf(dir, sizeof(dir), "%s/.config", d);
   mkdir(dir, 0755);
   snprintf(dir, sizeof(dir), "%s/.config/aimee", d);
   mkdir(dir, 0755);
   snprintf(path, sizeof(path), "%s/aimee.yaml", dir);
   FILE *f = fopen(path, "w");
   assert(f);
   fputs(YAML, f);
   fclose(f);

   /* Capture config_validate's stderr, saving the real fd so we can restore it
    * robustly (freopen("/dev/tty") does not work in CI). */
   char errpath[900];
   snprintf(errpath, sizeof(errpath), "%s/stderr.txt", d);
   fflush(stderr);
   int saved_stderr = dup(STDERR_FILENO);
   assert(saved_stderr >= 0);
   FILE *cap = freopen(errpath, "w", stderr);
   assert(cap);
   /* Called for its SIDE EFFECT, not for values: config_load is what emits the
    * unknown-key diagnostics this test captures from stderr below. The accessor
    * assertions further down read the same load's result. */
   config_t cfg;
   config_load(&cfg);
   (void)cfg;
   fflush(stderr);
   dup2(saved_stderr, STDERR_FILENO); /* restore real stderr */
   close(saved_stderr);
   clearerr(stderr);

   FILE *ef = fopen(errpath, "r");
   assert(ef);
   char buf[8192];
   size_t n = fread(buf, 1, sizeof(buf) - 1, ef);
   buf[n] = '\0';
   fclose(ef);

   /* Flat keys (derived from config_fields[]) must NOT be reported unknown. */
   assert(!strstr(buf, "unknown key \"provider\"") &&
          "flat key must validate from config_fields[]");
   assert(!strstr(buf, "unknown key \"autonomous\""));
   assert(!strstr(buf, "unknown key \"max_iterations\""));
   assert(!strstr(buf, "unknown key \"kb_evidence_emit_enabled\""));
   /* Dual-form nested key stays schema-validated as an object. */
   /* cross_verify is now a flat bool (flattened from the old {enabled:} object);
    * it validates via the config_fields[] fallback with no unknown/type warning. */
   assert(!strstr(buf, "unknown key \"cross_verify\""));
   assert(!strstr(buf, "\"cross_verify\" expected"));
   /* A genuinely unknown key is still caught. */
   assert(strstr(buf, "unknown key \"zzz_not_a_real_key\"") &&
          "genuinely unknown keys must still be reported");

   /* And the flat values actually loaded. */
   assert(strcmp(config_provider(), "openai") == 0);
   assert(config_autonomous() == 1);
   assert(config_max_iterations() == 7);
   assert(config_cross_verify() == 1);

   printf("  PASS: flat keys validate from config_fields[]; unknown keys still caught\n");
   return 0;
}
