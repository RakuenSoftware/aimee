/* Keep bench-perf focused on the operations it measures. These optional
 * production integrations are independently tested; the benchmark exercises
 * the deterministic guardrail path and local agent routing without pulling in
 * the entire server, semantic sidecar, skill loader, or release gate. */
#include "aimee.h"
#include "agent_config.h"
#include "git_verify.h"
#include "guardrails.h"
#include "guardrails_semantic.h"
#include "model_registry.h"
#include "workspace_provider.h"
#include <aimee/skills/skill.h>

#include <string.h>

int pre_tool_check(const char *tool_name, const char *input_json, session_state_t *state,
                   const char *guardrail_mode, const char *cwd, char *msg_buf, size_t msg_len)
{
   return pre_tool_check_inner(tool_name, input_json, state, guardrail_mode, cwd, msg_buf, msg_len);
}

int model_capability_get(const char *provider, const char *model_id, model_capability_t *out)
{
   (void)provider;
   (void)model_id;
   if (out)
      memset(out, 0, sizeof(*out));
   return 0;
}

int workspace_turn_container_bound(void)
{
   return 0;
}

const workspace_provider_t *workspace_provider_active(void)
{
   static const workspace_provider_t shared = {.kind = WS_PROVIDER_SHARED};
   return &shared;
}

int verify_gate_blocks(const char *target_root, const char *expected_commit, char *msg,
                       size_t msg_len)
{
   (void)target_root;
   (void)expected_commit;
   if (msg && msg_len)
      msg[0] = '\0';
   return 0;
}

int skill_trigger_matches(const char *project_root, const char *name, const char *tool_name,
                          const char *subject)
{
   (void)project_root;
   (void)name;
   (void)tool_name;
   (void)subject;
   return 0;
}

double gsem_effective_warn_threshold(void)
{
   return 0.0;
}

double gsem_effective_prompt_threshold(void)
{
   return 0.0;
}

double gsem_effective_block_threshold(void)
{
   return 0.0;
}

void gsem_build_input(const char *tool_name, cJSON *input_json, const char *cwd,
                      const char *session_mode, gsem_input_t *out)
{
   (void)tool_name;
   (void)input_json;
   (void)cwd;
   (void)session_mode;
   if (out)
      memset(out, 0, sizeof(*out));
}

int gsem_assess(const gsem_input_t *in, const char *command, gsem_output_t *out)
{
   (void)in;
   (void)command;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

const char *gsem_policy(const gsem_output_t *out, double warn_t, double prompt_t, double block_t)
{
   (void)out;
   (void)warn_t;
   (void)prompt_t;
   (void)block_t;
   return "allow";
}

int gsem_format_advisory_message(char *buf, size_t buf_len, const char *action,
                                 const gsem_output_t *out)
{
   (void)action;
   (void)out;
   if (buf && buf_len)
      buf[0] = '\0';
   return 0;
}

void gsem_record(const char *session_id_value, const gsem_input_t *in, const gsem_output_t *out,
                 const char *final_action, int dry_run)
{
   (void)session_id_value;
   (void)in;
   (void)out;
   (void)final_action;
   (void)dry_run;
}
