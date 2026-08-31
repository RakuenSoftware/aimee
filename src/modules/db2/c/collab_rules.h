/* db2/collab_rules.h: collaborative rules owned by DB2. */
#ifndef DEC_DB2_COLLAB_RULES_H
#define DEC_DB2_COLLAB_RULES_H 1

#include "../headers/collab_rules.h"

#ifdef __cplusplus
extern "C"
{
#endif

   int db2_collab_rules_epoch(void);
   int db2_collab_rules_list(collab_rule_t *out, int max);
   int db2_collab_rules_list_active(collab_rule_t *out, int max);
   int db2_collab_rules_propose(const char *text, const char *reason, const char *proposed_by);
   int db2_collab_rules_approve(int rule_id);
   int db2_collab_rules_reject(int rule_id);
   int db2_collab_rules_retire(int rule_id);
   char *db2_collab_rules_inject(int agent_last_epoch);
   char *db2_collab_rules_json_all(void);
   char *db2_collab_rules_json_active(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_COLLAB_RULES_H */
