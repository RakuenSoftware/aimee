#ifndef AIMEE_DB2_SUPPORT_RUNTIME_CONFIG_H
#define AIMEE_DB2_SUPPORT_RUNTIME_CONFIG_H

#define DB2_RUNTIME_CONFIG_ABI_VERSION 1u
#define DB2_RUNTIME_EMBEDDER_COMMAND_MAX 512

/* Immutable startup snapshot for the configuration values DB2 actually reads.
 * The module runtime installs it before worker threads or request dispatch. */
typedef struct
{
   unsigned int abi_version;
   int audit_worm_enabled;
   int cache_disabled;
   int code_cochange_git_enabled;
   int css_style_graph_enabled;
   int kb_curator_cross_repo_caller_collision_c;
   int kb_curator_cross_repo_distinctiveness_v;
   int kb_curator_cross_repo_graph_enabled;
   int kb_curator_cross_repo_k;
   int kb_curator_cross_repo_len_min;
   int kb_curator_cross_repo_m;
   int kb_curator_cross_repo_max_candidates;
   int kb_curator_cross_repo_p_pct;
   int kb_curator_cross_repo_review_queue_max;
   int kb_pdf_vector_enabled;
   int kb_purge_fence_ttl_s;
   int present;
   char embedder_command[DB2_RUNTIME_EMBEDDER_COMMAND_MAX];
} db2_runtime_config_t;

int db2_runtime_config_install(const db2_runtime_config_t *snapshot);

int config_audit_worm_enabled(void);
int config_cache_disabled(void);
int config_code_cochange_git_enabled(void);
int config_css_style_graph_enabled(void);
const char *config_embedder_command_current(const char *requested);
int config_kb_curator_cross_repo_caller_collision_c(void);
int config_kb_curator_cross_repo_distinctiveness_v(void);
int config_kb_curator_cross_repo_graph_enabled(void);
int config_kb_curator_cross_repo_k(void);
int config_kb_curator_cross_repo_len_min(void);
int config_kb_curator_cross_repo_m(void);
int config_kb_curator_cross_repo_max_candidates(void);
int config_kb_curator_cross_repo_p_pct(void);
int config_kb_curator_cross_repo_review_queue_max(void);
int config_kb_pdf_vector_enabled(void);
int config_kb_purge_fence_ttl_s(void);
int config_present(void);

#endif
