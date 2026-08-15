/* db2/kb_service_backend_export.h: filtered memory export/import for kb.export / kb.import RPCs. */
#ifndef DEC_DB2_KB_SERVICE_BACKEND_EXPORT_H
#define DEC_DB2_KB_SERVICE_BACKEND_EXPORT_H 1

#include "cJSON.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Query memories with optional filters and return a JSON export envelope.
    * workspace:        filter by memory_workspaces.workspace (NULL = no filter)
    * kind:             filter by memories.kind (NULL = no filter)
    * since_iso:        filter created_at >= this ISO date string (NULL = no filter)
    * include_archived: if 0, restrict to lifecycle_state = 'active'
    * Returns cJSON object with schema_version, exported_at, memories[], entity_profiles[], count.
    * Returns NULL on alloc failure. Caller frees with cJSON_Delete. */
   cJSON *db2_kb_service_memory_export_filtered_json(const char *workspace, const char *kind,
                                                     const char *since_iso, int include_archived);

   /* Re-insert memories from a JSON array into the memories table.
    * memories_arr:       cJSON array of memory objects (each must have kind, key, content).
    * workspace_override: if non-NULL/non-empty, record this workspace in memory_workspaces.
    * dry_run:            if 1, count without writing.
    * imported_out:       set to count of successfully inserted rows on success.
    * Returns 0 on success, -1 on error. */
   int db2_kb_service_memory_import_json(cJSON *memories_arr, const char *workspace_override,
                                         int dry_run, int *imported_out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_KB_SERVICE_BACKEND_EXPORT_H */
