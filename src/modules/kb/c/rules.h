/* kb_store/rules.h: agent rules (preferences/principles), KB_STORE subsystem.
 *
 * Pure domain API. No backend types. SQL is encapsulated in kb_store/rules.c. */
#ifndef DEC_KB_STORE_RULES_H
#define DEC_KB_STORE_RULES_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int id;
      char polarity[16];
      char title[256];
      char description[1024];
      int weight;
      char domain[128];
      char created_at[32];
      char updated_at[32];
      char directive_type[16]; /* "hard", "soft", "session" */
      char expires_at[32];
   } rule_t;

   /* INSERT a fresh rule row with created_at/updated_at = now. Empty
    * polarity defaults to "positive". Returns 0 on success, -1 on SQL
    * / connection error. Used by `aimee data import --category rules`. */
   int kb_store_rules_insert(const char *polarity, const char *title, const char *description,
                             int weight);

   /* Find rule by title (case-insensitive). Returns 0 on success, -1 if not found. */
   int kb_store_rules_find_by_title(const char *title, rule_t *out);

   /* Delete a rule by ID. Returns 0 on success. */
   int kb_store_rules_delete(int id);

   /* Delete every rule with a given directive_type ("hard", "soft",
    * "session"). Returns the number of rows removed (>= 0), or -1 on
    * DB error. */
   int kb_store_rules_delete_by_directive_type(const char *directive_type);

   /* Update a rule's directive_type ("hard", "soft", "session"). Returns 0 on success. */
   int kb_store_rules_update_directive_type(int id, const char *directive_type);

   /* Invalidate the in-process kb_store_rules_generate() cache. */
   void kb_store_rules_cache_invalidate(void);

   /* Decay weights of stale rules. Returns number of rules affected.
    * Soft rules decay every 14 days, hard directives every 42 days.
    * Rules with weight < 10 for 30+ days are archived (deleted). */
   int kb_store_rules_decay(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_STORE_RULES_H */
