/* Remaining native typed-relationship helpers, pending Go migration.
 * Seed metadata is generated from the shared Go memory owner; edit
 * server-go/modules/memory/ontology_seed.go and run aimee-memory-seed. */
#include "rel_types.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* BEGIN GO MEMORY ONTOLOGY SEED */
// clang-format off
static const rel_type_def_t SEED_ONTOLOGY[] = {
    {"has_distance", {99, 0, 0, 0, 0, 0, 0, 0}, 1, {16, 0, 0, 0, 0, 0, 0, 0}, 1, 0, NULL, 0, "measurement", 0, 0, 0},
    {"can_lift", {99, 0, 0, 0, 0, 0, 0, 0}, 1, {16, 0, 0, 0, 0, 0, 0, 0}, 1, 0, NULL, 0, "measurement", 0, 0, 0},
    {"has_height", {99, 0, 0, 0, 0, 0, 0, 0}, 1, {16, 0, 0, 0, 0, 0, 0, 0}, 1, 0, NULL, 0, "measurement", 0, 0, 0},
    {"works_for", {10, 0, 0, 0, 0, 0, 0, 0}, 1, {14, 0, 0, 0, 0, 0, 0, 0}, 1, 0, NULL, 0, "work", 0, 0, 0},
    {"member_of", {10, 0, 0, 0, 0, 0, 0, 0}, 1, {14, 0, 0, 0, 0, 0, 0, 0}, 1, 0, NULL, 0, "work", 0, 0, 0},
    {"has_role", {10, 0, 0, 0, 0, 0, 0, 0}, 1, {16, 0, 0, 0, 0, 0, 0, 0}, 1, 0, NULL, 0, "work", 0, 0, 0},
    {"spouse", {10, 0, 0, 0, 0, 0, 0, 0}, 1, {10, 0, 0, 0, 0, 0, 0, 0}, 1, 1, "spouse", 0, "family", 1, 0, 0},
    {"knows", {10, 0, 0, 0, 0, 0, 0, 0}, 1, {10, 0, 0, 0, 0, 0, 0, 0}, 1, 1, "knows", 0, "social", 1, 0, 0},
    {"parent_of", {10, 0, 0, 0, 0, 0, 0, 0}, 1, {10, 0, 0, 0, 0, 0, 0, 0}, 1, 0, "child_of", 2, "family", 1, 1, 0},
    {"child_of", {10, 0, 0, 0, 0, 0, 0, 0}, 1, {10, 0, 0, 0, 0, 0, 0, 0}, 1, 0, "parent_of", 2, "family", 1, 1, 0},
    {"lives_in", {10, 0, 0, 0, 0, 0, 0, 0}, 1, {11, 0, 0, 0, 0, 0, 0, 0}, 1, 0, NULL, 0, "identity", 1, 0, 0},
    {"born_in", {10, 0, 0, 0, 0, 0, 0, 0}, 1, {11, 0, 0, 0, 0, 0, 0, 0}, 1, 0, NULL, 2, "identity", 1, 0, 0},
    {"located_in", {99, 0, 0, 0, 0, 0, 0, 0}, 1, {11, 0, 0, 0, 0, 0, 0, 0}, 1, 0, NULL, 0, "geo", 0, 1, 0},
    {"device_has_ip", {13, 0, 0, 0, 0, 0, 0, 0}, 1, {15, 0, 0, 0, 0, 0, 0, 0}, 1, 0, NULL, 0, "network", 0, 0, 0},
    {"has_hostname", {13, 0, 0, 0, 0, 0, 0, 0}, 1, {16, 0, 0, 0, 0, 0, 0, 0}, 1, 0, NULL, 0, "network", 0, 0, 0},
    {"age", {10, 0, 0, 0, 0, 0, 0, 0}, 1, {16, 0, 0, 0, 0, 0, 0, 0}, 1, 0, NULL, 0, "identity", 1, 0, 0},
    {"also_known_as", {10, 0, 0, 0, 0, 0, 0, 0}, 1, {99, 0, 0, 0, 0, 0, 0, 0}, 1, 0, NULL, 1, "identity", 0, 0, 0},
    {"supersedes", {99, 0, 0, 0, 0, 0, 0, 0}, 1, {99, 0, 0, 0, 0, 0, 0, 0}, 1, 0, NULL, 0, "governance", 0, 0, 0},
    {"linked_policy", {99, 0, 0, 0, 0, 0, 0, 0}, 1, {99, 0, 0, 0, 0, 0, 0, 0}, 1, 0, NULL, 0, "governance", 0, 0, 0},
    {"decided_by", {99, 0, 0, 0, 0, 0, 0, 0}, 1, {10, 0, 0, 0, 0, 0, 0, 0}, 1, 0, NULL, 0, "governance", 0, 0, 0},
};
// clang-format on
/* END GO MEMORY ONTOLOGY SEED */

static const int SEED_COUNT = (int)(sizeof(SEED_ONTOLOGY) / sizeof(SEED_ONTOLOGY[0]));

void rel_type_normalize(const char *in, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   size_t o = 0;
   int prev_us = 1;             /* leading-underscore suppression */
   int prev_lower_or_digit = 0; /* for camelCase boundary detection */
   for (const char *p = in ? in : ""; *p && o + 1 < out_len; p++)
   {
      unsigned char c = (unsigned char)*p;
      if (isalnum(c))
      {
         /* camelCase boundary: an uppercase letter following a lowercase letter
          * or digit starts a new word ("worksFor" -> "works_for"). */
         if (isupper(c) && prev_lower_or_digit && !prev_us && o + 1 < out_len)
            out[o++] = '_';
         if (o + 1 < out_len)
            out[o++] = (char)tolower(c);
         prev_us = 0;
         prev_lower_or_digit = (islower(c) || isdigit(c));
      }
      else if (!prev_us)
      {
         out[o++] = '_';
         prev_us = 1;
         prev_lower_or_digit = 0;
      }
   }
   while (o > 0 && out[o - 1] == '_') /* strip trailing */
      o--;
   out[o] = '\0';
}

const rel_type_def_t *rel_types_seed_lookup(const char *rel_type)
{
   if (!rel_type || !rel_type[0])
      return NULL;
   char norm[REL_TYPE_NAME_MAX];
   rel_type_normalize(rel_type, norm, sizeof(norm));
   for (int i = 0; i < SEED_COUNT; i++)
      if (strcmp(SEED_ONTOLOGY[i].rel_type, norm) == 0)
         return &SEED_ONTOLOGY[i];
   return NULL;
}

/* Single-valued (functional) relations: a new object supersedes/replaces the prior
 * for the same subject (via correction_behavior). Kept explicit in one place so the
 * set is reviewed together; multi-valued relations (knows, member_of, parent_of,
 * child_of, also_known_as, ...) accumulate and are absent here. */
int rel_type_is_functional(const char *rel_type)
{
   if (!rel_type)
      return 0;
   static const char *const functional[] = {
       "lives_in", "born_in",   "age",      "located_in",    "has_hostname",
       "spouse",   "works_for", "has_role", "device_has_ip",
   };
   for (size_t i = 0; i < sizeof(functional) / sizeof(functional[0]); i++)
      if (strcmp(rel_type, functional[i]) == 0)
         return 1;
   return 0;
}
