#ifndef HOST_INPUT_ATTACHMENT_H
#define HOST_INPUT_ATTACHMENT_H
#include <aimee/ir/aimee_ir.h>
#include "server_session_capture.h"
#include "server_session_log.h"
#include "agent_types.h"
int agent_dispatch_original_input(const agent_t *, const agent_network_t *, const char *,
                                  const char *, const struct cJSON *, int, double, int,
                                  agent_result_t *);
int server_session_capture_original_input_begin(server_session_capture_t *,
                                                const aimee_message_t *);
/* Cached cursor immediately before the exact successful atomic pair, independent
 * of later appends. Caller holds capture ownership. No reads or writes. */
int server_session_capture_original_input_before(server_session_capture_t *, char before[32]);
extern const unsigned aimee_chat_original_input_delivery_contract_v1;
int server_session_log_begin_original(const char *, const struct cJSON *, const char *,
                                      const char *, const char *, server_session_log_t **);
/* Trusted startup callback verifies an active registered transform owner and
 * exact safe text/opaque attachment. Structural decode alone is never authority. */
typedef int (*server_input_attachment_admit_fn)(const char *session, const char *turn,
                                                const char *principal,
                                                const aimee_message_t *message, void *state);
int server_input_attachment_install(server_input_attachment_admit_fn, void *);
#endif
