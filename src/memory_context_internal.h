/* memory_context_internal.h: types and helpers shared across the memory_*
 * TUs that used to be one file (memory_logic.c + memory_health.c +
 * memory_conflict.c + memory_context.c + memory_retrieval.c +
 * memory_assemble.c). Not a public API. */
#ifndef DEC_MEMORY_CONTEXT_INTERNAL_H
#define DEC_MEMORY_CONTEXT_INTERNAL_H 1

#include "aimee.h"
#include "memory.h"
#include <stdint.h>

#define MAX_CANDIDATES 100

/* Shared candidate shape used by the retrieval-confidence scorer
 * (memory_retrieval.c) and the context assembler (memory_assemble.c). */
typedef struct
{
   int64_t id;
   char tier[4];
   char key[512];
   char content[2048];
   char kind[16];
   char scope[32];
   double confidence;
   int use_count;
   int scope_rank;         /* project > workspace > global > hidden */
   int tier_priority;      /* L4 > L2 > L3 > L1 > L5 > L0 */
   double score;           /* computed relevance score */
   int estimated_tokens;   /* estimated token cost (strlen/4 + 1) */
   double score_per_token; /* score / max(1, estimated_tokens) */
   /* Origin identity for envelope diversity (the session this row came from).
    * Empty means "no known origin", and such rows are each treated as their own
    * origin rather than pooled into one bucket. */
   char source_session[128];
   int activation_sticky_turns;
   int activation_cooldown_turns;
   int activation_delay_turns;
   int activation_suppressed;
} context_candidate_t;

/* Total per-origin cap after the breadth pass. Pass one admits one item from
 * each origin; pass two admits a second high-scoring item where budget remains.
 * That reserve split preserves local depth without allowing one source to fill
 * the envelope. */
#define CONTEXT_ORIGIN_RESERVE_CAP 2

/* From memory_logic.c — used by memory_health.c's maintenance cycle. */
int memory_demote_from_failures(void);
void embed_unembedded_l2(void);

/* From memory_context.c — used by memory_assemble.c. */
int is_coverage_stopword(const char *word);
int has_temporal_markers(const char *text);

/* From memory_context.c — whole-word keyword matching for the module's intent and
 * tagging keyword lists. Returns 1 iff |needle| occurs in |haystack| on both a left
 * and a right word boundary, case-insensitively, where the right boundary also
 * accepts a common inflectional suffix ("deploy" matches "deployed"). Plain strstr
 * on these lists matched "add" inside "address", "count" inside "account", and
 * "auth" inside "author". Multi-word needles are matched on their outer edges. */
int memory_keyword_present(const char *haystack, const char *needle);

#endif /* DEC_MEMORY_CONTEXT_INTERNAL_H */
