/* Connection contract for Go-owned pattern extraction and turn scanning. */
#ifndef DEC_MEMORY_EXTRACT_PATTERNS_H
#define DEC_MEMORY_EXTRACT_PATTERNS_H 1

#include <stddef.h>
#include "memory_ontology.h" /* memory_node_kind_t */
#include "rel_types.h"       /* REL_TYPE_NAME_MAX */

#ifdef __cplusplus
extern "C"
{
#endif

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

   /* Triple provider: returns 0 and writes the count to *count, or non-zero if
    * it could not produce an answer at all. */
   typedef int (*memory_pattern_extractor_fn)(const char *text, pattern_triple_t *out, int max,
                                              int *count);

/* Bound on the attribute returned by the Go scan stage. */
#define MEMORY_PATTERN_ATTR_MAX 128

   /* What the cheap pre-model scan learns about one turn. */
   typedef struct
   {
      int is_retraction;                     /* a retraction cue is present */
      int has_attr;                          /* a "my <attr>" possessive is present */
      char attr[MEMORY_PATTERN_ATTR_MAX];    /* the attribute, when has_attr */
   } memory_pattern_turn_t;

   /* Scan a turn for a retraction cue and the attribute it names, in one pass.
    * Returns 0 and fills `out`, or -1 if no answer could be produced.
    *
    * The two questions are asked together because their only caller asks them
    * together, once per turn. */
   int memory_pattern_scan_turn(const char *text, memory_pattern_turn_t *out);

   /* Turn scanner: returns 0 and fills `out`, or non-zero if it could not
    * answer. */
   typedef int (*memory_pattern_turn_scanner_fn)(const char *text, memory_pattern_turn_t *out);

   /* Route the turn scan through `scanner` (the memory module over the bus).
    * Authoritative, and its failure is reported rather than guessed at. The
    * caller must not retract on a -1: this scan drives deletion, and the safe
    * side of a broken module is leaving a fact the user asked to forget (they
    * can ask again) rather than deleting one they did not. */
   void memory_extract_register_turn_scanner(memory_pattern_turn_scanner_fn scanner);

   /* Route extraction through `extractor` (the memory module over the bus).
    * A registered extractor is authoritative: when it fails, memory_extract_patterns
    * returns -1 rather than falling back or reporting zero triples. Zero means the
    * text held no facts; a broken module must not be able to say that. -1 is the
    * function's existing bad-arg code, and the caller already distinguishes it. */
   void memory_extract_register_extractor(memory_pattern_extractor_fn extractor);

#ifdef __cplusplus
}
#endif

#endif /* DEC_MEMORY_EXTRACT_PATTERNS_H */
