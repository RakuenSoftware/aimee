/* db1/execution_trace.h: per-machine execution trace rows for turns and tools.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_EXECUTION_TRACE_H
#define DEC_DB1_EXECUTION_TRACE_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_ET_DIRECTION_LEN    16
#define DB1_ET_TOOL_NAME_LEN    64
#define DB1_ET_CREATED_AT_LEN   32
#define DB1_ET_CONTEXT_HASH_LEN 128
#define DB1_ET_CONTENT_LEN      4096
#define DB1_ET_TOOL_ARGS_LEN    4096
#define DB1_ET_TOOL_RESULT_LEN  4096

   typedef struct
   {
      int plan_id;
      /* Which delegate produced this row. Empty for the primary session. Without
       * it concurrent delegates interleave into one stream and cannot be told
       * apart -- see db_schema.c migration. */
      const char *session_id;
      int turn;
      const char *direction;
      const char *content;
      const char *tool_name;
      const char *tool_args;
      const char *tool_result;
      const char *context_hash;
   } db1_execution_trace_insert_row_t;

   typedef struct
   {
      int id;
      int turn;
      char direction[DB1_ET_DIRECTION_LEN];
      char tool_name[DB1_ET_TOOL_NAME_LEN];
      char created_at[DB1_ET_CREATED_AT_LEN];
   } db1_execution_trace_recent_row_t;

   typedef struct
   {
      int turn;
      char direction[DB1_ET_DIRECTION_LEN];
      char tool_name[DB1_ET_TOOL_NAME_LEN];
      char tool_args[DB1_ET_TOOL_ARGS_LEN];
      char tool_result[DB1_ET_TOOL_RESULT_LEN];
   } db1_execution_trace_tool_call_t;

   typedef struct
   {
      int id;
      int plan_id;
      int turn;
      char direction[DB1_ET_DIRECTION_LEN];
      char content[DB1_ET_CONTENT_LEN];
      char tool_name[DB1_ET_TOOL_NAME_LEN];
      char tool_args[DB1_ET_TOOL_ARGS_LEN];
      char tool_result[DB1_ET_TOOL_RESULT_LEN];
      char context_hash[DB1_ET_CONTEXT_HASH_LEN];
      char created_at[DB1_ET_CREATED_AT_LEN];
   } db1_execution_trace_detail_t;

   typedef struct
   {
      int64_t id;
      int plan_id;
      int turn;
      char direction[DB1_ET_DIRECTION_LEN];
      char tool_name[DB1_ET_TOOL_NAME_LEN];
      char tool_args[DB1_ET_TOOL_ARGS_LEN];
      char tool_result[DB1_ET_TOOL_RESULT_LEN];
   } db1_execution_trace_mining_row_t;

   int db1_execution_trace_insert(const db1_execution_trace_insert_row_t *row);
   int db1_execution_trace_count_for_session(const char *session_id);
   int db1_execution_trace_list_recent(db1_execution_trace_recent_row_t *out, int max);
   int db1_execution_trace_get(int trace_id, db1_execution_trace_detail_t *out);
   int db1_execution_trace_list_tool_calls(db1_execution_trace_tool_call_t *out, int max);
   int db1_execution_trace_list_after_id(int64_t after_id, db1_execution_trace_mining_row_t *out,
                                         int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_EXECUTION_TRACE_H */
