/* windows/agent_runtime.c: Windows execution engine — agent loop, context assembly, eval, and tool
 * execution */
#include "aimee.h"
#include "agent_exec.h"

int agent_execute_with_tools(const agent_t *agent, const agent_network_t *network,
                             const char *system_prompt, const char *user_prompt, int max_tokens,
                             double temperature, agent_result_t *out)
{
   (void)agent;
   (void)network;
   (void)system_prompt;
   (void)user_prompt;
   (void)max_tokens;
   (void)temperature;
   memset(out, 0, sizeof(*out));
   snprintf(out->error, sizeof(out->error), "tool execution not supported on Windows");
   return -1;
}

int agent_execute_with_tools_for_role(const agent_t *agent, const agent_network_t *network,
                                      const char *role, const char *system_prompt,
                                      const char *user_prompt, int max_tokens, double temperature,
                                      agent_result_t *out)
{
   (void)role;
   return agent_execute_with_tools(agent, network, system_prompt, user_prompt, max_tokens,
                                   temperature, out);
}

int agent_execute_session_with_tools(const agent_t *agent, const agent_network_t *network,
                                     const char *system_prompt, const char *user_prompt,
                                     int max_tokens, double temperature,
                                     struct cJSON *initial_messages,
                                     struct cJSON **updated_messages, agent_result_t *out)
{
   (void)initial_messages;
   if (updated_messages)
      *updated_messages = NULL;
   return agent_execute_with_tools(agent, network, system_prompt, user_prompt, max_tokens,
                                   temperature, out);
}

/* ================================================================
 * From: agent_context.c
 * ================================================================ */
#include "agent.h"

int agent_ssh_setup(const agent_network_t *network, char *key_path_out, size_t key_path_len,
                    char *session_id_out, size_t session_id_len)
{
   (void)network;
   (void)key_path_out;
   (void)key_path_len;
   (void)session_id_out;
   (void)session_id_len;
   return -1;
}

void agent_ssh_cleanup(const agent_network_t *network, const char *key_path, const char *session_id)
{
   (void)network;
   (void)key_path;
   (void)session_id;
}

/* ================================================================
 * From: agent_eval.c
 * ================================================================ */
#include "agent_eval.h"

int agent_eval_load_tasks(const char *suite_dir, eval_task_t *tasks, int max_tasks)
{
   (void)suite_dir;
   (void)tasks;
   (void)max_tasks;
   return 0;
}

/* ================================================================
 * From: agent_tools.c
 * ================================================================ */
#include "agent_tools.h"
#include "cJSON.h"
#include "diff.h"

char *tool_bash(const char *command, int timeout_ms)
{
   (void)command;
   (void)timeout_ms;
   return safe_strdup("{\"stdout\":\"\",\"stderr\":\"not supported on Windows\",\"exit_code\":-1}");
}

char *tool_read_file(const char *path, int offset, int limit, int raw)
{
   (void)offset;
   (void)limit;
   (void)raw; /* Windows minimal path: un-anchored bytes only */
   /* Basic read implementation for Windows */
   char resolved[MAX_PATH_LEN];
   const char *err = guardrails_validate_file_path(path, resolved, sizeof(resolved));
   if (err)
      return safe_strdup(err);

   FILE *f = fopen(path, "r");
   if (!f)
      return safe_strdup("error: file not found");

   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (sz <= 0)
   {
      fclose(f);
      return safe_strdup("");
   }
   if (sz > AGENT_TOOL_OUTPUT_MAX)
      sz = AGENT_TOOL_OUTPUT_MAX;
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return safe_strdup("error: out of memory");
   }
   size_t n = fread(buf, 1, (size_t)sz, f);
   buf[n] = '\0';
   fclose(f);
   return buf;
}

char *tool_write_file(const char *path, const char *content)
{
   char resolved[MAX_PATH_LEN];
   const char *err = guardrails_validate_file_path(path, resolved, sizeof(resolved));
   if (err)
      return safe_strdup(err);

   char *old_content = NULL;
   {
      FILE *rf = fopen(path, "r");
      if (rf)
      {
         fseek(rf, 0, SEEK_END);
         long sz = ftell(rf);
         fseek(rf, 0, SEEK_SET);
         if (sz > 0 && sz < 1024 * 1024)
         {
            old_content = malloc((size_t)sz + 1);
            if (old_content)
            {
               size_t rd = fread(old_content, 1, (size_t)sz, rf);
               old_content[rd] = '\0';
            }
         }
         fclose(rf);
      }
   }

   FILE *f = fopen(path, "w");
   if (!f)
   {
      free(old_content);
      return safe_strdup("error: cannot open file for writing");
   }
   if (content)
      fputs(content, f);
   fclose(f);

   diff_result_t dr;
   if (diff_compute(old_content, content, &dr) == 0 && (dr.additions > 0 || dr.deletions > 0))
   {
      char *summary = diff_format_summary(&dr);
      char *unified = diff_format_unified(old_content, content, &dr);
      cJSON *payload = cJSON_CreateObject();
      cJSON_AddStringToObject(payload, "status", "ok");
      cJSON_AddStringToObject(payload, "path", path);
      cJSON_AddBoolToObject(payload, "changed", 1);
      cJSON_AddStringToObject(payload, "summary", summary ? summary : "changed");
      cJSON_AddItemToObject(payload, "diff", diff_result_to_json(&dr));
      if (unified && unified[0])
         cJSON_AddStringToObject(payload, "unified_diff", unified);
      char *out = cJSON_PrintUnformatted(payload);
      cJSON_Delete(payload);
      free(summary);
      free(unified);
      free(old_content);
      if (out)
         return out;
      return safe_strdup("error: out of memory");
   }

   free(old_content);
   return safe_strdup("ok");
}

