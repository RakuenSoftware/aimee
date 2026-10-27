#include "provider_cli_adapter.h"
#include "agent_exec.h"
#include "agent_config.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int native_limit_test_tls;
int native_limit_test_observed;
int native_limit_test_calls;
int native_limit_test_host_unavailable;

/* Strong server-owned TLS override for the actual weak adapter seam. */
int agent_native_request_turn_limit(void)
{
   return native_limit_test_tls;
}
const char *run_cmd_get_cwd(void)
{
   return "/fixture/owned-workspace";
}
int provider_cli_adapter_host_execute(const provider_cli_cfg_t *cfg, agent_result_t *out)
{
   assert(cfg && cfg->agent && !cfg->role && !cfg->no_tools);
   assert(!strcmp(cfg->cwd, "/fixture/owned-workspace"));
   assert(!cfg->system_prompt && !cfg->user_prompt);
   assert(cfg->canonical_request || native_limit_test_host_unavailable);
   if (cfg->canonical_request)
   {
      assert(cfg->canonical_request->n_messages == 1);
      const aimee_message_t *message = cfg->canonical_request->messages;
      assert(!strcmp(message->role, "user") && message->n_blocks == 1);
      assert(message->blocks[0].type == AIMEE_BLK_TEXT && !strcmp(message->blocks[0].text, "task"));
   }
   native_limit_test_calls++;
   native_limit_test_observed = cfg->native_turn_limit;
   out->success = 0;
   return native_limit_test_host_unavailable
              ? 1
              : AGENT_RC_CONTEXT_REFUSED; /* No transport fallback is permitted. */
}
void agent_request_creds_snapshot(agent_request_creds_t *out)
{
   (void)out;
   abort();
}
int server_session_log_active(void)
{
   abort();
}
