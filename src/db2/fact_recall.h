/* fact_recall.h: typed-fact recall into the pre-injection envelope, with §7 PII
 * gating. P5. The read-side counterpart of fact_ingest: it surfaces an entity's
 * current (active, non-superseded, non-suppressed) typed facts into the
 * <aimee-context> block, withholding sensitive attributes unless the turn asks
 * for them (memory_pii_*). KB-side — needs a live db2_conn. */
#ifndef DEC_DB2_FACT_RECALL_H
#define DEC_DB2_FACT_RECALL_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Recall `entity`'s current semantic facts and format the ones that pass §7
    * PII gating into `out` (one "- <relation>: <target>\n" line each, highest
    * confidence first, up to an internal cap). `turn_requests_sensitive` is the
    * result of memory_pii_turn_requests_sensitive(query): when 0, PII/SECRET
    * attributes are withheld; identity/normal facts always pass above the
    * confidence floor. Writes "" when nothing qualifies. Returns the number of
    * facts written (>=0), or -1 on bad args. */
   int db2_fact_recall_block(const char *entity, int turn_requests_sensitive, char *out,
                             size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_FACT_RECALL_H */
