#ifndef AIMEE_SERVER_DELEGATE_INPUT_H
#define AIMEE_SERVER_DELEGATE_INPUT_H
#include <aimee/ir/session_event.h>
/* Public text compatibility is imported once. Canonical producers use request.
 * The owned source and its read-only routing/job summary have distinct owners. */
int server_delegate_input_parse(const struct cJSON *envelope, int allow_text, aimee_request_t *out,
                                char **task_text);
int server_delegate_input_has_policy(const aimee_request_t *request);
size_t server_delegate_input_task_size(const aimee_request_t *request);
void server_delegate_input_attenuate(aimee_request_t *request);
int server_delegate_request_prepare(const aimee_request_t *source, const aimee_block_t *host_policy,
                                    int max_tokens, aimee_request_t *out);
#endif
