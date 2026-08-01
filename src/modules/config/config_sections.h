/* config_sections.h: per-aimee.yaml-section parsers extracted from config_load.
 * Each fills the relevant config_t fields from the parsed YAML `root`. Split out
 * of config.c to keep that file under the line cap. */
#ifndef DEC_CONFIG_SECTIONS_H
#define DEC_CONFIG_SECTIONS_H 1

#include "aimee.h"
#include "cJSON.h"

/* Shared helpers (defined in config.c) used by the section parsers. */
const char *config_default_db1_path(void);
config_mcp_transport_t config_mcp_transport_from_string(const char *s);

void config_parse_memory_rewrite_section(config_t *cfg, cJSON *root);
void config_parse_memory_negation_section(config_t *cfg, cJSON *root);
void config_parse_memory_query_expansion_section(config_t *cfg, cJSON *root);
void config_parse_memory_recall_lanes_section(config_t *cfg, cJSON *root);
void config_parse_memory_window_section(config_t *cfg, cJSON *root);
void config_parse_kb_section(config_t *cfg, cJSON *root);
void config_parse_memory_maintenance_section(config_t *cfg, cJSON *root);
void config_parse_worktree_gc_section(config_t *cfg, cJSON *root);
void config_parse_fold_section(config_t *cfg, cJSON *root);
void config_parse_modules_section(config_t *cfg, cJSON *root);
int config_parse_economizer_section(config_t *cfg, cJSON *root);
void config_parse_autonomy_section(config_t *cfg, cJSON *root);
void config_parse_memory_section(config_t *cfg, cJSON *root);
void config_parse_cross_verify_section(config_t *cfg, cJSON *root);
void config_parse_retry_section(config_t *cfg, cJSON *root);
void config_parse_concurrency_section(config_t *cfg, cJSON *root);
void config_parse_search_section(config_t *cfg, cJSON *root);
void config_parse_dogfood_section(config_t *cfg, cJSON *root);
void config_parse_identity_section(config_t *cfg, cJSON *root);
void config_parse_compact_section(config_t *cfg, cJSON *root);
void config_parse_sessions_section(config_t *cfg, cJSON *root);
void config_parse_sandbox_section(config_t *cfg, cJSON *root);
void config_parse_lsp_servers_section(config_t *cfg, cJSON *root);
void config_parse_mcp_clients_section(config_t *cfg, cJSON *root);
void config_parse_mcp_section(config_t *cfg, cJSON *root);
void config_parse_rewind_section(config_t *cfg, cJSON *root);
void config_parse_otel_section(config_t *cfg, cJSON *root);
void config_parse_integrity_section(config_t *cfg, cJSON *root);
void config_parse_session_section(config_t *cfg, cJSON *root);
void config_parse_transport_section(config_t *cfg, cJSON *root);
void config_parse_guardrails_section(config_t *cfg, cJSON *root);
void config_parse_auxiliary_section(config_t *cfg, cJSON *root);
void config_parse_model_meta_section(config_t *cfg, cJSON *root);
void config_parse_db2_section(config_t *cfg, cJSON *root);
void config_parse_ensemble_section(config_t *cfg, cJSON *root);
void config_parse_roundtable_section(config_t *cfg, cJSON *root);

void config_parse_kb_section2(config_t *cfg, cJSON *root);

/* context.engine: name — selects the active context-compaction engine
 * (context_engine_set_active in server_main). Re-homed from the deleted
 * config_plugin.c, which was mis-filed under the plugin subsystem. */
void config_parse_context_engine(config_t *cfg, cJSON *root);

#endif /* DEC_CONFIG_SECTIONS_H */
