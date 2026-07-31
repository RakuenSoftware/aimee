/* test_rel_types.c: the typed-relationship ontology seed + helpers (typed-fact
 * §1 / P1). The headline test is rel_types_self_validate() == 0 — a malformed
 * seed (unknown kind, broken symmetric/inverse) fails the build's tests. */
#include "rel_types.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_seed_self_validates(void)
{
   char err[256] = "";
   int rc = rel_types_self_validate(err, sizeof(err));
   if (rc != 0)
      fprintf(stderr, "seed self-validation failed: %s\n", err);
   assert(rc == 0);
   assert(rel_types_seed_count() > 0);
   printf("  PASS: test_seed_self_validates\n");
}

static void test_normalize(void)
{
   char out[REL_TYPE_NAME_MAX];
   rel_type_normalize("worksFor", out, sizeof(out));
   assert(strcmp(out, "works_for") == 0); /* camelCase boundary split */
   rel_type_normalize("deviceHasIp", out, sizeof(out));
   assert(strcmp(out, "device_has_ip") == 0);
   rel_type_normalize("Works-For", out, sizeof(out));
   assert(strcmp(out, "works_for") == 0);
   rel_type_normalize("  WORKS  FOR  ", out, sizeof(out));
   assert(strcmp(out, "works_for") == 0);
   rel_type_normalize("device__has--ip", out, sizeof(out));
   assert(strcmp(out, "device_has_ip") == 0);
   printf("  PASS: test_normalize\n");
}

static void test_seed_lookup_case_insensitive(void)
{
   assert(rel_types_seed_lookup("works_for") != NULL);
   assert(rel_types_seed_lookup("Works For") != NULL); /* normalized */
   assert(rel_types_seed_lookup("DEVICE-HAS-IP") != NULL);
   assert(rel_types_seed_lookup("definitely_not_a_seed_relation") == NULL);
   assert(rel_types_seed_lookup("") == NULL);
   assert(rel_types_seed_lookup(NULL) == NULL);

   const rel_type_def_t *spouse = rel_types_seed_lookup("spouse");
   assert(spouse && spouse->is_symmetric == 1);
   assert(spouse->sensitivity == SENS_PII);
   const rel_type_def_t *born = rel_types_seed_lookup("born_in");
   assert(born && born->correction_behavior == CORR_IMMUTABLE);
   printf("  PASS: test_seed_lookup_case_insensitive\n");
}

static void test_kind_allowed(void)
{
   const rel_type_def_t *wf = rel_types_seed_lookup("works_for");
   assert(rel_type_kind_allowed(wf, 1, NODE_PERSON) == 1);
   assert(rel_type_kind_allowed(wf, 1, NODE_DEVICE) == 0);
   assert(rel_type_kind_allowed(wf, 0, NODE_ORG) == 1);
   assert(rel_type_kind_allowed(wf, 0, NODE_PERSON) == 0);

   /* located_in head is ANY (NODE_OTHER wildcard). */
   const rel_type_def_t *li = rel_types_seed_lookup("located_in");
   assert(rel_type_kind_allowed(li, 1, NODE_DEVICE) == 1);
   assert(rel_type_kind_allowed(li, 1, NODE_PERSON) == 1);
   assert(rel_type_kind_allowed(li, 0, NODE_PLACE) == 1);
   assert(rel_type_kind_allowed(li, 0, NODE_SCALAR) == 0);
   assert(rel_type_kind_allowed(NULL, 1, NODE_PERSON) == 0);
   printf("  PASS: test_kind_allowed\n");
}

