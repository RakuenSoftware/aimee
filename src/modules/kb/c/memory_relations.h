/* kb_store/memory_relations.h: cross-memory relationship SQL primitives —
 * memory_links + memory_provenance reads. KB_STORE-owned.
 *
 * The memory_link_t / provenance_entry_t typedefs live in
 * headers/memory.h. Callers must include "aimee.h" before this header
 * so the types resolve (same convention as kind_lifecycle.h). */
#ifndef DEC_KB_STORE_MEMORY_RELATIONS_H
#define DEC_KB_STORE_MEMORY_RELATIONS_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Same table, full schema (INSERT OR REPLACE): includes episode_id
    * (pass 0 for NULL), valid_at, invalid_at, weight. */
   void kb_store_memory_relation_upsert_full(int64_t memory_id, int64_t episode_id,
                                             const char *src_entity, const char *relation,
                                             const char *dst_entity, const char *fact_text,
                                             const char *valid_at, const char *invalid_at,
                                             double weight);

   /* Search memory_relations by infix LIKE across src_entity / relation /
    * dst_entity / fact_text. Ordered by weight DESC, has-valid_at, created_at
    * DESC. memory_relation_t lives in headers/memory.h. */
   int kb_store_memory_relations_search(const char *query, int limit, memory_relation_t *out,
                                        int max);

   /* List memory_relations rows where src_entity or dst_entity equals
    * `entity` (case-insensitive), ordered by weight DESC, created_at DESC. */
   int kb_store_memory_relations_for_entity(const char *entity, int limit, memory_relation_t *out,
                                            int max);

   /* List up to `limit` memory_relations rows where src_entity OR dst_entity
    * matches `entity_token` (case-insensitive infix LIKE) and fact_text is
    * non-empty. Ordered by weight DESC. Used by context assembly for typed
    * S-R-O expansion. Returns the count written. */
   int kb_store_memory_relations_supporting(const char *entity_token, int limit,
                                            memory_relation_t *out, int max);

   /* Append valid_at strings (NUL-terminated, truncated to fit) for OCCURRED_AT /
    * occurred_at / valid_from rows of `memory_id`. Each row is written as a
    * fixed 32-byte buffer. Returns the count written, ordered by valid_at ASC. */
   typedef struct
   {
      char date[32];
   } kb_store_memory_relation_date_row_t;

   /* As-of variant: same as_kb_store_memory_relations_search but adds the
    * temporal filter `valid_at <= as_of AND (invalid_at = '' OR
    * invalid_at > as_of)`. Pass NULL/"" for `as_of` to disable the filter
    * (caller handles fallback to kb_store_memory_relations_search). */
   int kb_store_memory_relations_search_as_of(const char *query, const char *as_of, int limit,
                                              memory_relation_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_STORE_MEMORY_RELATIONS_H */
