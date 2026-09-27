/* db2/agent_hints.h: one-shot hints surfaced to delegated agents.
 *
 * Hints are user-provided pattern->advice rows that match against an
 * incoming prompt for a given role. Each hint is consumable once.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB2_AGENT_HINTS_H
#define DEC_DB2_AGENT_HINTS_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Atomically find one unconsumed hint where role matches and the
    * prompt LIKE-matches the stored pattern, mark it consumed, and
    * return a heap-allocated copy of the hint text. Caller frees.
    * Returns NULL when no hint matches or DB2 is unavailable. */
   char *db2_agent_hint_find_and_consume(const char *role, const char *prompt);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_AGENT_HINTS_H */
