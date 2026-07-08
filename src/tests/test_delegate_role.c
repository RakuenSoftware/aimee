/* test_delegate_role.c: unit tests for delegate role canonicalization */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "delegate_role.h"
#include "agent_types.h"
#include <stdlib.h>   /* mkdtemp */
#include <sys/stat.h> /* mkdir */

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
   snprintf(g_roles_dir, sizeof(g_roles_dir), "/tmp/aimee-test-roles-XXXXXX");
   assert(mkdtemp(g_roles_dir));
   char sub[512];
   snprintf(sub, sizeof(sub), "%s/role_templates", g_roles_dir);
   assert(mkdir(sub, 0700) == 0);
   write_role_template("review", 20);
   write_role_template("validate", 12); /* "test" -> validate */
   write_role_template("diagnose", 16);
   /* no code.md -> role_template_max_turns returns -1 for "code" */
}

static void test_canonical_roles_unchanged(void)
{
   static const char *canonical[] = {"code",    "review",   "validate", "diagnose",
                                     "execute", "refactor", "draft",    NULL};
   for (int i = 0; canonical[i]; i++)
   {
      const char *out = delegate_role_canonicalize(canonical[i]);
      assert(out == canonical[i]); /* pointer identity: no alias applied */
   }
   printf("  PASS: test_canonical_roles_unchanged\n");
}

static void test_implement_maps_to_code(void)
{
   const char *out = delegate_role_canonicalize("implement");
   assert(out != NULL);
   assert(strcmp(out, "code") == 0);
   printf("  PASS: test_implement_maps_to_code\n");
}

static void test_build_maps_to_code(void)
{
   const char *out = delegate_role_canonicalize("build");
   assert(out != NULL);
   assert(strcmp(out, "code") == 0);
   printf("  PASS: test_build_maps_to_code\n");
}

static void test_test_maps_to_validate(void)
{
   const char *out = delegate_role_canonicalize("test");
   assert(out != NULL);
   assert(strcmp(out, "validate") == 0);
   printf("  PASS: test_test_maps_to_validate\n");
}

static void test_check_maps_to_validate(void)
{
   const char *out = delegate_role_canonicalize("check");
   assert(out != NULL);
   assert(strcmp(out, "validate") == 0);
   printf("  PASS: test_check_maps_to_validate\n");
}

static void test_human_role_nouns_map_to_canonical_roles(void)
{
   assert(strcmp(delegate_role_canonicalize("reviewer"), "review") == 0);
   assert(strcmp(delegate_role_canonicalize("verifier"), "validate") == 0);
   printf("  PASS: test_human_role_nouns_map_to_canonical_roles\n");
}

static void test_planner_maps_to_plan(void)
{
   const char *out = delegate_role_canonicalize("planner");
   assert(out != NULL);
   assert(strcmp(out, "plan") == 0);
   out = delegate_role_canonicalize("planning");
   assert(out != NULL);
   assert(strcmp(out, "plan") == 0);
   printf("  PASS: test_planner_maps_to_plan\n");
}

static void test_charter_aliases_map_to_builtin_roles(void)
{
   assert(strcmp(delegate_role_canonicalize("synthesize"), "summarize") == 0);
   assert(strcmp(delegate_role_canonicalize("rank-fuse"), "reason") == 0);
   assert(strcmp(delegate_role_canonicalize("classify-score"), "reason") == 0);
   assert(strcmp(delegate_role_canonicalize("evaluate-optimize"), "validate") == 0);
   assert(strcmp(delegate_role_canonicalize("recall"), "search") == 0);
   assert(strcmp(delegate_role_canonicalize("enforce"), "execute") == 0);
   printf("  PASS: test_charter_aliases_map_to_builtin_roles\n");
}

static void test_unknown_role_unchanged(void)
{
   const char *role = "completely_unknown_role_xyz";
   const char *out = delegate_role_canonicalize(role);
   assert(out == role); /* pointer identity */
   printf("  PASS: test_unknown_role_unchanged\n");
}

static void test_null_role(void)
{
   const char *out = delegate_role_canonicalize(NULL);
   assert(out == NULL);
   printf("  PASS: test_null_role\n");
}

static void test_empty_role(void)
{
   const char *role = "";
   const char *out = delegate_role_canonicalize(role);
   assert(out == role); /* pointer identity, no alias for empty */
   printf("  PASS: test_empty_role\n");
}

static void test_is_write_code_role(void)
{
   assert(delegate_role_is_write("code") == 1);
   printf("  PASS: test_is_write_code_role\n");
}

static void test_is_write_refactor_role(void)
{
   assert(delegate_role_is_write("refactor") == 1);
   printf("  PASS: test_is_write_refactor_role\n");
}

