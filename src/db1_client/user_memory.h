/* user_memory.h: per-user structured memory (db1).
 *
 * Proposal 2 (memory-db1-kb_store-architecture), Phase 1. aimee-server is 1:1 per
 * user, so db1 is per-user by construction and this store needs no tenancy
 * column. It mirrors the kb_store `memories` recall shape + selector patterns so
 * memory_recall can merge db1 (user-specific) and kb_store (org-shared) rows
 * uniformly. Scope for S1: identity + preferences only. */
#ifndef AIMEE_DB1_USER_MEMORY_H
#define AIMEE_DB1_USER_MEMORY_H

#include <stddef.h>
#include <stdint.h>

typedef struct cJSON cJSON;

/* Row shape mirrors kb_store_memory_cand_row_t's recall-relevant fields (same
 * capacities) so the recall renderer treats db1 and kb_store rows identically. */
typedef struct
{
   int64_t id;
   char tier[4];
   char kind[16];
   char key[512];
   char content[2048];
} db1_user_memory_row_t;

typedef enum
{
   DB1_USER_RECALL_IDENTITY = 1,
   DB1_USER_RECALL_PREFERENCES
} db1_user_recall_section_t;

#endif /* AIMEE_DB1_USER_MEMORY_H */
