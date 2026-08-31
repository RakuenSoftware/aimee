/* db2/rules.h: agent rules (preferences/principles), DB2 subsystem.
 *
 * Pure domain API. No backend types. SQL is encapsulated in db2/rules.c. */
#ifndef DEC_DB2_RULES_H
#define DEC_DB2_RULES_H 1

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
   int db2_rules_insert(const char *polarity, const char *title, const char *description,
                        int weight);

   /* Export every rule row as JSONL to |path| (one object per line)
    * with the columns: polarity, title, description, weight, domain,
    * created_at, updated_at. Returns rows written, or -1 on alloc / DB
    * / IO failure. Used by `aimee data export --category rules`. */
   int db2_rules_export_jsonl(const char *path);

   /* List all rules ordered by weight desc. Returns count. */
   int db2_rules_list(rule_t *out, int max_rules);

   /* List rules with minimum weight. Returns count. */
   int db2_rules_list_by_tier(int min_weight, rule_t *out, int max_rules);

   /* List rules with directive_type='hard', ordered by weight desc. Returns count. */
   int db2_rules_list_hard(rule_t *out, int max_rules);

   /* Get a single rule by ID. Returns 0 on success, -1 if not found. */
   int db2_rules_get(int id, rule_t *out);

   /* Find rule by title (case-insensitive). Returns 0 on success, -1 if not found. */
   int db2_rules_find_by_title(const char *title, rule_t *out);

   /* Delete a rule by ID. Returns 0 on success. */
   int db2_rules_delete(int id);

   /* Delete every rule with a given directive_type ("hard", "soft",
    * "session"). Returns the number of rows removed (>= 0), or -1 on
    * DB error. */
   int db2_rules_delete_by_directive_type(const char *directive_type);

   /* Update a rule's weight. Returns 0 on success. */
   int db2_rules_update_weight(int id, int weight);

   /* Update a rule's directive_type ("hard", "soft", "session"). Returns 0 on success. */
   int db2_rules_update_directive_type(int id, const char *directive_type);

   /* Reinforce a rule by setting directive_type, optionally weight, and bumping
    * updated_at + last_reinforced_at to now. weight_override < 0 leaves weight
    * unchanged. Returns 0 on success. */
   int db2_rules_reinforce_directive(int id, const char *directive_type, int weight_override);

   /* Generate rules.md content. Returns malloc'd string (caller frees). */
   char *db2_rules_generate(void);

   /* Invalidate the in-process db2_rules_generate() cache. */
   void db2_rules_cache_invalidate(void);

   /* Write a stable, compact signature of the rules table
    * (count + max(updated_at)) into buf. Used by callers that build a
    * larger session-scoped cache key. */
   void db2_rules_signature(char *buf, size_t len);

   /* Tier label for a weight. */
   const char *db2_rules_tier(int weight);

   /* Polarity symbol (+, -, ~). */
   char db2_rules_polarity_symbol(const char *polarity);

   /* Decay weights of stale rules. Returns number of rules affected.
    * Soft rules decay every 14 days, hard directives every 42 days.
    * Rules with weight < 10 for 30+ days are archived (deleted). */
   int db2_rules_decay(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_RULES_H */
