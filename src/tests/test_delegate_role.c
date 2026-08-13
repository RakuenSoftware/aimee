/* test_delegate_role.c: unit tests for delegate role canonicalization */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <aimee/delegates/delegate_role.h>
#include "agent_types.h"
#include "config.h"             /* config_default_dir */
#include <stdlib.h>             /* mkdtemp */
#include <sys/stat.h>           /* mkdir */
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* delegate_agent_supports_role() now defers to the canonical agent_has_role()
 * (declared-role membership, `all` wildcard included). Stub it here — the real
 * definition lives in agent_route.o, which this unit test does not link. */
int agent_has_role(const agent_t *agent, const char *role)
{
   if (!agent || !role || !role[0])
      return 0;
   for (int i = 0; i < agent->role_count; i++)
      if (strcmp(agent->roles[i], "all") == 0 || strcmp(agent->roles[i], role) == 0)
         return 1;
   return 0;
}

/* role_template_max_turns() (reached via delegate_default_max_turns_for_role) reads
 * <config_default_dir()>/role_templates/<canonical-role>.md and parses `max_turns:`
 * from its frontmatter, returning -1 when the file is absent. Stub config_default_dir
 * at a temp dir and lay down the templates the inspection-policy assertions expect
 * (note: the "test" role canonicalizes to "validate"). */
static char g_roles_dir[256];
const char *config_default_dir(void)
{
   return g_roles_dir;
}
static void write_role_template(const char *canonical, int max_turns)
{
   char path[512];
   snprintf(path, sizeof(path), "%s/role_templates/%s.md", g_roles_dir, canonical);
   FILE *f = fopen(path, "w");
   assert(f);
   fprintf(f, "---\nmax_turns: %d\n---\nbody\n", max_turns);
   fclose(f);
}
static void setup_role_templates(void)
{
   snprintf(g_roles_dir, sizeof(g_roles_dir), "%s/aimee-test-roles-XXXXXX", platform_tmpdir());
   assert(mkdtemp(g_roles_dir));
   char sub[512];
   snprintf(sub, sizeof(sub), "%s/role_templates", g_roles_dir);
   assert(mkdir(sub, 0700) == 0);
   write_role_template("review", 20);
   write_role_template("validate", 12); /* "test" -> validate */
   write_role_template("diagnose", 16);
   /* no code.md -> role_template_max_turns returns -1 for "code" */
}

/* These tests used to assert the CONTENT of two tables that lived in C: which
 * aliases resolve where, and which roles ship. Both were copies of the module's,
 * agreeing with it by coincidence, and both are gone. What the answers ARE is
 * pinned against the module in server-go/modules/delegates/rolepolicy_test.go
 * and permissions_test.go.
 *
 * What is left here is the seam: that C asks, that it fails closed when nobody
 * answers, and that the half C genuinely owns -- a template file on disk -- still
 * works. */

static int g_policy_calls;
static int g_builtin_answer;

/* A canonicalizer that resolves exactly one alias, so a test can tell "the
 * module was asked and its answer used" from "C worked it out". */
static int role_seam_canonicalize(const char *role, char *out, size_t cap)
{
   if (!role || !out)
      return -1;
   snprintf(out, cap, "%s", strcmp(role, "alias-of-code") == 0 ? "code" : role);
   return 0;
}

static int role_seam_policy(int op, const char *role, int a, int b, int c, int *out)
{
   (void)role;
   (void)a;
   (void)b;
   (void)c;
   if (op != DELEGATE_ROLE_OP_BUILTIN || !out)
      return -1;
   g_policy_calls++;
   *out = g_builtin_answer;
   return 0;
}

static void install_role_seam(void)
{
   delegate_role_register_canonicalizer(role_seam_canonicalize);
   delegate_register_role_policy_provider(role_seam_policy);
}

/* With nobody to ask, a role has no canonical spelling and nothing is known.
 *
 * This is the direction that matters. The fallback that used to sit here made
 * the answer depend on whether the module was registered, which is the same
 * question answered twice; a refusal is at least the same answer every time. */
static void test_without_a_provider_nothing_is_known(void)
{
   delegate_role_register_canonicalizer(NULL);
   delegate_register_role_policy_provider(NULL);

   assert(delegate_role_canonicalize("code")[0] == '\0');
   assert(delegate_role_known(NULL, "code") == 0);
   assert(delegate_role_known(NULL, "implement") == 0);
   printf("  PASS: test_without_a_provider_nothing_is_known\n");
}

/* The module's answer is used verbatim, for the spelling and for whether the
 * role ships. */
static void test_the_module_names_the_role(void)
{
   install_role_seam();

   assert(strcmp(delegate_role_canonicalize("alias-of-code"), "code") == 0);

   g_builtin_answer = 1;
   g_policy_calls = 0;
   assert(delegate_role_known(NULL, "alias-of-code") == 1);
   assert(g_policy_calls == 1); /* asked, not assumed */

   g_builtin_answer = 0;
   assert(delegate_role_known(NULL, "not-a-role-anywhere") == 0);
   printf("  PASS: test_the_module_names_the_role\n");
}

