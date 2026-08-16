/* db2/memory_export.h: JSONL export helpers for the memories table.
 *
 * Owns the SQL + cJSON serialization for `aimee data export` so the
 * column lists and the writer loop never have to leak into cmd_data.c.
 */
#ifndef DEC_DB2_MEMORY_EXPORT_H
#define DEC_DB2_MEMORY_EXPORT_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Heap-allocated row used by db2_memory_export_alloc_all so the
    * `aimee data export memories` JSONL writer can preserve full
    * key/content lengths (memory_t.key/content are fixed-size and may
    * truncate). */
   typedef struct
   {
      int64_t id;
      char tier[16];
      char kind[32];
      char *key;
      char *content;
      double confidence;
      int use_count;
      char *source_session;
      char *created_at;
      char *updated_at;
   } db2_memory_export_row_t;

   /* Free the heap-allocated string fields on |row|. Safe on a zeroed
    * struct. */
   void db2_memory_export_row_free(db2_memory_export_row_t *row);

   /* Materialize every row of memories ORDER BY id into a freshly
    * allocated array. On success sets *out to a heap buffer the caller
    * must free (after freeing each row's string fields with
    * db2_memory_export_row_free) and *count to the row count, and
    * returns 0. Returns -1 on alloc / DB2 failure (out=NULL, count=0).
    *
    * The row materialization is so the caller can issue follow-up
    * scope queries per id without fighting libpq's one-active-result
    * rule. */
   int db2_memory_export_alloc_all(db2_memory_export_row_t **out, size_t *count);

   /* Export every memory row with kind = 'decision' as JSONL to |path|
    * (one object per line) with the columns: tier, kind, key, content,
    * confidence, source_session, created_at. Returns rows written, or
    * -1 on alloc / DB / IO failure. Used by
    * `aimee data export --category decisions`. */
   int db2_memory_decisions_export_jsonl(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_MEMORY_EXPORT_H */
