/* provider_cli_adapter.h: registry and shared loop for provider CLI delegates. */
#ifndef DEC_PROVIDER_CLI_ADAPTER_H
#define DEC_PROVIDER_CLI_ADAPTER_H 1

#include "aimee.h"
#include "agent_types.h"

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      PROVIDER_CLI_PROTO_HEURISTIC = 0,
      PROVIDER_CLI_PROTO_STABLE = 1,
      PROVIDER_CLI_PROTO_NATIVE = 2,
   } provider_cli_proto_stability_t;

   typedef struct
   {
      int max_context_tokens; /* rough cap; 0 = unknown */
      int supports_tool_use;
      provider_cli_proto_stability_t proto_stability;
      float write_confidence;
   } cli_adapter_caps_t;

   typedef enum
   {
      CLI_EVENT_NONE = 0,
      CLI_EVENT_TEXT_DELTA,
      CLI_EVENT_TOOL_START,
      CLI_EVENT_TOOL_COMPLETE,
      CLI_EVENT_TURN_COMPLETE,
      CLI_EVENT_ERROR,
   } cli_event_type_t;

   typedef struct
   {
      cli_event_type_t type;
      char text[4096];
      char tool_name[128];
      int write_event;
      int prompt_tokens;
      int completion_tokens;
      int cache_write_tokens;
      int cache_read_tokens;
      int latency_ms;
   } cli_event_t;

   typedef struct
   {
      const agent_t *agent;
      const char *cwd;
      const char *system_prompt;
      const char *user_prompt;
   } provider_cli_cfg_t;

   typedef struct provider_cli_adapter
   {
      const char *cli_kind;     /* "codex", "claude", "gemini", "mistral" */
      const char *display_name; /* human-readable CLI name for diagnostics */
      cli_adapter_caps_t caps;

      int (*spawn)(const provider_cli_cfg_t *cfg, const char *task_prompt, int *stdin_fd,
                   int *stdout_fd, pid_t *pid_out);
      int (*parse_line)(const char *line, cli_event_t *event_out);
      int (*format_tool_result)(const char *tool_name, const char *result_json, char *buf,
                                size_t buf_sz);
      int (*is_write_event)(const cli_event_t *event);
      int (*build_prompt)(const provider_cli_cfg_t *cfg, const char *system_prompt,
                          const char *task, char *buf, size_t buf_sz);

      /* Build the adapter's full argv into tokens[] (caller array of >= cap+1
       * slots), NUL-terminating it. The first *split_count tokens are
       * heap-allocated (the parsed cli_cmd) and must be freed with
       * provider_cli_free_tokens(tokens, *split_count); the remaining tokens are
       * borrowed literals / agent fields. Returns argc (>=0) or -1 on error.
       * Adapters that can run on a DETACHED (thin-client) workspace implement
       * this so the same argv that spawn() would fork locally is instead
       * marshalled to the client and run there. NULL = no remote support. */
      int (*build_argv)(const provider_cli_cfg_t *cfg, char **tokens, int cap, int *split_count);

      /* Native provider bridge for routes that historically used provider-CLI
       * config but should execute through Aimee's HTTP provider loop. */
      const char *native_provider;
      const char *native_default_endpoint;
      const char *native_default_model;
      const char *native_api_key_env;

      /* Optional full adapter override for protocols with an existing native
       * implementation, such as Codex app-server JSON-RPC. */
      int (*execute)(const provider_cli_cfg_t *cfg, agent_result_t *out);
   } provider_cli_adapter_t;

   const provider_cli_adapter_t *provider_cli_adapter_get(const char *cli_kind);
   int provider_cli_adapter_prepare_native_agent(const provider_cli_adapter_t *adapter,
                                                 const agent_t *agent, agent_t *native_agent,
                                                 char *errbuf, size_t errbuf_len);
   int provider_cli_adapter_execute(const provider_cli_adapter_t *adapter, const agent_t *agent,
                                    const char *cwd, const char *system_prompt,
                                    const char *user_prompt, agent_result_t *out);

   int provider_cli_spawn_argv(const provider_cli_cfg_t *cfg, char *const argv[], int *stdin_fd,
                               int *stdout_fd, pid_t *pid_out);
   int provider_cli_split_command(const char *cli_cmd, char **argv, int max_args, char *errbuf,
                                  size_t errbuf_len);
   void provider_cli_free_tokens(char **tokens, int count);
   int provider_cli_command_has_shell_operators(const char *cli_cmd);
   int provider_cli_check_available(const char *cli_cmd, const char *cli_kind);

   int provider_cli_default_build_prompt(const provider_cli_cfg_t *cfg, const char *system_prompt,
                                         const char *task, char *buf, size_t buf_sz);
   int provider_cli_format_json_tool_result(const char *tool_name, const char *result_json,
                                            char *buf, size_t buf_sz);
   int provider_cli_event_is_write(const cli_event_t *event);
   int provider_cli_parse_json_line_common(const char *line, cli_event_t *event_out);

   extern const provider_cli_adapter_t codex_provider_cli_adapter;
   extern const provider_cli_adapter_t claude_provider_cli_adapter;
   extern const provider_cli_adapter_t mistral_provider_cli_adapter;

#ifdef __cplusplus
}
#endif

#endif /* DEC_PROVIDER_CLI_ADAPTER_H */
