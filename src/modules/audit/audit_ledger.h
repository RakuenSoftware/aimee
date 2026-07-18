/* audit_ledger.h: read the governed-action audit ledger (S3 of the per-action
 * governance audit).
 *
 * The audit.log written by audit_action_log() (kind="tool_action") is the
 * canonical, tamper-evident governed-action ledger. This reader makes it
 * replayable/inspectable — the "reader-before-writer" half of the rollout, so
 * the rows are consumable before the writer is enabled by default (S7).
 */
#ifndef AIMEE_AUDIT_LEDGER_H
#define AIMEE_AUDIT_LEDGER_H 1

#include "cJSON.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Read all kind="tool_action" rows from audit.log and its rotated siblings
    * (audit.log.0 .. audit.log.N), returned as a cJSON array (caller owns via
    * cJSON_Delete), ordered deterministically by (ts, file recency, byte offset).
    *
    * `from_ts`/`to_ts` bound the ISO-8601 ts inclusively; NULL/empty means
    * unbounded on that end. Non-JSON lines and non-tool_action rows are skipped;
    * the count of unparseable lines is logged (rate-limited) so drift is visible.
    * Returns NULL only on allocation failure (a missing audit.log yields an empty
    * array, not NULL). */
   cJSON *audit_ledger_read(const char *from_ts, const char *to_ts);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_AUDIT_LEDGER_H */
