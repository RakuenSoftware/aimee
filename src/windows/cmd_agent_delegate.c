/* cmd_agent_delegate.c: Windows synchronous background dispatch (no fork). */
#include "aimee.h"
#include "agent.h"
#include "agent_config.h"
#include "cmd_agent_delegate_impl.h"
#include "cJSON.h"

void platform_delegate_run_background(
    const char *tasks_dir, const char *task_id, const char *result_path, int json_output,
    agent_config_t *cfg, const char *role, const char *sys_prompt, const char *final_prompt,
    int max_tokens, int force_tools, const char *original_cwd, const char *delegate_git_root,
    const char *delegate_work_name, int keep_worktree, const char *launch_worktree_path,
    const char *launch_head, const char *parent_worktree_path, const char *parent_worktree_head,
    const char *parent_worktree_fingerprint, char *effective_prompt, char *file_prompt)
{
   (void)tasks_dir;

   /* Print task info first */
   if (json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "task_id", task_id);
      cJSON_AddStringToObject(obj, "result_path", result_path);
      cJSON_AddStringToObject(obj, "status", "running");
      if (launch_worktree_path && launch_worktree_path[0])
         cJSON_AddStringToObject(obj, "launch_worktree_path", launch_worktree_path);
      if (launch_head && launch_head[0])
         cJSON_AddStringToObject(obj, "launch_head", launch_head);
      if (parent_worktree_path && parent_worktree_path[0])
         cJSON_AddStringToObject(obj, "parent_worktree_path", parent_worktree_path);
      if (parent_worktree_head && parent_worktree_head[0])
         cJSON_AddStringToObject(obj, "parent_worktree_head", parent_worktree_head);
      if (parent_worktree_fingerprint && parent_worktree_fingerprint[0])
         cJSON_AddStringToObject(obj, "parent_worktree_fingerprint", parent_worktree_fingerprint);
      char *json = cJSON_Print(obj);
      if (json)
      {
         printf("%s\n", json);
         free(json);
      }
      cJSON_Delete(obj);
   }
   else
   {
      printf("task_id: %s\nresult: %s\n", task_id, result_path);
      if (launch_worktree_path && launch_worktree_path[0])
         printf("launch_worktree_path: %s\n", launch_worktree_path);
      if (launch_head && launch_head[0])
         printf("launch_head: %s\n", launch_head);
      if (parent_worktree_path && parent_worktree_path[0])
         printf("parent_worktree_path: %s\n", parent_worktree_path);
      if (parent_worktree_head && parent_worktree_head[0])
         printf("parent_worktree_head: %s\n", parent_worktree_head);
      if (parent_worktree_fingerprint && parent_worktree_fingerprint[0])
         printf("parent_worktree_fingerprint: %s\n", parent_worktree_fingerprint);
   }

   /* Run agent synchronously */
   agent_http_init();
   agent_result_t result;
   memset(&result, 0, sizeof(result));
   if (force_tools)
      agent_run_with_tools(cfg, role, sys_prompt, final_prompt, max_tokens, &result);
   else
      agent_run(cfg, role, sys_prompt, final_prompt, max_tokens, &result);
   agent_http_cleanup();
   write_result_json_with_checkout_ex(result_path, &result, launch_worktree_path, launch_head,
                                      parent_worktree_path, parent_worktree_head,
                                      parent_worktree_fingerprint);
   delegate_worktree_restore(original_cwd, delegate_git_root, delegate_work_name, keep_worktree);
   free(result.response);
   free(effective_prompt);
   free(file_prompt);
}

int platform_trace_run_task(agent_config_t *cfg, const char *role, const char *sys,
                            const char *prompt, int task_timeout, int use_tools,
                            const char *result_path, int task_idx)
{
   (void)task_idx;

   /* Windows: run synchronously in-process */
   agent_http_init();
   if (task_timeout > 0)
   {
      for (int ai = 0; ai < cfg->agent_count; ai++)
         cfg->agents[ai].timeout_ms = task_timeout;
   }
   agent_result_t result;
   memset(&result, 0, sizeof(result));
   if (use_tools)
      agent_run_with_tools(cfg, role, sys, prompt, 0, &result);
   else
      agent_run(cfg, role, sys, prompt, 0, &result);
   agent_http_cleanup();
   write_result_json(result_path, &result);
   free(result.response);
   return 0;
}
