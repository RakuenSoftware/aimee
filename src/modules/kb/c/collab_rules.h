/* kb_store/collab_rules.h: collaborative rules owned by KB_STORE. */
#ifndef DEC_KB_STORE_COLLAB_RULES_H
#define DEC_KB_STORE_COLLAB_RULES_H 1

#include "../headers/collab_rules.h"

#ifdef __cplusplus
extern "C"
{
#endif

   int kb_store_collab_rules_epoch(void);
   int kb_store_collab_rules_list(collab_rule_t *out, int max);
   int kb_store_collab_rules_list_active(collab_rule_t *out, int max);
   int kb_store_collab_rules_propose(const char *text, const char *reason, const char *proposed_by);
   int kb_store_collab_rules_approve(int rule_id);
   int kb_store_collab_rules_reject(int rule_id);
   int kb_store_collab_rules_retire(int rule_id);
   char *kb_store_collab_rules_inject(int agent_last_epoch);
   char *kb_store_collab_rules_json_all(void);
   char *kb_store_collab_rules_json_active(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_STORE_COLLAB_RULES_H */
