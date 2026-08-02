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
    /* SYMMETRIC. If X is also known as Y then Y is also known as X — this is the
     * identity relation, and an alias that only resolves in one direction is the
     * one thing it must not be. It was declared asymmetric, so `A also_known_as
     * B` and `B also_known_as A` were stored as two unrelated edges and a lookup
     * on one alias could not surface the other. Measured on the tier-A 10k run:
     * 50 extractions stated the pair in the opposite order and were counted
     * wrong, which is how this surfaced.
     *
     * head_kinds must equal tail_kinds for a symmetric type — rel_types_validate
     * FAILS the build otherwise — and the head was NODE_PERSON while the tail was
     * NODE_OTHER. Aliases are not person-only in practice: the benchmark aliases
     * files and versions, and kb_memory_facts coerces a rejected kind to
     * head_kinds[0], so a person-only head silently retyped every file alias as a
     * person. NODE_OTHER on both sides is both what symmetry requires and what
     * the data already was. */
    {"also_known_as",
     {NODE_OTHER},
     1,
     {NODE_OTHER},
     1,
     1,
     "also_known_as",
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
    /* Commercial and deployment relations. Added because the ontology did not
     * cover the domain and the gap was being filled by the model inventing a
     * word for each one: measured over two 1k extraction runs, 22-24% of every
     * extracted fact used a non-seed predicate, 89 distinct ones, and 23 of
     * those recurred often enough for §7.2 auto-promotion (threshold 3) to make
     * them active. That would have grown the ontology to ~40 relations, most of
     * them near-synonyms — hosting facts alone split four ways across runs_on,
     * has_hostname, operates and hosts.
     *
     * Seeding the relations the domain actually needs is the difference between
     * the model landing on a shared predicate and the promoter admitting
     * whichever synonym it happened to produce first. */
    {"customer_of",
     {NODE_ORG},
     1,
     {NODE_OTHER},
     1,
     0,
     NULL,
     CORR_SUPERSEDE,
     "work",
     SENS_NORMAL,
     0,
     REL_STATUS_ACTIVE},
    {"subscription_tier",
     {NODE_ORG},
     1,
     {NODE_SCALAR},
     1,
     0,
     NULL,
     /* Single-valued: a new tier replaces the old one rather than accumulating.
      * Also listed in rel_type_is_functional(). */
     CORR_SUPERSEDE,
     "work",
     SENS_NORMAL,
     0,
     REL_STATUS_ACTIVE},
    {"owns_account",
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
    {"purchased",
     {NODE_OTHER},
     1,
     {NODE_OTHER},
     1,
     0,
     NULL,
     /* Multi-valued: an acquisition is a historical event and a second purchase
      * does not undo the first. Accumulation is governed by absence from
      * rel_type_is_functional(), not by correction_behavior. */
     CORR_SUPERSEDE,
     "work",
     SENS_NORMAL,
     0,
     REL_STATUS_ACTIVE},
    {"founded",
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
    {"mentors",
     {NODE_PERSON},
     1,
     {NODE_OTHER},
     1,
     0,
     NULL,
     CORR_SUPERSEDE,
     "work",
     SENS_NORMAL,
     0,
     REL_STATUS_ACTIVE},
    /* DEPLOYMENT, and deliberately NOT folded onto has_hostname or onto "runs".
     * "wol-realm runs on wol-realm-dev-9" says where a service is deployed;
     * has_hostname says what a thing is called; "Northwind Foods runs Lantern
     * Gateway" says who operates a business. Three different facts that read
     * similarly, and conflating them is defect 33.
     *
     * The tail is a DEVICE, not ANY, because runs_on and has_hostname sit at
     * different levels of the same chain:
     *
     *     service --runs_on--> host --has_hostname--> "wol-realm-dev-9"
     *
     * The object of runs_on is the host itself (named, as hosts always are, by
     * its hostname); the object of has_hostname is the name. Typing the tail ANY
     * would let a deployment point at a bare string and lose that distinction,
     * which is the same conflation defect 33 is about. Head stays ANY: the thing
     * deployed may be a service, a container or a job, and none of those are
     * entity kinds the ontology models. */
    {"runs_on",
     {NODE_OTHER},
     1,
     {NODE_DEVICE},
     1,
     0,
     NULL,
     CORR_SUPERSEDE,
     "network",
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
    /* Folds for the commercial/deployment seeds. Each one is a synonym the
     * extractor actually produced over two 1k runs, not a guess. Deliberately
     * NOT folded: "owns" (too generic outside this domain), "operates" and
     * "runs" (those mean running a BUSINESS -- "Northwind Foods runs Lantern
     * Gateway" -- not running ON a host), and "contributes_to" (contributing is
     * not membership). Folding those would trade one wrong answer for another. */
    {"has_tier", "subscription_tier"},
    {"tier", "subscription_tier"},
    {"subscription_level", "subscription_tier"},
    {"plan_tier", "subscription_tier"},
    {"account_owner", "owns_account"},
    {"owns_account_for", "owns_account"},
    {"is_customer_of", "customer_of"},
    {"customer", "customer_of"},
    {"acquired", "purchased"},
    {"bought", "purchased"},
    {"founder_of", "founded"},
    {"mentor_of", "mentors"},
    {"deployed_on", "runs_on"},
    {"hosted_on", "runs_on"},
    {"runs_at", "runs_on"},
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
       /* One tier at a time: an upgrade replaces the previous tier rather than
        * adding to it. runs_on is deliberately NOT here — a service can be
        * deployed on several hosts at once — and neither are owns_account or
        * customer_of, which accumulate by nature. */
       "subscription_tier",
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
