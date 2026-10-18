#ifndef AIMEE_CLI_PROXY_H
#define AIMEE_CLI_PROXY_H

#include <stddef.h>

typedef struct cli_proxy cli_proxy_t;

/* One callback owns one request until it returns. Body/header views and IO
 * context are borrowed; the immutable host context lives until stop has joined
 * every worker. Write raw HTTP response bytes through write_response, or notify
 * response_started immediately before an existing FD response handler takes over.
 * on_upstream lets the listener interrupt quiet remote IO on downstream EOF. */
typedef struct
{
   int downstream_fd;
   void *context;
   int (*write_response)(const void *data, unsigned long length, void *context);
   void (*on_upstream)(int fd, void *context);
   void (*response_started)(void *context);
   int (*cancelled)(void *context);
} cli_proxy_io_t;

typedef int (*cli_proxy_forward_fn)(const char *method, const char *path, const char *headers,
                                    const void *body, unsigned long body_len,
                                    const char *session_id, const cli_proxy_io_t *io,
                                    void *host_context);

/* Explicit transport for an already authorized host scope. No global CLI auth
 * is consulted. A nonempty bound session overrides client-supplied correlation.
 * Callback context remains caller-owned and must outlive cli_proxy_stop(). */
cli_proxy_t *cli_proxy_start_bound(unsigned port, const char *local_token, const char *session_id,
                                   unsigned *bound_port, cli_proxy_forward_fn forward,
                                   void *host_context);

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
