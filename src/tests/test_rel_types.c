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
   test_enum_text();
   test_governance_rel_types();
   printf("rel_types: all tests passed\n");
   return 0;
}
