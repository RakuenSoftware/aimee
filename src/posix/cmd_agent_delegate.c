/* cmd_agent_delegate.c: POSIX background dispatch (fork-based). */
#include "aimee.h"
#include "agent.h"
#include "agent_config.h"
#include "cmd_agent_delegate_impl.h"
#include "events.h"
#include "cJSON.h"
#include <sys/wait.h>
#include <unistd.h>

void platform_delegate_run_background(
    const char *tasks_dir, const char *task_id, const char *result_path, int json_output,
    agent_config_t *cfg, const char *role, const char *sys_prompt, const char *final_prompt,
    int max_tokens, int force_tools, const char *original_cwd, const char *delegate_git_root,
    const char *delegate_work_name, int keep_worktree, const char *launch_worktree_path,
    const char *launch_head, const char *parent_worktree_path, const char *parent_worktree_head,
    const char *parent_worktree_fingerprint, char *effective_prompt, char *file_prompt)
{
   pid_t pid = fork();
   if (pid < 0)
      fatal("fork failed");

   if (pid > 0)
   {
      /* Parent: write .pid file so delegate status can distinguish running from not_found */
      char pid_path[MAX_PATH_LEN];
      snprintf(pid_path, sizeof(pid_path), "%s/%s.pid", tasks_dir, task_id);
      FILE *pf = fopen(pid_path, "w");
      if (pf)
      {
         fprintf(pf, "%d\n", pid);
         fclose(pf);
      }
      /* Print task info and return */
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
            cJSON_AddStringToObject(obj, "parent_worktree_fingerprint",
                                    parent_worktree_fingerprint);
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
      return;
   }

   /* Child: detach, run agent, write result file */
   setsid();
   fclose(stdin);

   agent_http_init();
   agent_result_t result = {0};
   memset(&result, 0, sizeof(result));
   if (force_tools)
      agent_run_with_tools(cfg, role, sys_prompt, final_prompt, max_tokens, &result);
   else
      agent_run(cfg, role, sys_prompt, final_prompt, max_tokens, &result);
   agent_http_cleanup();
   write_result_json_with_checkout_ex(result_path, &result, launch_worktree_path, launch_head,
                                      parent_worktree_path, parent_worktree_head,
                                      parent_worktree_fingerprint);

   /* Fire completion event before exiting */
   if (result.success)
   {
      char notify_msg[256];
      snprintf(notify_msg, sizeof(notify_msg), "background delegate %s complete", role);
      event_notify(AIMEE_EVENT_DELEGATE_COMPLETE, notify_msg);
   }
   else
   {
      char notify_msg[256];
      snprintf(notify_msg, sizeof(notify_msg), "background delegate %s failed: %s", role,
               result.error);
      event_notify(AIMEE_EVENT_DELEGATE_FAILED, notify_msg);
   }

   /* Clean up .pid file now that result is written */
   char pid_path[MAX_PATH_LEN];
   snprintf(pid_path, sizeof(pid_path), "%s/%s.pid", tasks_dir, task_id);
   unlink(pid_path);

   delegate_worktree_restore(original_cwd, delegate_git_root, delegate_work_name, keep_worktree);
   agent_result_clear_response(&result);
   free(effective_prompt);
   free(file_prompt);
   _exit(0);
}

int platform_trace_run_task(agent_config_t *cfg, const char *role, const char *sys,
                            const char *prompt, int task_timeout, int use_tools,
                            const char *result_path, int task_idx)
{
   pid_t pid = fork();
   if (pid < 0)
   {
      fprintf(stderr, "aimee: fork failed for task %d\n", task_idx);
      return -1;
   }

   if (pid == 0)
   {
      /* Child: run agent and write result */
      setsid();
      fclose(stdin);
      agent_http_init();

      if (task_timeout > 0)
      {
         for (int ai = 0; ai < cfg->agent_count; ai++)
            cfg->agents[ai].timeout_ms = task_timeout;
      }

      agent_result_t result = {0};
      memset(&result, 0, sizeof(result));
      if (use_tools)
         agent_run_with_tools(cfg, role, sys, prompt, 0, &result);
      else
         agent_run(cfg, role, sys, prompt, 0, &result);
      agent_http_cleanup();
      write_result_json(result_path, &result);
      agent_result_clear_response(&result);
      _exit(0);
   }

   return 0; /* parent: success */
}
