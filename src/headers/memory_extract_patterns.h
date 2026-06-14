/* memory_extract_patterns.h: pattern-first fact extraction (typed-fact §6) +
 * the cheap retraction-signal scan (§4). P5.
 *
 * A pure, high-precision pass that runs BEFORE the model on ingress text:
 * structured-value classifiers (IPv4/IPv6/MAC/email/ISO-date) and a small set of
 * unambiguous sentence templates emit candidate triples directly, so the residual
 * text — only what the patterns don't cover — needs the (costly) model rewrite.
 * Pattern hits are still validated by the §1 gate (db2_fact_commit); regex
 * precision buys cost, not a bypass of validation. The same pass cheaply flags
 * retraction cues ("forget that", "that's wrong") for the §4 correction path.
 *
 * No DB / config dependency — this is deterministic logic, unit-tested directly. */
#ifndef DEC_MEMORY_EXTRACT_PATTERNS_H
#define DEC_MEMORY_EXTRACT_PATTERNS_H 1

#include <stddef.h>
#include "memory_ontology.h" /* memory_node_kind_t */
#include "rel_types.h"       /* REL_TYPE_NAME_MAX */

#ifdef __cplusplus
extern "C"
{
#endif

   /* Structured value shapes the classifier recognizes (high precision). */
   typedef enum
   {
      PAT_VAL_NONE = 0,
      PAT_VAL_IPV4,
      PAT_VAL_IPV6,
      PAT_VAL_MAC,
      PAT_VAL_EMAIL,
      PAT_VAL_DATE, /* ISO 8601 calendar date YYYY-MM-DD */
   } pattern_value_kind_t;

   /* Classify a single trimmed token as a structured value, or PAT_VAL_NONE.
    * Whole-token match only (no substring), so precision stays high. */
   pattern_value_kind_t memory_pattern_classify_value(const char *token);

   /* The entity kind a value shape implies: IPv4/IPv6 -> NODE_IP; MAC/email/date
    * -> NODE_SCALAR; NONE -> NODE_OTHER. */
   memory_node_kind_t memory_pattern_value_node_kind(pattern_value_kind_t k);

   /* Retraction-signal scan (§4/§6): 1 if the text carries a retraction cue
    * ("forget", "delete that", "that's wrong", "no longer", "scratch that",
    * "ignore that"), else 0. Case-insensitive. NULL/empty -> 0. */
   int memory_pattern_is_retraction(const char *text);

   /* The attribute a retraction turn refers to: the word(s) after "my " up to a
    * sentence terminator, " is "/" was ", or ~3 words. E.g. "forget my email" ->
    * "email", "forget my favorite color please" -> "favorite color". Writes the
    * raw attribute into out (the caller normalizes it via the rel_type path).
    * Returns 1 when a "my <attr>" possessive is present, else 0. Pairs with
    * memory_pattern_is_retraction to drive db2_fact_retract; because retraction
    * only affects facts that actually exist, an imprecise attr safely no-ops. */
   int memory_pattern_possessive_attr(const char *text, char *out, size_t out_len);

   /* A candidate triple extracted before the model. rel_type is a normalized
    * guess; the §1 gate still decides whether it is written and how. */
   typedef struct
   {
      char subject[128];
      char rel_type[REL_TYPE_NAME_MAX];
      char object[128];
      memory_node_kind_t subject_kind;
      memory_node_kind_t object_kind;
   } pattern_triple_t;

   /* Extract high-precision candidate triples from `text` into `out` (up to max).
    * Currently recognizes the canonical personal-fact template
    *   "my <attr> is <value>"   -> (user, <attr>, <value>)
    * with subject_kind=NODE_PERSON and object_kind inferred from the value shape.
    * Conservative by design (precision over recall): unmatched text yields no
    * triple and is left for the model. Returns the count written (>=0), or -1 on
    * bad args. */
   int memory_extract_patterns(const char *text, pattern_triple_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_MEMORY_EXTRACT_PATTERNS_H */
