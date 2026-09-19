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

int rel_types_seed_count(void)
{
   return SEED_COUNT;
}

const rel_type_def_t *rel_types_seed_at(int i)
{
   return (i >= 0 && i < SEED_COUNT) ? &SEED_ONTOLOGY[i] : NULL;
}

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

/* ── Relation aliases ────────────────────────────────────────────────────────
 * Synonyms models reach for when naming a relation we already model. Every entry
 * must resolve to an ACTIVE seed rel_type — rel_types_self_validate() enforces
 * that, so a typo here fails the build's tests rather than silently misfiling
 * facts.
 *
 * Deliberately conservative: only labels that mean the SAME relation, never a
 * near-neighbour. "founded" is not member_of, "mentors" is not knows; those are
 * genuinely new relations and must keep staging as provisional so §7.2 can
 * decide on them. Folding them here would quietly destroy information. */
static const struct
{
   const char *alias;
   const char *canonical;
} SEED_ALIASES[] = {
    {"has_ip", "device_has_ip"},
    {"ip", "device_has_ip"},
    {"ip_address", "device_has_ip"},
    {"hostname", "has_hostname"},
    {"has_host", "has_hostname"},
    {"host_name", "has_hostname"},
    {"works_at", "works_for"},
    {"employed_by", "works_for"},
    {"employer", "works_for"},
    {"belongs_to", "member_of"},
    {"aka", "also_known_as"},
    {"alias", "also_known_as"},
    {"also_called", "also_known_as"},
    {"married_to", "spouse"},
    {"wife", "spouse"},
    {"husband", "spouse"},
    {"daughter", "child_of"},
    {"son", "child_of"},
    {"mother", "parent_of"},
    {"father", "parent_of"},
    {"mother_of", "parent_of"},
    {"father_of", "parent_of"},
    {"son_of", "child_of"},
    {"daughter_of", "child_of"},
    {"resides_in", "lives_in"},
    {"birthplace", "born_in"},
    {"governed_by", "linked_policy"},
    {"replaces", "supersedes"},
};

static const int SEED_ALIAS_COUNT = (int)(sizeof(SEED_ALIASES) / sizeof(SEED_ALIASES[0]));

int rel_types_alias_count(void)
{
   return SEED_ALIAS_COUNT;
}

const char *rel_types_alias_at(int i, const char **canonical_out)
{
   if (i < 0 || i >= SEED_ALIAS_COUNT)
      return NULL;
   if (canonical_out)
      *canonical_out = SEED_ALIASES[i].canonical;
   return SEED_ALIASES[i].alias;
}

void rel_type_canonicalize(const char *in, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   rel_type_normalize(in, out, out_len);
   if (!out[0])
      return;
   /* A real seed type is already canonical; never rewrite one. */
   for (int i = 0; i < SEED_COUNT; i++)
      if (strcmp(SEED_ONTOLOGY[i].rel_type, out) == 0)
         return;
   for (int i = 0; i < SEED_ALIAS_COUNT; i++)
      if (strcmp(SEED_ALIASES[i].alias, out) == 0)
      {
         snprintf(out, out_len, "%s", SEED_ALIASES[i].canonical);
         return;
      }
}