static void test_describe(void)
{
   char buf[128];

   /* The type signature is what extraction prompts need: a bare "device_has_ip"
    * leaves a model guessing, and its reasonable guesses (has_ip) are staged as
    * provisional Class-C edges rather than committed as validated Class-B ones. */
   const rel_type_def_t *ip = rel_types_seed_lookup("device_has_ip");
   assert(rel_types_describe(ip, buf, sizeof(buf)) > 0);
   assert(strcmp(buf, "device_has_ip (device->ip)") == 0);

   const rel_type_def_t *wf = rel_types_seed_lookup("works_for");
   rel_types_describe(wf, buf, sizeof(buf));
   assert(strcmp(buf, "works_for (person->org)") == 0);

   /* NODE_SCALAR reads as "value"; a NODE_OTHER wildcard collapses to "any". */
   const rel_type_def_t *hr = rel_types_seed_lookup("has_role");
   rel_types_describe(hr, buf, sizeof(buf));
   assert(strcmp(buf, "has_role (person->value)") == 0);

   const rel_type_def_t *li = rel_types_seed_lookup("located_in");
   rel_types_describe(li, buf, sizeof(buf));
   assert(strcmp(buf, "located_in (any->place)") == 0);

   assert(rel_types_describe(NULL, buf, sizeof(buf)) == 0);
   assert(rel_types_describe(wf, NULL, 0) == 0);

   /* Every seed row must render, and fit the 128-byte budget the prompt uses. */
   for (int i = 0; i < rel_types_seed_count(); i++)
   {
      int n = rel_types_describe(rel_types_seed_at(i), buf, sizeof(buf));
      assert(n > 0 && n < (int)sizeof(buf));
   }

   assert(strcmp(rel_types_kind_word(NODE_IP), "ip") == 0);
   assert(strcmp(rel_types_kind_word(NODE_SCALAR), "value") == 0);
   assert(strcmp(rel_types_kind_word(NODE_OTHER), "any") == 0);
   printf("  PASS: test_describe\n");
}

static void test_functional_classification(void)
{
   /* Single-valued: a new object supersedes the prior (commit-time §4 correction). */
   assert(rel_type_is_functional("lives_in") == 1);
   assert(rel_type_is_functional("works_for") == 1);
   assert(rel_type_is_functional("born_in") == 1);
   assert(rel_type_is_functional("has_role") == 1);
   assert(rel_type_is_functional("device_has_ip") == 1);
   /* Multi-valued: objects accumulate, no correction. */
   assert(rel_type_is_functional("knows") == 0);
   assert(rel_type_is_functional("member_of") == 0);
   assert(rel_type_is_functional("also_known_as") == 0);
   assert(rel_type_is_functional("parent_of") == 0);
   assert(rel_type_is_functional(NULL) == 0);
   printf("  PASS: test_functional_classification\n");
}

static void test_enum_text(void)
{
   assert(correction_behavior_from_text("immutable") == CORR_IMMUTABLE);
   assert(correction_behavior_from_text("hard_delete") == CORR_HARD_DELETE);
   assert(correction_behavior_from_text("garbage") == CORR_SUPERSEDE); /* default */
   assert(strcmp(correction_behavior_to_text(CORR_IMMUTABLE), "immutable") == 0);
   assert(rel_sensitivity_from_text("normal") == SENS_NORMAL);
   assert(rel_sensitivity_from_text("secret") == SENS_SECRET);
   assert(rel_sensitivity_from_text("garbage") == SENS_PII); /* fail closed */
   assert(rel_sensitivity_from_text(NULL) == SENS_PII);
   printf("  PASS: test_enum_text\n");
}

static void test_governance_rel_types(void)
{
   /* The P1 decision-record relations are registered in the seed. */
   assert(rel_types_seed_lookup("supersedes") != NULL);
   assert(rel_types_seed_lookup("linked_policy") != NULL);
   assert(rel_types_seed_lookup("decided_by") != NULL);
   printf("  PASS: test_governance_rel_types\n");
}

int main(void)
{
   test_seed_self_validates();
   test_normalize();
   test_seed_lookup_case_insensitive();
   test_kind_allowed();
   test_describe();
   test_functional_classification();
   test_enum_text();
   test_governance_rel_types();
   printf("rel_types: all tests passed\n");
   return 0;
}
