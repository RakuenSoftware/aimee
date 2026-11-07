#ifndef AIMEE_CLI_CODEX_PROFILE_H
#define AIMEE_CLI_CODEX_PROFILE_H
#include "cli_codex.h"
/* Borrowed actual config/read reply. Verify effective settings and contributing
 * layers against the owned launch, returning a credential-free cache digest.
 * No thread, model or cache may start before this proof succeeds. */
int cli_codex_profile_verify(const cli_codex_host_launch_t *host, const struct cJSON *reply,
                             char digest[65]);
#endif
