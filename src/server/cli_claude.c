/* cli_claude.c: legacy Claude provider-CLI adapter. */
#include "provider_cli_adapter.h"

#include "cJSON.h"

#include <stdio.h>
#include <string.h>

#define CLAUDE_ARG_MAX 48

/* Build `claude -p` argv into tokens[]. The leading *split_count tokens (the
 * parsed cli_cmd, default "claude") are heap-allocated; the rest are borrowed.
 * The prompt is NOT in argv — it is fed on stdin by the caller. Returns argc or
 * -1. Shared by the local spawn and the detached (thin-client) exec_stream path. */
static int claude_build_argv(const provider_cli_cfg_t *cfg, char **tokens, int cap,
                             int *split_count)
{
   char err[128];
   const agent_t *agent = cfg ? cfg->agent : NULL;
   const char *cmd = (agent && agent->cli_cmd[0]) ? agent->cli_cmd : "claude";
   int count = provider_cli_split_command(cmd, tokens, cap, err, sizeof(err));
   if (count < 0)
      return -1;
   if (split_count)
      *split_count = count;

   int argc = count;
#define CLAUDE_ADD_ARG(s)                                                                          \
   do                                                                                              \
   {                                                                                               \
      if (argc >= cap)                                                                             \
      {                                                                                            \
         provider_cli_free_tokens(tokens, count);                                                  \
         return -1;                                                                                \
      }                                                                                            \
      tokens[argc++] = (char *)(s);                                                                \
      tokens[argc] = NULL;                                                                         \
   } while (0)

   CLAUDE_ADD_ARG("-p");
   CLAUDE_ADD_ARG("--output-format");
   CLAUDE_ADD_ARG("stream-json");
   CLAUDE_ADD_ARG("--verbose");
   CLAUDE_ADD_ARG("--include-partial-messages");
   CLAUDE_ADD_ARG("--allowedTools");
   CLAUDE_ADD_ARG("Bash(*)");
   CLAUDE_ADD_ARG("Edit");
   CLAUDE_ADD_ARG("Read");
   CLAUDE_ADD_ARG("Write");
   CLAUDE_ADD_ARG("Glob");
   CLAUDE_ADD_ARG("Grep");
   CLAUDE_ADD_ARG("WebFetch");
   CLAUDE_ADD_ARG("WebSearch");
   CLAUDE_ADD_ARG("NotebookEdit");
   CLAUDE_ADD_ARG("--disallowedTools");
   CLAUDE_ADD_ARG("AskUserQuestion,Agent,RemoteTrigger");
   if (cfg && cfg->system_prompt && cfg->system_prompt[0])
   {
      CLAUDE_ADD_ARG("--append-system-prompt");
      CLAUDE_ADD_ARG(cfg->system_prompt);
   }
   if (agent && agent->model[0])
   {
      CLAUDE_ADD_ARG("--model");
      CLAUDE_ADD_ARG(agent->model);
   }
#undef CLAUDE_ADD_ARG
   return argc;
}

static int claude_spawn(const provider_cli_cfg_t *cfg, const char *task_prompt, int *stdin_fd,
                        int *stdout_fd, pid_t *pid_out)
{
   (void)task_prompt;
   char *tokens[CLAUDE_ARG_MAX + 1] = {0};
   int split = 0;
   int argc = claude_build_argv(cfg, tokens, CLAUDE_ARG_MAX, &split);
   if (argc < 0)
      return -1;

   int rc = provider_cli_spawn_argv(cfg, tokens, stdin_fd, stdout_fd, pid_out);
   provider_cli_free_tokens(tokens, split);
   return rc;
}

static int claude_build_prompt(const provider_cli_cfg_t *cfg, const char *system_prompt,
                               const char *task, char *buf, size_t buf_sz)
{
   (void)cfg;
   (void)system_prompt;
   if (!task || !task[0] || !buf || buf_sz == 0)
      return -1;
   snprintf(buf, buf_sz, "%s", task);
   return 0;
}

static const char *json_string(cJSON *obj, const char *name)
{
   cJSON *v = obj ? cJSON_GetObjectItem(obj, name) : NULL;
   return (v && cJSON_IsString(v)) ? v->valuestring : NULL;
}

static void claude_parse_usage(cJSON *usage, cli_event_t *event_out)
{
   if (!usage || !cJSON_IsObject(usage))
      return;
   cJSON *input = cJSON_GetObjectItem(usage, "input_tokens");
   if (input && cJSON_IsNumber(input))
      event_out->prompt_tokens = input->valueint;
   cJSON *output = cJSON_GetObjectItem(usage, "output_tokens");
   if (output && cJSON_IsNumber(output))
      event_out->completion_tokens = output->valueint;
   cJSON *cache_write = cJSON_GetObjectItem(usage, "cache_creation_input_tokens");
   if (cache_write && cJSON_IsNumber(cache_write))
      event_out->cache_write_tokens = cache_write->valueint;
   cJSON *cache_read = cJSON_GetObjectItem(usage, "cache_read_input_tokens");
   if (cache_read && cJSON_IsNumber(cache_read))
      event_out->cache_read_tokens = cache_read->valueint;
}

