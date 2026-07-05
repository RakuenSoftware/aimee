/* db1/harness_memory.h: DB1-backed store for intercepted agent memory.
 *
 * One canonical, client-neutral row per (project, name); the on-disk
 * memory file (memory/<name>.md) is an aimee-owned projection of it. See
 * docs/proposals/pending/central-agent-memory-interception.{md,plan.md}.
 */
#ifndef DEC_DB1_HARNESS_MEMORY_H
#define DEC_DB1_HARNESS_MEMORY_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define HMEM_PROJECT_LEN 256
#define HMEM_NAME_LEN    256
#define HMEM_TYPE_LEN    16
#define HMEM_CLIENT_LEN  64
#define HMEM_SESSION_LEN 64
#define HMEM_TS_LEN      32
#define HMEM_HASH_LEN    65

   typedef struct
   {
      int64_t id;
      char project[HMEM_PROJECT_LEN];
      char name[HMEM_NAME_LEN];
      char type[HMEM_TYPE_LEN];
      /* On INPUT to hmem_upsert these three may be caller-BORROWED (e.g.
       * cJSON-owned) and need only be valid for the duration of the call —
       * upsert binds them SQLITE_TRANSIENT and never retains the struct. On rows
       * returned by get/list/search they are heap-owned (free via
       * hmem_row_free_fields); description="" not NULL after a load. */
      char *description;
      char *body;
      char *meta_json;
      char content_hash[HMEM_HASH_LEN];
      char last_client[HMEM_CLIENT_LEN];
      char source_session[HMEM_SESSION_LEN];
      int schema_version;
      char deleted_at[HMEM_TS_LEN]; /* "" when live */
      char created_at[HMEM_TS_LEN];
      char updated_at[HMEM_TS_LEN];
   } hmem_row_t;

   /* Is `type` one of the allowed values? (Mirrors the SQL CHECK.) */
   int hmem_type_valid(const char *type);

   /* Upsert by (project,name). Reuses in->content_hash if set, else computes it
    * via hmem_content_hash. Clears deleted_at (resurrection). No-op (and still
    * returns 0) when a live row already has the identical content_hash. On
    * success *out_id (if non-NULL) gets the row id. Returns 0 / -1. */
   int hmem_upsert(const hmem_row_t *in, int64_t *out_id);

   /* Fetch a live row. Returns 0 (out owns heap fields — free with
    * hmem_row_free_fields) or -1 if absent/tombstoned. NOTE: on success `out`
    * is overwritten wholesale; the caller must hmem_row_free_fields() (or zero)
    * any previously-populated row before reusing it here, else its heap fields
    * leak. `out` need not be zeroed on a first call. */
   int hmem_get(const char *project, const char *name, hmem_row_t *out);

   /* List rows for a project (live only unless include_deleted). Allocates
    * *out (free with hmem_rows_free) and sets *n. Returns 0 / -1. */
   int hmem_list(const char *project, hmem_row_t **out, int *n, int include_deleted);

   /* Substring search over name/description/body of live rows. As hmem_list. */
   int hmem_search(const char *project, const char *query, hmem_row_t **out, int *n);

   /* Exclusive end index of the response page that starts at `offset` and whose
    * serialized rows fit within `budget` bytes. Always returns at least
    * offset+1 when rows remain (so a single oversized row still advances and the
    * pager can't stall). Pure — used by the list/search routes to page a result
    * set that would otherwise overflow the fixed RPC response buffer. */
   int hmem_page_end(const hmem_row_t *rows, int n, int offset, size_t budget);

   /* Tombstone one live row. Returns 0 (even if already absent) / -1 on error. */
   int hmem_tombstone(const char *project, const char *name);

   /* Bulk-tombstone every live row whose name == dir or starts with dir+"/"
    * (dir=="" tombstones the whole project) in ONE statement/transaction.
    * Returns the number tombstoned, or -1 on error. */
   int hmem_tombstone_prefix(const char *project, const char *dir);

   void hmem_row_free_fields(hmem_row_t *row);
   void hmem_rows_free(hmem_row_t *rows, int n);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_HARNESS_MEMORY_H */
