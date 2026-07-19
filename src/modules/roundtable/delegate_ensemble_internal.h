#ifndef DELEGATE_ENSEMBLE_INTERNAL_H
#define DELEGATE_ENSEMBLE_INTERNAL_H
#include "server.h"
#include "delegate_ensemble.h"
/* Cross-TU decls split from delegate_ensemble.c (was delegate_ensemble_review.inc). */
/* promoted cross-TU (former .inc statics) */
void capture_round_review_items(const agent_result_t *results, int ref_count,
                                roundtable_result_t *out, int round);
int parse_review_issue_keys(const char *text, char keys[][128], int *count, int max);
int key_seen128(char keys[][128], int count, const char *key);
cJSON *parse_model_json_lenient(const char *text);

#endif
