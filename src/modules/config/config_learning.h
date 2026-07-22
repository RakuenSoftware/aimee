#ifndef DEC_CONFIG_LEARNING_H
#define DEC_CONFIG_LEARNING_H 1

#include "config.h"
#include "cJSON.h"

void config_learning_defaults(config_t *cfg);
void config_apply_learning_settings(config_t *cfg, cJSON *root);
void config_apply_calibration_settings(config_t *cfg, cJSON *root);
void config_apply_demotion_settings(config_t *cfg, cJSON *root);
void config_apply_ranking_settings(config_t *cfg, cJSON *root);
void config_apply_reasoning_settings(config_t *cfg, cJSON *root);
void config_apply_bandit_settings(config_t *cfg, cJSON *root);
void config_apply_planner_settings(config_t *cfg, cJSON *root);
void config_apply_mdl_settings(config_t *cfg, cJSON *root);
void config_apply_review_settings(config_t *cfg, cJSON *root);

#endif
