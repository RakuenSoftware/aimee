/* db1/tool_local_availability.h: local machine tool usability cache.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_TOOL_LOCAL_AVAILABILITY_H
#define DEC_DB1_TOOL_LOCAL_AVAILABILITY_H 1

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_TOOL_UUID_LEN 64
#define DB1_TOOL_PATH_LEN 1024
#define DB1_TOOL_TS_LEN   32

   typedef struct
   {
      char tool_uuid[DB1_TOOL_UUID_LEN];
      int usable;
      char binary_path[DB1_TOOL_PATH_LEN];
      char checked_at[DB1_TOOL_TS_LEN];
   } db1_tool_local_availability_t;

   int db1_tool_local_availability_set(const char *tool_uuid, int usable, const char *binary_path);
   int db1_tool_local_availability_get(const char *tool_uuid, db1_tool_local_availability_t *out);
   int db1_tool_local_availability_delete(const char *tool_uuid);
   int db1_tool_local_availability_list(db1_tool_local_availability_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_TOOL_LOCAL_AVAILABILITY_H */
