#ifndef AIMEE_CLI_CLAUDE_NATIVE_H
#define AIMEE_CLI_CLAUDE_NATIVE_H
#include "provider_cli_adapter.h"
typedef struct cli_claude_host_launch
{
   int executable_fd, cwd_fd;
   const char *home, *cwd, *mcp_config, *gateway_url, *gateway_token, *expected_profile;
   char *const *environment;
   const aimee_request_t *catalog;
   int (*init)(const struct cJSON *frame, void *context);
   void *context;
   int (*progress)(void *context);
   void *progress_context;
} cli_claude_host_launch_t;
/* Held descriptor + explicit argv/env only. No ambient CLI registration,
 * credential HOME, inherited server descriptor or workspace execution path. */
int cli_claude_native_spawn(const provider_cli_cfg_t *cfg, int *input, int *output, pid_t *pid);
/* Actual argv/environment profile, with verified ephemeral capability/HOME and
 * scoped resume resources normalized out before hashing. */
int cli_claude_native_launch_digest(const provider_cli_cfg_t *cfg, char digest[65]);
/* Pure readback of the actual SDK's initialization before host model release. */
int cli_claude_native_verify(const cli_claude_host_launch_t *host, const struct cJSON *frame);
#endif
