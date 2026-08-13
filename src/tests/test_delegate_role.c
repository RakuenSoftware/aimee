/* test_delegate_role.c: unit tests for delegate role canonicalization */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <aimee/delegates/delegate_role.h>
#include "agent_types.h"
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

/* The cull deleted the roles that only restated a persona. Writing prose or a
 * lyric is the `draft` action performed BY a novel/songwriter persona, so these
 * names must now be rejected outright rather than silently degrading to a
 * read-only delegate with a generic prompt. */
static void test_culled_persona_roles_are_rejected(void)
{
   static const char *const culled[] = {"prose", "line-edit", "lyric",
                                        "hook",  "prosody",   "songform"};
   for (size_t i = 0; i < sizeof(culled) / sizeof(culled[0]); i++)
   {
      assert(delegate_role_removed_reason(culled[i]) != NULL);
      /* And they must not linger as write roles, which would hand tool access
       * to a name routing will refuse. */
      assert(delegate_role_is_write(culled[i]) == 0);
   }
   /* Surviving roles are not swept up by the removal check. */
   assert(delegate_role_removed_reason("draft") == NULL);
   assert(delegate_role_removed_reason("continuity") == NULL);
   assert(delegate_role_removed_reason(NULL) == NULL);
   assert(delegate_role_removed_reason("") == NULL);
   printf("  PASS: test_culled_persona_roles_are_rejected\n");
}

/* An unknown role name used to be accepted verbatim and dispatched: no template
 * (generic prompt), no write classification (silently read-only), no agent role
 * an operator could grant. That is the same hazard the removed-role blacklist
 * guards, so the check has to be a positive list, not six special cases. */
static void test_unknown_roles_are_not_known(void)
{
   static const char *const unknown[] = {"bogusrole", "revieww", "delete-everything", "", NULL};
   for (int i = 0; unknown[i]; i++)
      assert(delegate_role_known(NULL, unknown[i]) == 0);
   assert(delegate_role_known(NULL, NULL) == 0);

   /* Culled names are not known either — dispatch must reach the removed-role
    * reason, never treat them as a live role. */
   static const char *const culled[] = {"prose", "line-edit", "lyric", "hook", NULL};
   for (int i = 0; culled[i]; i++)
      assert(delegate_role_known(NULL, culled[i]) == 0);
   printf("  PASS: test_unknown_roles_are_not_known\n");
}

/* Every shipped role, and every alias target, must be known. This is the drift
 * guard: adding an alias whose canonical name is not a real role would make the
 * alias dispatch-refused, and the failure would only show up in production. */
static void test_known_roles_cover_documented_and_aliased(void)
{
   static const char *const roles[] = {"review",    "validate",   "diagnose",   "code",
                                       "refactor",  "explain",    "draft",      "execute",
                                       "summarize", "format",     "search",     "reason",
                                       "plan",      "continuity", "beat-check", NULL};
   for (int i = 0; roles[i]; i++)
      assert(delegate_role_known(NULL, roles[i]) == 1);

   static const char *const aliases[] = {"implement",
                                         "build",
                                         "reviewer",
                                         "verifier",
                                         "test",
                                         "check",
                                         "evaluate",
                                         "inspect",
                                         "research",
                                         "enforce",
                                         "recall",
                                         "synthesize",
                                         "rank-fuse",
                                         "classify-score",
                                         "planner",
                                         "planning",
                                         "evaluate-optimize",
                                         NULL};
   for (int i = 0; aliases[i]; i++)
   {
      assert(delegate_role_known(NULL, aliases[i]) == 1);
      /* and the alias must resolve INTO the known set, not merely match it */
      assert(delegate_role_known(NULL, delegate_role_canonicalize(aliases[i])) == 1);
   }
   printf("  PASS: test_known_roles_cover_documented_and_aliased\n");
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
   test_culled_persona_roles_are_rejected();
   test_unknown_roles_are_not_known();
   test_known_roles_cover_documented_and_aliased();
   test_apply_max_turns_policy();
   test_apply_max_turns_cap();
   printf("All tests passed.\n");
   return 0;
}
