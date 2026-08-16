/* db2/trace_mining.h: trace-mining cursor — DB2 subsystem.
 *
 * Records the highest execution-trace id consumed by the pattern miner
 * so subsequent runs only process new rows. */
#ifndef DEC_DB2_TRACE_MINING_H
#define DEC_DB2_TRACE_MINING_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Returns the highest last_trace_id recorded by previous mining runs,
    * or 0 if no runs have happened yet (or the db is unavailable). */
   int64_t db2_trace_mining_last_id(void);

   /* Append a new trace_mining_log row recording that mining advanced to
    * last_trace_id. Returns 0 on success. */
   int db2_trace_mining_record(int64_t last_trace_id);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_TRACE_MINING_H */
