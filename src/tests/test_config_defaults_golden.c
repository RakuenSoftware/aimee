/* test_config_defaults_golden.c: pins the exact config_load default for every FLAT
 * config field (Proposal A, step 1). This is the safety net for retiring the
 * flat-field arm of config_set_defaults: it asserts config_load (with NO
 * aimee.yaml) yields these exact values, so migrating those defaults to a
 * table-driven application cannot silently change a shipped default.
 * Golden values were captured from the pre-migration config_set_defaults. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aimee.h"
#include "config_fields.h"
#include "platform_path.h"
#include "platform_test_util.h"

typedef struct
{
   const char *key;
   const char *want_json;
} golden_t;
static const golden_t g_golden[] = {
    {"db2_url", "\"\""},
    /* EMPTY: a fresh install has no primary until one is chosen. It used to be
     * "claude", which pinned the chat turn to a synthesized claude CLI agent and
     * broke chat on any machine without that CLI. See config_fields.c. */
    {"provider", "\"\""},
    {"default_persona", "\"engineer\""},
    {"claude_model", "\"\""},
    {"openai_endpoint", "\"https://api.openai.com/v1\""},
    {"openai_model", "\"gpt-4o\""},
    {"openai_key_cmd", "\"\""},
    {"guardrail_mode", "\"approve\""},
    {"embedder_command", "\"\""},
    {"embedder_model", "\"\""},
    {"embedder_url", "\"\""},
    {"kb_client_url", "\"\""},
    {"kb_client_bearer_token", "\"\""},
    {"memory_rerank_mode", "\"\""},
    {"ingress_preinject_enabled", "true"},
    {"code_context_mode", "\"on\""},
    {"ingress_preinject_anthropic_enabled", "false"},
    {"ingress_compress_enabled", "true"},
    {"gateway_prevent_subagents", "false"},
    {"gateway_pin_model", "false"},
    {"require_session_worktree", "true"},
    {"require_aimee_memory", "true"},
    {"require_aimee_git", "true"},
    {"subagent_ban_enabled", "true"},
    {"delegate_sandbox_require_isolation", "false"},
    {"delegate_sandbox_learn_packages", "true"},
    {"typed_facts_enabled", "true"},
    {"kb_pdf_ingest_enabled", "false"},
    {"kb_pdf_vector_enabled", "false"},
    {"kb_pdf_tsr_enabled", "false"},
    {"tsr_command", "\"\""},
    {"kb_pdf_assets_enabled", "false"},
    {"kb_pdf_blob_dir", "\"\""},
    {"kb_pdf_blob_recon_secs", "3600"},
    {"kb_pdf_blob_orphan_alarm_mb", "1024"},
    {"kb_pdf_ocr_enabled", "false"},
    {"ocr_command", "\"\""},
    {"css_style_graph_enabled", "true"},
    {"code_cochange_git_enabled", "true"},
    {"wfe_live_forge_enabled", "false"},
    {"wfe_proposals_autoscan_enabled", "false"},
    {"client_integrations_enabled", "true"},
    {"audit_action_enabled", "true"},
    {"audit_worm_enabled", "false"},
    {"css_render_command",
     "\"curl -s --max-time 30 --data-binary @- http://aimee-css-render:8780/render\""},
    {"kb_evidence_emit_enabled", "false"},
    {"fidelity_check_enabled", "false"},
    {"autonomous", "false"},
    {"max_iterations", "0"},
    {"max_iterations_delegate", "0"},
    {"verify_enabled", "false"},
    {"delegate_graph_context_enabled", "false"},
    {"verify_cross_project", "false"},
};
static const int g_golden_n = (int)(sizeof(g_golden) / sizeof(g_golden[0]));

int main(void)
{
   char tmp[512];
   snprintf(tmp, sizeof(tmp), "%s/aimee-golden-XXXXXX", platform_tmpdir());
   char *d = platform_mkdtemp(tmp);
   assert(d);
   setenv("HOME", d, 1);
   setenv("AIMEE_HOME", d, 1);
   config_t cfg;
   config_load(&cfg); /* no aimee.yaml -> pure defaults */

   int bad = 0;
   for (int i = 0; i < g_golden_n; i++)
   {
      const config_field_t *f = config_field_lookup(g_golden[i].key);
      assert(f && "golden key must exist in config_fields[]");
      cJSON *v = config_field_value_json(&cfg, f);
      char *got = v ? cJSON_PrintUnformatted(v) : strdup("null");
      if (strcmp(got, g_golden[i].want_json) != 0)
      {
         bad++;
         fprintf(stderr, "  DEFAULT CHANGED: %s: want %s got %s\n", g_golden[i].key,
                 g_golden[i].want_json, got);
      }
      free(got);
      if (v)
         cJSON_Delete(v);
   }
   printf("config defaults golden: %d flat fields checked\n", g_golden_n);
   assert(bad == 0 && "a flat config default changed");
   printf("  PASS: all flat config defaults unchanged\n");
   return 0;
}
