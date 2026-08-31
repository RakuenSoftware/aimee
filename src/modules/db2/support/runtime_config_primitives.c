#include "db2_runtime_config.h"

#include <string.h>

static db2_runtime_config_t DB2_RUNTIME_CONFIG = {
    .abi_version = DB2_RUNTIME_CONFIG_ABI_VERSION,
    .audit_worm_enabled = 1,
};

int db2_runtime_config_install(const db2_runtime_config_t *snapshot)
{
   if (!snapshot || snapshot->abi_version != DB2_RUNTIME_CONFIG_ABI_VERSION ||
       !memchr(snapshot->embedder_command, '\0', sizeof(snapshot->embedder_command)))
      return -1;
   DB2_RUNTIME_CONFIG = *snapshot;
   return 0;
}

int config_audit_worm_enabled(void) { return DB2_RUNTIME_CONFIG.audit_worm_enabled; }
int config_cache_disabled(void) { return DB2_RUNTIME_CONFIG.cache_disabled; }
int config_code_cochange_git_enabled(void) { return DB2_RUNTIME_CONFIG.code_cochange_git_enabled; }
int config_css_style_graph_enabled(void) { return DB2_RUNTIME_CONFIG.css_style_graph_enabled; }

const char *config_embedder_command_current(const char *requested)
{
   return requested && requested[0] ? requested : DB2_RUNTIME_CONFIG.embedder_command;
}

int config_kb_curator_cross_repo_caller_collision_c(void)
{
   return DB2_RUNTIME_CONFIG.kb_curator_cross_repo_caller_collision_c;
}
int config_kb_curator_cross_repo_distinctiveness_v(void)
{
   return DB2_RUNTIME_CONFIG.kb_curator_cross_repo_distinctiveness_v;
}
int config_kb_curator_cross_repo_graph_enabled(void)
{
   return DB2_RUNTIME_CONFIG.kb_curator_cross_repo_graph_enabled;
}
int config_kb_curator_cross_repo_k(void) { return DB2_RUNTIME_CONFIG.kb_curator_cross_repo_k; }
int config_kb_curator_cross_repo_len_min(void)
{
   return DB2_RUNTIME_CONFIG.kb_curator_cross_repo_len_min;
}
int config_kb_curator_cross_repo_m(void) { return DB2_RUNTIME_CONFIG.kb_curator_cross_repo_m; }
int config_kb_curator_cross_repo_max_candidates(void)
{
   return DB2_RUNTIME_CONFIG.kb_curator_cross_repo_max_candidates;
}
int config_kb_curator_cross_repo_p_pct(void)
{
   return DB2_RUNTIME_CONFIG.kb_curator_cross_repo_p_pct;
}
int config_kb_curator_cross_repo_review_queue_max(void)
{
   return DB2_RUNTIME_CONFIG.kb_curator_cross_repo_review_queue_max;
}
int config_kb_pdf_vector_enabled(void) { return DB2_RUNTIME_CONFIG.kb_pdf_vector_enabled; }
int config_kb_purge_fence_ttl_s(void) { return DB2_RUNTIME_CONFIG.kb_purge_fence_ttl_s; }
int config_present(void) { return DB2_RUNTIME_CONFIG.present; }
