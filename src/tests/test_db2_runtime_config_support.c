#include "../modules/db2/support/db2_runtime_config.h"

#include <assert.h>
#include <string.h>

static void test_fail_closed_defaults(void)
{
   assert(config_audit_worm_enabled() == 0);
   assert(config_cache_disabled() == 0);
   assert(config_code_cochange_git_enabled() == 0);
   assert(config_css_style_graph_enabled() == 0);
   assert(config_embedder_command_current(NULL)[0] == '\0');
   assert(config_kb_curator_cross_repo_k() == 0);
   assert(config_kb_pdf_vector_enabled() == 0);
   assert(config_present() == 0);
}

static void test_full_snapshot(void)
{
   db2_runtime_config_t cfg = {
       .abi_version = DB2_RUNTIME_CONFIG_ABI_VERSION,
       .audit_worm_enabled = 1,
       .cache_disabled = 2,
       .code_cochange_git_enabled = 3,
       .css_style_graph_enabled = 4,
       .kb_curator_cross_repo_caller_collision_c = 5,
       .kb_curator_cross_repo_distinctiveness_v = 6,
       .kb_curator_cross_repo_graph_enabled = 7,
       .kb_curator_cross_repo_k = 8,
       .kb_curator_cross_repo_len_min = 9,
       .kb_curator_cross_repo_m = 10,
       .kb_curator_cross_repo_max_candidates = 11,
       .kb_curator_cross_repo_p_pct = 12,
       .kb_curator_cross_repo_review_queue_max = 13,
       .kb_pdf_vector_enabled = 14,
       .kb_purge_fence_ttl_s = 15,
       .present = 16,
       .embedder_command = "configured-embedder --json",
   };
   assert(db2_runtime_config_install(&cfg) == 0);
   assert(config_audit_worm_enabled() == 1);
   assert(config_cache_disabled() == 2);
   assert(config_code_cochange_git_enabled() == 3);
   assert(config_css_style_graph_enabled() == 4);
   assert(config_kb_curator_cross_repo_caller_collision_c() == 5);
   assert(config_kb_curator_cross_repo_distinctiveness_v() == 6);
   assert(config_kb_curator_cross_repo_graph_enabled() == 7);
   assert(config_kb_curator_cross_repo_k() == 8);
   assert(config_kb_curator_cross_repo_len_min() == 9);
   assert(config_kb_curator_cross_repo_m() == 10);
   assert(config_kb_curator_cross_repo_max_candidates() == 11);
   assert(config_kb_curator_cross_repo_p_pct() == 12);
   assert(config_kb_curator_cross_repo_review_queue_max() == 13);
   assert(config_kb_pdf_vector_enabled() == 14);
   assert(config_kb_purge_fence_ttl_s() == 15);
   assert(config_present() == 16);
   assert(strcmp(config_embedder_command_current(NULL), "configured-embedder --json") == 0);
   const char *requested = "request-specific";
   assert(config_embedder_command_current(requested) == requested);
}

static void test_invalid_install_is_atomic(void)
{
   assert(db2_runtime_config_install(NULL) == -1);
   db2_runtime_config_t bad = {.abi_version = DB2_RUNTIME_CONFIG_ABI_VERSION + 1};
   assert(db2_runtime_config_install(&bad) == -1);
   memset(&bad, 'x', sizeof(bad));
   bad.abi_version = DB2_RUNTIME_CONFIG_ABI_VERSION;
   assert(db2_runtime_config_install(&bad) == -1);
   assert(config_kb_curator_cross_repo_k() == 8);
   assert(strcmp(config_embedder_command_current(NULL), "configured-embedder --json") == 0);
}

int main(void)
{
   test_fail_closed_defaults();
   test_full_snapshot();
   test_invalid_install_is_atomic();
   return 0;
}
