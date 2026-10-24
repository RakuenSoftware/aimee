#ifndef DEC_CLI_ACP_H
#define DEC_CLI_ACP_H 1

#include <stddef.h>
#include "provider_cli_adapter.h"
#include <aimee/translation/harness_ir.h>

/* Private creator-owned launch resources. Protocol frames never create these.
 * The creator retains all descriptors/strings until the worker joins its child. */
typedef struct cli_acp_host_launch
{
   int executable_fd, cwd_fd;
   char *const *argv;
   char *const *environment;
   const char *cwd;
   const struct cJSON *mcp_servers;
   long long deadline_ms;
   int (*validate)(void *context);
   int (*service)(aimee_harness_ir_t *state, void *context); /* Producer only. */
   void *context;
} cli_acp_host_launch_t;
/* Pure one-shot representation check, also used before any child starts. */
int cli_acp_request_supported(const provider_cli_cfg_t *cfg);
int cli_acp_timeout_ms(const agent_t *agent);

/* Dispatch an already-captured native client request through its exact admitted
 * acp resource owner. No filesystem fallback. Permission decisions must come
 * from the current interactive control owner; headless scopes refuse them. */
int acp_dispatch_client_request(const cJSON *frame, const provider_cli_cfg_t *cfg,
                                aimee_harness_ir_t *ir, const cJSON *decision, cJSON **reply);

#endif /* DEC_CLI_ACP_H */
