/* fact_lifecycle.h: provenance-keyed confidence classes (typed-fact §5) +
 * declarative correction / retraction (§4), over the semantic edges written
 * through db2_fact_commit. P3.
 *
 * §5 stores a confidence_class (A/B/C) + numeric confidence on each semantic
 * edge. Class is provenance-keyed: a user assertion earns A (permanent, wins all
 * conflicts), an ontology-consistent model inference earns B, model speculation /
 * a novel rel_type earns C (expires unless reinforced). Promotion (B -> durable,
 * never to A) and expiry (C after a TTL) are maintenance modes.
 *
 * §4 applies a rel_type's correction_behavior to a retraction: supersede stamps
 * superseded_at (transaction-time close, keeping the row), hard_delete tombstones
 * via a suppressed flag (still retained, per "always keep the origin artifact"),
 * immutable refuses model/inferred edits — but a user authority always wins. */
#ifndef DEC_DB2_FACT_LIFECYCLE_H
#define DEC_DB2_FACT_LIFECYCLE_H 1

#include "memory_fact_gate.h" /* fact_gate_verdict_t */

#ifdef __cplusplus
extern "C"
{
#endif

   /* Provenance-keyed confidence classes (§5). Ranked A > B > C. */
#define FACT_CLASS_A "A" /* user-stated; conf 1.0; wins all conflicts */
#define FACT_CLASS_B "B" /* model-inferred, ontology-consistent; 0.6-0.8 */
#define FACT_CLASS_C "C" /* model speculation / novel rel_type; 0.4; expires */

   /* Write / correction authority. A user assertion always wins (§4 R1-B1). */
   typedef enum
   {
      FACT_AUTHORITY_MODEL = 0,
      FACT_AUTHORITY_USER = 1,
   } fact_authority_t;

   /* Default numeric confidence for a class string (A=1.0, B=0.6, C=0.4; any
    * other / NULL -> 0.4, the conservative floor). */
   double fact_class_confidence(const char *cls);

   /* Rank of a class for conflict resolution: A=3, B=2, C=1, else 0. */
   int fact_class_rank(const char *cls);

   /* The class a write earns from its authority + gate verdict: a NOVEL rel_type
    * is always Class C (speculation, even from a user); otherwise a user authority
    * earns A and a model ACCEPT earns B. Returns a static FACT_CLASS_* string.
    * This is the single source of truth — callers route NOVEL through it too. */
   const char *fact_class_for(fact_authority_t authority, fact_gate_verdict_t verdict);

   /* §5 maintenance modes (counterparts to memory_run_maintenance promote/expire).
    *
    * Supersede unconfirmed (weight <= 1) Class C semantic edges asserted more than
    * ttl_days ago and still active (superseded_at = ''). The stamp is set so the
    * row is retained and remains a transaction-time query target. ttl_days <= 0 is
    * rejected. Returns the count expired (>=0), or -1 on error. */
   int db2_fact_expire_speculative(int ttl_days);

   /* Promote Class B semantic edges confirmed >= threshold times (weight) to
    * durable: confidence raised to 0.8. Never promotes to Class A (§5 R2-3).
    * threshold <= 0 is rejected. Returns the count promoted (>=0), or -1. */
   int db2_fact_promote_durable(int threshold);

   /* §4 retraction. Negative return = not applied. */
#define FACT_RETRACT_IMMUTABLE (-2) /* immutable rel_type, non-user authority refused */

   /* Retract value(s) for (source, relation) on semantic edges, applying the
    * rel_type's correction_behavior:
    *   supersede    -> stamp superseded_at (archived, still auditable);
    *   hard_delete  -> set suppressed = 1 + stamp superseded_at (tombstone);
    *   immutable    -> refuse (return FACT_RETRACT_IMMUTABLE) UNLESS authority is
    *                   user, in which case it supersedes (the user always wins).
    * Unknown / novel rel_types default to supersede.
    *
    * `target` (the old value) scopes the retraction to the specific
    * {source, relation, target} edge; NULL/empty retracts all current values of
    * (source, relation).
    *
    * Authority guard (§4/§5): a non-user (model/inferred) authority must NOT
    * retract a user-stated Class-A edge — such rows are skipped. A user authority
    * is unrestricted (and also overrides `immutable`, above).
    *
    * Returns the count of edges affected (>=0), FACT_RETRACT_IMMUTABLE on a
    * refused immutable edit, or -1 on bad args / DB error. */
   int db2_fact_retract(const char *source, const char *relation, const char *target,
                        fact_authority_t authority);

   /* Count currently-believed semantic edges touching `entity`: active
    * (superseded_at = '' AND suppressed = 0). The current-state recall filter
    * (§4 bitemporal transaction-time). Returns the count (>=0), or -1 on error. */
   int db2_fact_current_count(const char *entity);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_FACT_LIFECYCLE_H */
