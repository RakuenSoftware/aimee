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
} context_candidate_t;

/* From memory_logic.c — used by memory_health.c's maintenance cycle. */
int memory_demote_from_failures(void);
void embed_unembedded_l2(void);

/* From memory_context.c — used by memory_assemble.c. */
int is_coverage_stopword(const char *word);
int has_temporal_markers(const char *text);

#endif /* DEC_MEMORY_CONTEXT_INTERNAL_H */
