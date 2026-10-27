/* Strong protocol selection. Missing ownership never authorizes a fallback. */
#include "server_acp_execution.h"
#include <string.h>
int provider_cli_adapter_host_execute(const provider_cli_cfg_t *cfg, agent_result_t *out)
{
   if (!out)
      return -1;
   if (!cfg || !cfg->agent)
      return 1;
   return !strcmp(cfg->agent->cli_kind, "acp") ? server_acp_execute_owned(cfg, out)
                                               : server_native_execute_owned(cfg, out);
}