char *tool_list_files(const char *path, const char *pattern)
{
   (void)path;
   (void)pattern;
   return safe_strdup("error: tool_list_files not supported on Windows");
}

char *tool_verify(const char *check_type, const char *target, const char *expected)
{
   (void)check_type;
   (void)target;
   (void)expected;
   return safe_strdup("{\"pass\":false,\"reason\":\"not supported on Windows\"}");
}

char *tool_grep(const char *path, const char *pattern, int max_results)
{
   (void)path;
   (void)pattern;
   (void)max_results;
   return safe_strdup("error: tool_grep not supported on Windows");
}

char *tool_git_diff(const char *repo_path, const char *ref)
{
   char cmd[4096];
   snprintf(cmd, sizeof(cmd), "git -C \"%s\" diff %s", repo_path ? repo_path : ".",
            ref ? ref : "HEAD");
   int ec;
   char *out = run_cmd(cmd, &ec);
   return out ? out : safe_strdup("");
}

char *tool_git_status(const char *repo_path)
{
   char cmd[4096];
   snprintf(cmd, sizeof(cmd), "git -C \"%s\" status --porcelain", repo_path ? repo_path : ".");
   int ec;
   char *out = run_cmd(cmd, &ec);
   return out ? out : safe_strdup("");
}

char *tool_env_get(const char *name)
{
   if (!name)
      return safe_strdup("error: missing name");
   const char *val = getenv(name);
   return safe_strdup(val ? val : "");
}

char *tool_test(const char *path, const char *check)
{
   (void)path;
   (void)check;
   return safe_strdup("{\"pass\":false,\"reason\":\"not supported on Windows\"}");
}

char *tool_git_log(const char *repo_path, int count)
{
   char cmd[4096];
   snprintf(cmd, sizeof(cmd), "git -C \"%s\" log --oneline -n %d", repo_path ? repo_path : ".",
            count > 0 ? count : 10);
   int ec;
   char *out = run_cmd(cmd, &ec);
   return out ? out : safe_strdup("");
}

char *tool_request_input(const char *question)
{
   (void)question;
   return safe_strdup("error: interactive input not supported on Windows");
}

char *tool_code_search(const char *query, const char *project, int max_results)
{
   (void)query;
   (void)project;
   (void)max_results;
   return safe_strdup("error: code_search not supported on Windows");
}

char *tool_create_note(const char *title, const char *content, const char *tags)
{
   (void)title;
   (void)content;
   (void)tags;
   return safe_strdup("error: create_note not supported on Windows");
}

char *tool_list_notes(const char *tag, int limit)
{
   (void)tag;
   (void)limit;
   return safe_strdup("error: list_notes not supported on Windows");
}

char *tool_search_notes(const char *query)
{
   (void)query;
   return safe_strdup("error: search_notes not supported on Windows");
}

char *dispatch_tool_call_ctx(const char *name, const char *arguments_json, int timeout_ms)
{
   if (!name)
      return safe_strdup("error: missing tool name");
   if (strcmp(name, "bash") == 0)
      return tool_bash(arguments_json, timeout_ms);
   if (strcmp(name, "read_file") == 0 || strcmp(name, "Read") == 0)
   {
      cJSON *args = cJSON_Parse(arguments_json);
      cJSON *p = args ? cJSON_GetObjectItem(args, "path") : NULL;
      char *result = tool_read_file(p && cJSON_IsString(p) ? p->valuestring : "", 0, 0, 1);
      cJSON_Delete(args);
      return result;
   }
   if (strcmp(name, "write_file") == 0 || strcmp(name, "Write") == 0)
   {
      cJSON *args = cJSON_Parse(arguments_json);
      cJSON *p = args ? cJSON_GetObjectItem(args, "path") : NULL;
      cJSON *c = args ? cJSON_GetObjectItem(args, "content") : NULL;
      char *result = tool_write_file(p && cJSON_IsString(p) ? p->valuestring : "",
                                     c && cJSON_IsString(c) ? c->valuestring : "");
      cJSON_Delete(args);
      return result;
   }
   return safe_strdup("error: tool not supported on Windows");
}

char *dispatch_tool_call(const char *name, const char *arguments_json, int timeout_ms)
{
   return dispatch_tool_call_ctx(name, arguments_json, timeout_ms);
}
