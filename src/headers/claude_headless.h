#ifndef AIMEE_CLAUDE_HEADLESS_H
#define AIMEE_CLAUDE_HEADLESS_H
#include "provider_cli_adapter.h"
typedef int (*claude_headless_session_fn)(const char *native_id, void *context);
/* cfg.home is materialized by the host. Returning from a failed IR sink stops
 * the process before accepting further output. Child stdin carries the prompt. */
int claude_headless_run(const provider_cli_cfg_t *cfg, claude_headless_session_fn bind_session,
                        void *context, agent_result_t *out);
#endif