static int claude_parse_tool_use(cJSON *tool, cli_event_t *event_out)
{
   if (!tool || !cJSON_IsObject(tool))
      return 0;
   const char *type = json_string(tool, "type");
   if (type && strcmp(type, "tool_use") != 0)
      return 0;
   const char *name = json_string(tool, "name");
   if (!name || !name[0])
      return 0;
   event_out->type = CLI_EVENT_TOOL_START;
   snprintf(event_out->tool_name, sizeof(event_out->tool_name), "%s", name);
   event_out->write_event = provider_cli_event_is_write(event_out);
   return 1;
}

static int claude_parse_content(cJSON *content, cli_event_t *event_out)
{
   if (!content || !cJSON_IsArray(content))
      return 0;
   cJSON *item;
   cJSON_ArrayForEach(item, content)
   {
      if (claude_parse_tool_use(item, event_out))
         return 1;
   }
   return 0;
}

static int claude_parse_line(const char *line, cli_event_t *event_out)
{
   if (!line || !event_out)
      return 0;
   memset(event_out, 0, sizeof(*event_out));

   cJSON *obj = cJSON_Parse(line);
   if (!obj || !cJSON_IsObject(obj))
   {
      if (obj)
         cJSON_Delete(obj);
      return 0;
   }

   int matched = 0;
   const char *type = json_string(obj, "type");
   if (type && strcmp(type, "stream_event") == 0)
   {
      cJSON *event = cJSON_GetObjectItem(obj, "event");
      const char *etype = json_string(event, "type");
      if (etype && strcmp(etype, "content_block_delta") == 0)
      {
         cJSON *delta = cJSON_GetObjectItem(event, "delta");
         const char *dts = json_string(delta, "type");
         const char *text = json_string(delta, "text");
         if (dts && strcmp(dts, "text_delta") == 0 && text && text[0])
         {
            event_out->type = CLI_EVENT_TEXT_DELTA;
            snprintf(event_out->text, sizeof(event_out->text), "%s", text);
            matched = 1;
         }
      }
      else if (etype && strcmp(etype, "content_block_start") == 0)
      {
         matched = claude_parse_tool_use(cJSON_GetObjectItem(event, "content_block"), event_out);
      }
      else if (etype && strcmp(etype, "message_start") == 0)
      {
         cJSON *message = cJSON_GetObjectItem(event, "message");
         claude_parse_usage(message ? cJSON_GetObjectItem(message, "usage") : NULL, event_out);
         matched = event_out->prompt_tokens || event_out->completion_tokens ||
                   event_out->cache_write_tokens || event_out->cache_read_tokens;
      }
      else if (etype && strcmp(etype, "message_delta") == 0)
      {
         claude_parse_usage(cJSON_GetObjectItem(event, "usage"), event_out);
         matched = event_out->prompt_tokens || event_out->completion_tokens ||
                   event_out->cache_write_tokens || event_out->cache_read_tokens;
      }
   }
   else if (type && strcmp(type, "assistant") == 0)
   {
      cJSON *message = cJSON_GetObjectItem(obj, "message");
      matched =
          claude_parse_content(message ? cJSON_GetObjectItem(message, "content") : NULL, event_out);
      claude_parse_usage(message ? cJSON_GetObjectItem(message, "usage") : NULL, event_out);
      matched = matched || event_out->prompt_tokens || event_out->completion_tokens ||
                event_out->cache_write_tokens || event_out->cache_read_tokens;
   }
   else if (type && strcmp(type, "result") == 0)
   {
      event_out->type = CLI_EVENT_TURN_COMPLETE;
      matched = 1;
      const char *result = json_string(obj, "result");
      if (result && result[0])
         snprintf(event_out->text, sizeof(event_out->text), "%s", result);
      claude_parse_usage(cJSON_GetObjectItem(obj, "usage"), event_out);
      cJSON *duration = cJSON_GetObjectItem(obj, "duration_ms");
      if (duration && cJSON_IsNumber(duration))
         event_out->latency_ms = duration->valueint;
   }

   cJSON_Delete(obj);
   return matched;
}

const provider_cli_adapter_t claude_provider_cli_adapter = {
    .cli_kind = "claude",
    .display_name = "Claude CLI",
    .caps = {.max_context_tokens = 200000,
             .supports_tool_use = 1,
             .proto_stability = PROVIDER_CLI_PROTO_STABLE,
             .write_confidence = 0.95f},
    .spawn = claude_spawn,
    .parse_line = claude_parse_line,
    .format_tool_result = provider_cli_format_json_tool_result,
    .is_write_event = provider_cli_event_is_write,
    .build_prompt = claude_build_prompt,
    .build_argv = claude_build_argv,
    .execute = NULL,
};
