/* mcp_osv_cache.h: DB1 storage for MCP OSV package verdicts. */
#ifndef DEC_DB1_MCP_OSV_CACHE_H
#define DEC_DB1_MCP_OSV_CACHE_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_MCP_OSV_TEXT_LEN    256
#define DB1_MCP_OSV_VERDICT_LEN 16
#define DB1_MCP_OSV_ACTION_LEN  32
#define DB1_MCP_OSV_CHECKED_LEN 32

   typedef struct
   {
      char client_name[DB1_MCP_OSV_TEXT_LEN];
      char ecosystem[DB1_MCP_OSV_TEXT_LEN];
      char name[DB1_MCP_OSV_TEXT_LEN];
      char version[DB1_MCP_OSV_TEXT_LEN];
      char verdict[DB1_MCP_OSV_VERDICT_LEN];
      char advisory_ids[DB1_MCP_OSV_TEXT_LEN];
      int64_t checked_at;
      char checked_at_text[DB1_MCP_OSV_CHECKED_LEN];
   } db1_mcp_osv_cache_row_t;

   int db1_mcp_osv_cache_get(const char *ecosystem, const char *name, const char *version,
                             int ttl_hours, db1_mcp_osv_cache_row_t *out);
   int db1_mcp_osv_cache_upsert(const char *ecosystem, const char *name, const char *version,
                                const char *verdict, const char *advisory_ids);
   int db1_mcp_osv_cache_list(db1_mcp_osv_cache_row_t *out, int max);
   int db1_mcp_osv_audit(const char *client_name, const char *ecosystem, const char *name,
                         const char *version, const char *verdict, const char *action,
                         const char *advisory_ids);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_MCP_OSV_CACHE_H */