static void test_is_write_implement_alias(void)
{
   assert(delegate_role_is_write("implement") == 1);
   printf("  PASS: test_is_write_implement_alias\n");
}

static void test_is_write_read_only_roles(void)
{
   assert(delegate_role_is_write("review") == 0);
   assert(delegate_role_is_write("validate") == 0);
   assert(delegate_role_is_write("diagnose") == 0);
   assert(delegate_role_is_write("execute") == 0);
   assert(delegate_role_is_write("draft") == 0);
   assert(delegate_role_is_write("planner") == 0);
   printf("  PASS: test_is_write_read_only_roles\n");
}

static void test_is_write_null_empty(void)
{
   assert(delegate_role_is_write(NULL) == 0);
   assert(delegate_role_is_write("") == 0);
   printf("  PASS: test_is_write_null_empty\n");
}

static void test_novel_roles(void)
{
   /* Write roles draft/edit manuscript files. */
   assert(delegate_role_is_write("prose") == 1);
   assert(delegate_role_is_write("line-edit") == 1);
   /* Read-only review roles. */
   assert(delegate_role_is_write("continuity") == 0);
   assert(delegate_role_is_write("beat-check") == 0);
   /* Read-only novel roles auto-enable tools like review/validate. */
   assert(delegate_role_auto_tools_for_invocation("continuity", -1, 0) == 1);
   assert(delegate_role_auto_tools_for_invocation("beat-check", 2, 0) == 1);
   assert(delegate_role_auto_tools_for_invocation("continuity", 1, 0) == 0);
   printf("  PASS: test_novel_roles\n");
}

static void test_songwriter_roles(void)
{
   /* Write roles draft/edit lyric files. */
   assert(delegate_role_is_write("lyric") == 1);
   assert(delegate_role_is_write("hook") == 1);
   /* Read-only review roles. */
   assert(delegate_role_is_write("prosody") == 0);
   assert(delegate_role_is_write("songform") == 0);
   /* Read-only songwriter roles auto-enable tools. */
   assert(delegate_role_auto_tools_for_invocation("prosody", -1, 0) == 1);
   assert(delegate_role_auto_tools_for_invocation("songform", 2, 0) == 1);
   printf("  PASS: test_songwriter_roles\n");
}

static void test_inspection_turn_policies(void)
{
   assert(delegate_default_max_turns_for_role("review") == 20);
   assert(delegate_default_max_turns_for_role("test") == 12);
   assert(delegate_default_max_turns_for_role("diagnose") == 16);
   assert(delegate_default_max_turns_for_role("code") == -1);
   assert(delegate_final_after_turns_for_role("review") == -1);
   assert(delegate_final_after_turns_for_role("validate") == 8);
   assert(delegate_final_after_turns_for_role("test") == 8);
   assert(delegate_final_after_turns_for_role("search") == 10);
   assert(delegate_final_after_turns_for_role("diagnose") == 12);
   assert(delegate_final_after_turns_for_role("inspect") == 12);
   assert(delegate_final_after_turns_for_role("plan") == -1);
   assert(delegate_final_after_turns_for_role("code") == -1);
   printf("  PASS: test_inspection_turn_policies\n");
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

static void test_auto_tools_policy(void)
{
   assert(delegate_role_auto_tools_for_invocation("diagnose", -1, 0) == 1);
   assert(delegate_role_auto_tools_for_invocation("validate", 2, 0) == 1);
   assert(delegate_role_auto_tools_for_invocation("diagnose", 1, 0) == 0);
   assert(delegate_role_auto_tools_for_invocation("inspect", 1, 0) == 0);
   assert(delegate_role_auto_tools_for_invocation("diagnose", 1, 1) == 1);
   assert(delegate_role_auto_tools_for_invocation("review", -1, 0) == 1);
   assert(delegate_role_auto_tools_for_invocation("review", 1, 0) == 0);
   printf("  PASS: test_auto_tools_policy\n");
}

int main(void)
{
   printf("test_delegate_role\n");
   setup_role_templates();
   test_canonical_roles_unchanged();
   test_implement_maps_to_code();
   test_build_maps_to_code();
   test_test_maps_to_validate();
   test_check_maps_to_validate();
   test_human_role_nouns_map_to_canonical_roles();
   test_planner_maps_to_plan();
   test_charter_aliases_map_to_builtin_roles();
   test_unknown_role_unchanged();
   test_null_role();
   test_empty_role();
   test_is_write_code_role();
   test_is_write_refactor_role();
   test_is_write_implement_alias();
   test_is_write_read_only_roles();
   test_is_write_null_empty();
   test_novel_roles();
   test_songwriter_roles();
   test_inspection_turn_policies();
   test_apply_max_turns_policy();
   test_auto_tools_policy();
   printf("All tests passed.\n");
   return 0;
}
