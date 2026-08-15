/* user_memory.h: per-user structured memory (db1).
 *
 * Proposal 2 (memory-db1-db2-architecture), Phase 1. aimee-server is 1:1 per
 * user, so db1 is per-user by construction and this store needs no tenancy
 * column. It mirrors the db2 `memories` recall shape + selector patterns so
 * memory_recall can merge db1 (user-specific) and db2 (org-shared) rows
 * uniformly. Scope for S1: identity + preferences only. */
#ifndef AIMEE_DB1_USER_MEMORY_H
#define AIMEE_DB1_USER_MEMORY_H

#include <stddef.h>
#include <stdint.h>

typedef struct cJSON cJSON;

/* Row shape mirrors db2_memory_cand_row_t's recall-relevant fields (same
 * capacities) so the recall renderer treats db1 and db2 rows identically. */
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

/* Fill up to `cap` rows for a recall section from db1 user_memories, using the
 * same tier band (L2..L5) + key conventions as the db2 selectors. Returns the
 * number filled (0 on empty table / no db1 / error). */
int db1_user_memory_list_recall(db1_user_recall_section_t section, db1_user_memory_row_t *rows,
                                int cap);

/* Fast "does this user have any db1 memory at all?" check. Lets hot paths skip
 * the recall parse/merge/reserialize entirely when the table is empty (the
 * production case until capture is wired). */
int db1_user_memory_any(void);

/* Merge this user's db1 rows for a recall section INTO an existing recall array
 * (as produced by aimee-kb for db2/org memory). db1 rows are surfaced first and
 * win on key collision (the org duplicate is removed) — the user>soft-org half
 * of the precedence lattice. Each injected row is tagged scope="user". A no-op
 * when db1 has no matching rows. */
void db1_user_memory_merge_into_array(cJSON *arr, db1_user_recall_section_t section,
                                      const char *why);

/* Upsert a user memory (insert or replace by (kind,key)). Returns 0 on success.
 * S1 uses this for seeding + tests; the ergonomic capture commands
 * (aimee identity/prefer) arrive in S2. */
int db1_user_memory_upsert(const char *kind, const char *tier, const char *key, const char *content,
                           double confidence, const char *source_session);

#endif /* AIMEE_DB1_USER_MEMORY_H */
