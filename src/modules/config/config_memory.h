#ifndef DEC_CONFIG_MEMORY_H
#define DEC_CONFIG_MEMORY_H 1

#include "aimee.h"

int config_parse_dispositions(config_t *cfg, const cJSON *root);
int config_parse_memory_citations(config_t *cfg, const cJSON *root);
int config_parse_memory_briefing(config_t *cfg, const cJSON *root);
int config_parse_memory_aggregation(config_t *cfg, const cJSON *root);
int config_parse_memory_prospective(config_t *cfg, const cJSON *root);
int config_parse_memory_lifecycle(config_t *cfg, const cJSON *root);
int config_parse_memory_recall(config_t *cfg, const cJSON *root);
int config_parse_memory_directives(config_t *cfg, const cJSON *root);
int config_parse_memory_cognify(config_t *cfg, const cJSON *root);

#endif /* DEC_CONFIG_MEMORY_H */
