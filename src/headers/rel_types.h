/* rel_types.h: typed-relationship ontology for the typed-fact knowledge layer
 * (proposal typed-fact-knowledge-layer §1, phase P1).
 *
 * A rel_type is a self-describing relation definition — `works_for`, `spouse`,
 * `device_has_ip`, … — with subject/object kind constraints, symmetry, an
 * optional auto-enforced inverse, a correction policy, a PII sensitivity tier,
 * and a status. It is the single source of truth for *what relations mean*, used
 * by the typed-fact write gate (memory_fact_gate) to validate candidate triples
 * before they are committed as `semantic` edges in entity_edges.
 *
 * Two ontology sources exist (mirroring the config-table registry-cache idiom):
 *   - the in-code SEED_ONTOLOGY here, always available (DB-outage fallback), and
 *   - the live `rel_types` DB2 table (db2/rel_types.c) which overlays/extends the
 *     seed and can carry operator/promoted (§2) types.
 * This header is the pure, DB-free core: the seed table, name normalization, kind
 * validation, and ontology self-validation. It links without libpq so it is unit-
 * testable in isolation.
 *
 * Entity kinds reuse memory_node_kind_t (memory_ontology.h); NODE_OTHER is the
 * ANY wildcard (matching the existing schema-rule convention) and NODE_SCALAR is
 * a value-typed object (age=30). */
#ifndef DEC_REL_TYPES_H
#define DEC_REL_TYPES_H 1

#include <stddef.h>
#include "memory_ontology.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* §4 correction policy: what happens when a new assertion contradicts an
    * existing one for this relation. NOT NULL in the table, default supersede. */
   typedef enum
   {
      CORR_SUPERSEDE = 0,   /* archive the old, assert the new (default) */
      CORR_HARD_DELETE = 1, /* tombstone the old (suppressed flag) */
      CORR_IMMUTABLE = 2,   /* reject the change; the fact is fixed (born_in) */
   } correction_behavior_t;

   /* §7 PII gating tier. NOT NULL, defaults to the restrictive `pii` so a
    * seed-omitted/learned type fails closed (withheld from injection). */
   typedef enum
   {
      SENS_NORMAL = 0,
      SENS_PII = 1,
      SENS_SECRET = 2,
   } rel_sensitivity_t;

   typedef enum
   {
      REL_STATUS_ACTIVE = 0,
      REL_STATUS_PROVISIONAL = 1, /* staged novel type pending promotion (§2) */
   } rel_status_t;

#define REL_TYPE_NAME_MAX  64
#define REL_TYPE_MAX_KINDS 8

   typedef struct
   {
      const char *rel_type; /* canonical lower snake_case key */
      /* Allowed subject/object entity kinds. NODE_OTHER in the list means ANY. */
      memory_node_kind_t head_kinds[REL_TYPE_MAX_KINDS];
      int head_kind_count;
      memory_node_kind_t tail_kinds[REL_TYPE_MAX_KINDS];
      int tail_kind_count;
      int is_symmetric;             /* one assertion implies both directions */
      const char *inverse_rel_type; /* auto-enforced inverse, or NULL */
      correction_behavior_t correction_behavior;
      const char *category; /* family | network | work | identity | … */
      rel_sensitivity_t sensitivity;
      int is_hierarchy_rel; /* participates in classification chains */
      rel_status_t status;
   } rel_type_def_t;

   /* Normalize a relation label to the canonical convention: lower-cased,
    * non-alphanumeric runs collapsed to single '_', leading/trailing '_'
    * stripped (so "worksFor", "Works For", "works-for" all -> "works_for").
    * Writes a NUL-terminated string into out (<= REL_TYPE_NAME_MAX). */
   void rel_type_normalize(const char *in, char *out, size_t out_len);

   /* Look up a rel_type in the in-code SEED_ONTOLOGY by (normalized) name.
    * Case/format-insensitive. Returns NULL if not a seed type. */
   const rel_type_def_t *rel_types_seed_lookup(const char *rel_type);

   /* Seed iteration (used to upsert the seed into the DB2 table). */
   int rel_types_seed_count(void);
   const rel_type_def_t *rel_types_seed_at(int i);

   /* Is `kind` permitted in the head (is_head=1) or tail (is_head=0) slot of
    * `def`? NODE_OTHER in the def's list is the ANY wildcard. */
   int rel_type_kind_allowed(const rel_type_def_t *def, int is_head, memory_node_kind_t kind);

   /* Is `rel_type` single-valued (functional)? A functional relation's new object
    * contradicts any prior object for the same subject, so the commit path applies
    * the relation's correction_behavior (supersede/hard-delete/reject-if-immutable).
    * Multi-valued relations (knows, member_of, parent_of, also_known_as, ...)
    * accumulate objects and are not corrected. */
   int rel_type_is_functional(const char *rel_type);

   /* Validate the SEED_ONTOLOGY for internal consistency (R1-D1): every head/tail
    * kind is a known entity kind; a symmetric type's inverse is itself and its
    * head/tail kind sets match; a non-symmetric type's declared inverse exists in
    * the seed with head/tail flipped. Returns 0 if the whole seed is consistent,
    * -1 on the first violation (a description is written to err when err!=NULL).
    * Exercised by a unit test so a malformed seed fails the build's tests. */
   int rel_types_self_validate(char *err, size_t errlen);

   /* enum <-> text (for the DB2 table columns + CLI). from_text is tolerant:
    * unknown correction -> CORR_SUPERSEDE; unknown sensitivity -> SENS_PII
    * (fail closed, §1/§7). */
   const char *correction_behavior_to_text(correction_behavior_t b);
   correction_behavior_t correction_behavior_from_text(const char *s);
   const char *rel_sensitivity_to_text(rel_sensitivity_t s);
   rel_sensitivity_t rel_sensitivity_from_text(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* DEC_REL_TYPES_H */
