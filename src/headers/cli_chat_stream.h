/* Canonical first-party chat subscriber; callback tool IDs are opaque and stable. */
#ifndef DEC_CLI_CHAT_STREAM_H
#define DEC_CLI_CHAT_STREAM_H
struct cJSON;
typedef void (*cli_chat_tool_fn)(const char *phase, const char *id, const char *name,
                                 void *context);
char *cli_chat_request_stream(const char *endpoint, const char *bearer, const struct cJSON *request,
                              int timeout_ms, void (*text)(const char *, void *),
                              cli_chat_tool_fn tool, void *context);
char *cli_chat_stream(const char *sock, const char *session, const char *message,
                      void (*text)(const char *, void *), cli_chat_tool_fn tool, void *context);
#endif
