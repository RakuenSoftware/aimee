#ifndef AIMEE_SERVER_ACP_EXECUTION_H
#define AIMEE_SERVER_ACP_EXECUTION_H
#include "provider_cli_adapter.h"
/* The strong host dispatch alone creates the retained external-agent owner.
 * No provider SDK/profile/cache provenance or ambient fallback is implied. */
int server_native_execute_owned(const provider_cli_cfg_t *cfg, agent_result_t *out);
int server_acp_execute_owned(const provider_cli_cfg_t *cfg, agent_result_t *out);
#endif
