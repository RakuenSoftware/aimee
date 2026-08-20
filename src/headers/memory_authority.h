#ifndef MEMORY_AUTHORITY_H
#define MEMORY_AUTHORITY_H
/* Who is asking for a destructive memory edit.
 *
 * Split into its own dependency-free header (memory.h is not standalone-
 * includable) so the kb_service backend contract can name the type without
 * dragging the whole memory surface into every TU that includes it.
 *
 * The store already defends an old value on the WRITE path: memory_store()
 * compares an incoming L2 write against the existing value under the same key
 * and routes to memory_supersede() — versioning the old row rather than merging
 * over it — when the two are materially different. The two EDIT verbs (update,
 * forget) walked around that defence and destroyed the prior value outright.
 *
 * This mirrors fact_authority_t and the rule db2_fact_retract() already enforces
 * for typed facts: a model correction must not silently destroy what the user
 * stated. MODEL is 0 so that any caller that forgets to say (or any wire request
 * that omits the field) gets the non-destructive path by default. */
typedef enum
{
   MEMORY_AUTHORITY_MODEL = 0, /* agent/LLM-initiated: version, never destroy */
   MEMORY_AUTHORITY_USER = 1,  /* user/operator-initiated: may destroy */
} memory_authority_t;

#endif /* MEMORY_AUTHORITY_H */
