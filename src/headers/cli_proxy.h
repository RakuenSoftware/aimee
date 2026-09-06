#ifndef AIMEE_CLI_PROXY_H
#define AIMEE_CLI_PROXY_H

#include <stddef.h>

typedef struct cli_proxy cli_proxy_t;

/* A bounded, authenticated loopback model listener. Remote configuration must
 * be loaded before start and remain immutable until stop. */
cli_proxy_t *cli_proxy_start(unsigned port, const char *local_token, const char *session_id,
                             unsigned *bound_port);
void cli_proxy_stop(cli_proxy_t *proxy);
int cli_proxy_cmd(int argc, char **argv);
/* Own the listener while a provider client runs, then stop it and return the
 * client's exit status. No shell expansion of argv. */
int cli_proxy_launch(char *const argv[], const char *session_id);

#endif