static int is_known_kind(memory_node_kind_t k)
{
   switch (k)
   {
   case NODE_FILE:
   case NODE_FUNCTION:
   case NODE_STRUCT:
   case NODE_MODULE:
   case NODE_BUG:
   case NODE_COMMIT:
   case NODE_PR:
   case NODE_DEVELOPER:
   case NODE_CONCEPT:
   case NODE_EVENT:
   case NODE_PERSON:
   case NODE_PLACE:
   case NODE_TIME_EXPR:
   case NODE_DEVICE:
   case NODE_ORG:
   case NODE_IP:
   case NODE_SCALAR:
   case NODE_OTHER:
      return 1;
   }
   return 0;
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

int rel_type_kind_allowed(const rel_type_def_t *def, int is_head, memory_node_kind_t kind)
{
   if (!def)
      return 0;
   const memory_node_kind_t *list = is_head ? def->head_kinds : def->tail_kinds;
   int n = is_head ? def->head_kind_count : def->tail_kind_count;
   for (int i = 0; i < n; i++)
      if (list[i] == NODE_OTHER || list[i] == kind)
         return 1;
   return 0;
}

/* Set equality over the small kind lists (order-independent, dup-tolerant). */
static int kind_sets_equal(const memory_node_kind_t *a, int an, const memory_node_kind_t *b, int bn)
{
   for (int i = 0; i < an; i++)
   {
      int found = 0;
      for (int j = 0; j < bn; j++)
         if (a[i] == b[j])
         {
            found = 1;
            break;
         }
      if (!found)
         return 0;
   }
   for (int j = 0; j < bn; j++)
   {
      int found = 0;
      for (int i = 0; i < an; i++)
         if (b[j] == a[i])
         {
            found = 1;
            break;
         }
      if (!found)
         return 0;
   }
   return 1;
}

int rel_types_self_validate(char *err, size_t errlen)
{
#define FAIL(...)                                                                                  \
   do                                                                                              \
   {                                                                                               \
      if (err && errlen)                                                                           \
         snprintf(err, errlen, __VA_ARGS__);                                                       \
      return -1;                                                                                   \
   } while (0)

   for (int i = 0; i < SEED_COUNT; i++)
   {
      const rel_type_def_t *d = &SEED_ONTOLOGY[i];
      if (!d->rel_type || !d->rel_type[0])
         FAIL("seed[%d]: empty rel_type", i);
      if (d->head_kind_count <= 0 || d->head_kind_count > REL_TYPE_MAX_KINDS ||
          d->tail_kind_count <= 0 || d->tail_kind_count > REL_TYPE_MAX_KINDS)
         FAIL("%s: bad kind count", d->rel_type);
      for (int k = 0; k < d->head_kind_count; k++)
         if (!is_known_kind(d->head_kinds[k]))
            FAIL("%s: unknown head kind %d", d->rel_type, (int)d->head_kinds[k]);
      for (int k = 0; k < d->tail_kind_count; k++)
         if (!is_known_kind(d->tail_kinds[k]))
            FAIL("%s: unknown tail kind %d", d->rel_type, (int)d->tail_kinds[k]);

      if (d->is_symmetric)
      {
         /* A symmetric type's inverse must be itself, and its head/tail kind sets
          * must match (the relation is over one kind population). */
         if (d->inverse_rel_type && strcmp(d->inverse_rel_type, d->rel_type) != 0)
            FAIL("%s: symmetric inverse must be itself (got %s)", d->rel_type, d->inverse_rel_type);
         if (!kind_sets_equal(d->head_kinds, d->head_kind_count, d->tail_kinds, d->tail_kind_count))
            FAIL("%s: symmetric head/tail kind sets differ", d->rel_type);
      }
      else if (d->inverse_rel_type)
      {
         /* A non-symmetric inverse must exist and have head/tail flipped. */
         const rel_type_def_t *inv = rel_types_seed_lookup(d->inverse_rel_type);
         if (!inv)
            FAIL("%s: inverse %s not in seed", d->rel_type, d->inverse_rel_type);
         if (inv->is_symmetric)
            FAIL("%s: inverse %s is symmetric (mismatch)", d->rel_type, d->inverse_rel_type);
         if (!kind_sets_equal(d->head_kinds, d->head_kind_count, inv->tail_kinds,
                              inv->tail_kind_count) ||
             !kind_sets_equal(d->tail_kinds, d->tail_kind_count, inv->head_kinds,
                              inv->head_kind_count))
            FAIL("%s: inverse %s head/tail not flipped", d->rel_type, d->inverse_rel_type);
         if (!inv->inverse_rel_type || strcmp(inv->inverse_rel_type, d->rel_type) != 0)
            FAIL("%s: inverse %s does not point back", d->rel_type, d->inverse_rel_type);
      }
   }

   /* Aliases: each must be normalized already, must NOT shadow a real seed type,
    * and must resolve to an ACTIVE one. A broken entry here would silently
    * misfile facts, so it fails the tests instead. */
   for (int i = 0; i < SEED_ALIAS_COUNT; i++)
   {
      const char *a = SEED_ALIASES[i].alias;
      const char *c = SEED_ALIASES[i].canonical;
      char norm[REL_TYPE_NAME_MAX];
      rel_type_normalize(a, norm, sizeof(norm));
      if (strcmp(norm, a) != 0)
         FAIL("alias %s is not in canonical form (%s)", a, norm);
      for (int j = 0; j < SEED_COUNT; j++)
         if (strcmp(SEED_ONTOLOGY[j].rel_type, a) == 0)
            FAIL("alias %s shadows a seed rel_type", a);
      const rel_type_def_t *target = rel_types_seed_lookup(c);
      if (!target)
         FAIL("alias %s -> %s: target is not a seed rel_type", a, c);
      if (target->status != REL_STATUS_ACTIVE)
         FAIL("alias %s -> %s: target is not active", a, c);
      for (int j = 0; j < i; j++)
         if (strcmp(SEED_ALIASES[j].alias, a) == 0)
            FAIL("alias %s is duplicated", a);
   }
   return 0;
#undef FAIL
}

const char *correction_behavior_to_text(correction_behavior_t b)
{
   switch (b)
   {
   case CORR_SUPERSEDE:
      return "supersede";
   case CORR_HARD_DELETE:
      return "hard_delete";
   case CORR_IMMUTABLE:
      return "immutable";
   }
   return "supersede";
}

correction_behavior_t correction_behavior_from_text(const char *s)
{
   if (s && strcmp(s, "hard_delete") == 0)
      return CORR_HARD_DELETE;
   if (s && strcmp(s, "immutable") == 0)
      return CORR_IMMUTABLE;
   return CORR_SUPERSEDE; /* default / unknown */
}

const char *rel_sensitivity_to_text(rel_sensitivity_t s)
{
   switch (s)
   {
   case SENS_NORMAL:
      return "normal";
   case SENS_PII:
      return "pii";
   case SENS_SECRET:
      return "secret";
   }
   return "pii";
}

rel_sensitivity_t rel_sensitivity_from_text(const char *s)
{
   if (s && strcmp(s, "normal") == 0)
      return SENS_NORMAL;
   if (s && strcmp(s, "secret") == 0)
      return SENS_SECRET;
   return SENS_PII; /* fail closed (§7): unknown/omitted -> pii */
}
