#ifndef DEC_DELEGATE_EGRESS_ADAPTER_H
#define DEC_DELEGATE_EGRESS_ADAPTER_H

#include <stddef.h>

/* Legacy server handoff to the Go sole-egress module.
 *
 * This boundary makes no destination or network decision and opens no network
 * socket. It only transfers an accepted Unix client fd and request bytes to the
 * Go module. The caller owns and closes client_fd. */
int delegate_egress_adapter_serve(int client_fd, int is_uds, const char *head, size_t head_len,
                                  const char *tag);
/* Same Go policy, with actual Unix-fd verification and owner-bounded lifetime.
 * Cancellation stops the owned helper; it never synthesizes an operator receipt. */
int delegate_egress_adapter_serve_owned(int client_fd, const char *head, size_t head_len,
                                        const char *tag, int timeout_ms, int (*cancelled)(void *),
                                        void *context);

#endif /* DEC_DELEGATE_EGRESS_ADAPTER_H */
