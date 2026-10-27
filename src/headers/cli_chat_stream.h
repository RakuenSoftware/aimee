/* Canonical first-party chat subscriber; callback tool IDs are opaque and stable. */
#ifndef DEC_CLI_CHAT_STREAM_H
#define DEC_CLI_CHAT_STREAM_H
struct cJSON;
#include "cli_client.h"
#include <aimee/ir/aimee_ir.h>
typedef void (*cli_chat_tool_fn)(const char *phase, const char *id, const char *name,
                                 void *context);
typedef struct cli_chat_result
{
   aimee_message_t *messages;
   int n_messages;
} cli_chat_result_t;
void cli_chat_result_free(cli_chat_result_t *result);
/* Owned ordered canonical messages for this turn, including tool results and
 * all assistant blocks. No display projection or reconstructed messages. */
int cli_chat_request_stream_ir(const char *endpoint, const char *bearer,
                               const struct cJSON *request, int timeout_ms,
                               cli_chat_result_t *conversation);
/* Borrow a caller-owned stream so cancellation interrupts this same exchange. */
int cli_chat_request_stream_ir_on(cli_http_stream_t *stream, const cli_http_options_t *options,
                                  const struct cJSON *request, cli_chat_result_t *conversation);
/* Local language ABI only: existing canonical codecs own parsing/serialization. */
int cli_chat_request_stream_json(cli_http_stream_t *stream, const cli_http_options_t *options,
                                 const char *request_json, char **messages_json);
void cli_chat_string_free(char *value);
char *cli_chat_request_stream(const char *endpoint, const char *bearer, const struct cJSON *request,
                              int timeout_ms, void (*text)(const char *, void *),
                              cli_chat_tool_fn tool, void *context);
/* CLI/ACP convenience API uses process configuration; omitted from the shared library. */
char *cli_chat_stream(const char *sock, const char *session, const char *message,
                      void (*text)(const char *, void *), cli_chat_tool_fn tool, void *context);
#endif
