/* rel_types.c: the pure, DB-free core of the typed-relationship ontology
 * (proposal typed-fact §1 / P1). The in-code SEED_ONTOLOGY, name normalization,
 * kind validation, and ontology self-validation live here so they link and unit-
 * test without libpq. The live `rel_types` DB2 table overlay lives in
 * db2/rel_types_store.c. See rel_types.h. */
#include "rel_types.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ── Seed ontology ───────────────────────────────────────────────────────────
 * Identity/world relations aimee should understand out of the box. NODE_OTHER in
 * a kinds list is the ANY wildcard; NODE_SCALAR is a value object (age=30). Each
 * row is self-describing — the gate reads metadata, it does not hardcode rules.
 * This table is self-validated by rel_types_self_validate() (a unit test fails
 * the build if a row is inconsistent), so edits here are checked, not trusted. */
static const rel_type_def_t SEED_ONTOLOGY[] = {
    {"works_for",
     {NODE_PERSON},
     1,
     {NODE_ORG},
     1,
     0,
     NULL,
     CORR_SUPERSEDE,
     "work",
     SENS_NORMAL,
     0,
     REL_STATUS_ACTIVE},
    {"member_of",
     {NODE_PERSON},
     1,
     {NODE_ORG},
     1,
     0,
     NULL,
     CORR_SUPERSEDE,
     "work",
     SENS_NORMAL,
     0,
     REL_STATUS_ACTIVE},
    {"has_role",
     {NODE_PERSON},
     1,
     {NODE_SCALAR},
     1,
     0,
     NULL,
     CORR_SUPERSEDE,
     "work",
     SENS_NORMAL,
     0,
     REL_STATUS_ACTIVE},
    {"spouse",
     {NODE_PERSON},
     1,
     {NODE_PERSON},
     1,
     1,
     "spouse",
     CORR_SUPERSEDE,
     "family",
     SENS_PII,
     0,
     REL_STATUS_ACTIVE},
    {"knows",
     {NODE_PERSON},
     1,
     {NODE_PERSON},
     1,
     1,
     "knows",
     CORR_SUPERSEDE,
     "social",
     SENS_PII,
     0,
     REL_STATUS_ACTIVE},
    {"parent_of",
     {NODE_PERSON},
     1,
     {NODE_PERSON},
     1,
     0,
     "child_of",
     CORR_IMMUTABLE,
     "family",
     SENS_PII,
     1,
     REL_STATUS_ACTIVE},
    {"child_of",
     {NODE_PERSON},
     1,
     {NODE_PERSON},
     1,
     0,
     "parent_of",
     CORR_IMMUTABLE,
     "family",
     SENS_PII,
     1,
     REL_STATUS_ACTIVE},
    {"lives_in",
     {NODE_PERSON},
     1,
     {NODE_PLACE},
     1,
     0,
     NULL,
     CORR_SUPERSEDE,
     "identity",
     SENS_PII,
     0,
     REL_STATUS_ACTIVE},
    {"born_in",
     {NODE_PERSON},
     1,
     {NODE_PLACE},
     1,
     0,
     NULL,
     CORR_IMMUTABLE,
     "identity",
     SENS_PII,
     0,
     REL_STATUS_ACTIVE},
    {"located_in",
     {NODE_OTHER},
     1,
     {NODE_PLACE},
     1,
     0,
     NULL,
     CORR_SUPERSEDE,
     "geo",
     SENS_NORMAL,
     1,
     REL_STATUS_ACTIVE},
    {"device_has_ip",
     {NODE_DEVICE},
     1,
     {NODE_IP},
     1,
     0,
     NULL,
     CORR_SUPERSEDE,
     "network",
     SENS_NORMAL,
     0,
     REL_STATUS_ACTIVE},
    {"has_hostname",
     {NODE_DEVICE},
     1,
     {NODE_SCALAR},
     1,
     0,
     NULL,
     CORR_SUPERSEDE,
     "network",
     SENS_NORMAL,
     0,
     REL_STATUS_ACTIVE},
    {"age",
     {NODE_PERSON},
     1,
     {NODE_SCALAR},
     1,
     0,
     NULL,
     CORR_SUPERSEDE,
     "identity",
     SENS_PII,
     0,
     REL_STATUS_ACTIVE},
    /* also_known_as: alternate name/handle for an entity. §4's hard_delete
     * exemplar — a stale alias actively misleads, so a correction tombstones it
     * (suppress + supersede, still retained) rather than archiving it inert.
     * Tail is ANY (the alias label may be free-form). */
    {"also_known_as",
     {NODE_PERSON},
     1,
     {NODE_OTHER},
     1,
     0,
     NULL,
     CORR_HARD_DELETE,
     "identity",
     SENS_NORMAL,
     0,
     REL_STATUS_ACTIVE},
    /* Governance decision-record relations (P1): let a decision_log record
     * participate in the memory graph. Endpoints are ANY (decisions/policies are
     * not entity node kinds); decided_by targets a person. */
    {"supersedes",
     {NODE_OTHER},
     1,
     {NODE_OTHER},
     1,
     0,
     NULL,
     CORR_SUPERSEDE,
     "governance",
     SENS_NORMAL,
     0,
     REL_STATUS_ACTIVE},
    {"linked_policy",
     {NODE_OTHER},
     1,
     {NODE_OTHER},
     1,
     0,
     NULL,
     CORR_SUPERSEDE,
     "governance",
     SENS_NORMAL,
     0,
     REL_STATUS_ACTIVE},
    {"decided_by",
     {NODE_OTHER},
     1,
     {NODE_PERSON},
     1,
     0,
     NULL,
     CORR_SUPERSEDE,
     "governance",
     SENS_NORMAL,
     0,
     REL_STATUS_ACTIVE},
};

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
       "lives_in", "born_in",  "age",      "located_in", "has_hostname",
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