/* And the half C owns: a template file makes a custom role known even when the
 * module says it is not one that ships. */
static void test_a_template_file_defines_a_custom_role(void)
{
   install_role_seam();
   g_builtin_answer = 0;

   char dir[512];
   snprintf(dir, sizeof(dir), "%s/role_templates", config_default_dir());
   char cmd[600];
   snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
   assert(system(cmd) == 0);

   char path[600];
   snprintf(path, sizeof(path), "%s/deployer.md", dir);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs("You deploy things. {{TASK}}\n", f);
   fclose(f);

   assert(delegate_role_known(NULL, "deployer") == 1);
   assert(delegate_role_known(NULL, "not-a-role-anywhere") == 0);

   remove(path);
   printf("  PASS: test_a_template_file_defines_a_custom_role\n");
}

/* The removed-role blacklist is still C's, and still rejects. */
static void test_culled_persona_roles_are_rejected(void)
{
   install_role_seam();
   g_builtin_answer = 1; /* even if the module claimed them, these are refused */

   static const char *const culled[] = {"prose",   "line-edit", "lyric", "hook",
                                        "prosody", "songform",  NULL};
   for (int i = 0; culled[i]; i++)
   {
      const char *why = delegate_role_removed_reason(culled[i]);
      assert(why != NULL && strstr(why, "removed") != NULL);
   }
   assert(delegate_role_removed_reason("code") == NULL);
   printf("  PASS: test_culled_persona_roles_are_rejected\n");
}

static void test_apply_max_turns_policy(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 3;

   /* Agent A: undeclared cap -> role floor applies. */
   snprintf(cfg.agents[0].roles[0], sizeof(cfg.agents[0].roles[0]), "review");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].max_turns = -1;

   /* Agent B: declared high cap (frontier) -> honored, NOT clamped to floor. */
   snprintf(cfg.agents[1].roles[0], sizeof(cfg.agents[1].roles[0]), "review");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].max_turns = 200;

   /* Agent C: declared 0 == unlimited -> honored, NOT rewritten to floor. */
   snprintf(cfg.agents[2].roles[0], sizeof(cfg.agents[2].roles[0]), "review");
   cfg.agents[2].role_count = 1;
   cfg.agents[2].max_turns = 0;

   /* Review role floor is 20; applied only to the undeclared agent. */
   delegate_apply_max_turns_policy(&cfg, "review", -1);
   assert(cfg.agents[0].max_turns == 20);  /* undeclared -> floor */
   assert(cfg.agents[1].max_turns == 200); /* declared high honored */
   assert(cfg.agents[2].max_turns == 0);   /* unlimited honored */

   /* Explicit per-invocation override forces every agent regardless. */
   delegate_apply_max_turns_policy(&cfg, "review", 5);
   assert(cfg.agents[0].max_turns == 5);
   assert(cfg.agents[1].max_turns == 5);
   assert(cfg.agents[2].max_turns == 5);

   printf("  PASS: test_apply_max_turns_policy\n");
}

static void test_apply_max_turns_cap(void)
{
   agent_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 5;
   for (int i = 0; i < 4; i++)
   {
      snprintf(cfg.agents[i].roles[0], sizeof(cfg.agents[i].roles[0]), "code");
      cfg.agents[i].role_count = 1;
   }
   cfg.agents[0].max_turns = -1; /* inherited unlimited */
   cfg.agents[1].max_turns = 0;  /* explicitly unlimited */
   cfg.agents[2].max_turns = 20; /* stricter agent cap */
   cfg.agents[3].max_turns = 200;
   snprintf(cfg.agents[4].roles[0], sizeof(cfg.agents[4].roles[0]), "review");
   cfg.agents[4].role_count = 1;
   cfg.agents[4].max_turns = 200;

   delegate_apply_max_turns_cap(&cfg, "code", 48);
   assert(cfg.agents[0].max_turns == 48);
   assert(cfg.agents[1].max_turns == 48);
   assert(cfg.agents[2].max_turns == 20);
   assert(cfg.agents[3].max_turns == 48);
   assert(cfg.agents[4].max_turns == 200); /* ineligible role untouched */

   printf("  PASS: test_apply_max_turns_cap\n");
}

int main(void)
{
   printf("test_delegate_role\n");
   setup_role_templates();
   test_without_a_provider_nothing_is_known();
   test_the_module_names_the_role();
   test_a_template_file_defines_a_custom_role();
   test_culled_persona_roles_are_rejected();
   test_apply_max_turns_policy();
   test_apply_max_turns_cap();
   printf("All tests passed.\n");
   return 0;
}
