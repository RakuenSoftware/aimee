/* cmd_agent_trace.c: dispatch, queue, context, manifest, trace, and jobs CLI commands */
#include "aimee.h"
#include "agent.h"
#include "platform_path.h"
#include "platform_process.h"
#include "commands.h"
#include "cmd_agent_delegate_impl.h"
#include "cJSON.h"
#include "db1.h"
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Defined in cmd_agent_delegate.c */
void generate_task_id(char *buf, size_t len);

/* --- cmd_dispatch (formerly cmd_queue) --- */

void cmd_dispatch(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   /* Run multiple tasks in parallel via agents */
   if (argc < 1)
      fatal("usage: aimee dispatch '<json tasks array>' [--background]\n"
            "  format: [{\"role\":\"...\",\"prompt\":\"...\"}]");

   static const char *bool_flags[] = {"background", "tools", NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);
   int background = opt_has(&opts, "background");
   int global_tools = opt_has(&opts, "tools");
   int global_timeout = opt_get_int(&opts, "timeout", 0);
   const char *tasks_arg = opt_pos(&opts, 0);

   if (!tasks_arg)
      fatal("usage: aimee queue '<json tasks array>'");

   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0 || cfg.agent_count == 0)
      fatal("no agents configured");

   cJSON *tasks_json = cJSON_Parse(tasks_arg);
   if (!tasks_json || !cJSON_IsArray(tasks_json))
      fatal("invalid JSON tasks array");

   int n = cJSON_GetArraySize(tasks_json);
   if (n <= 0 || n > 512)
      fatal("task count must be 1-512");

   if (background)
   {
      /* Fire-and-forget: fork one child per task, return task IDs immediately */
      char tasks_dir[MAX_PATH_LEN];
      snprintf(tasks_dir, sizeof(tasks_dir), "%s/tasks", config_default_dir());
      platform_mkdir_p(tasks_dir, 0700);

      cJSON *id_arr = cJSON_CreateArray();

      for (int i = 0; i < n; i++)
      {
         cJSON *t = cJSON_GetArrayItem(tasks_json, i);
         cJSON *r = cJSON_GetObjectItem(t, "role");
         cJSON *p = cJSON_GetObjectItem(t, "prompt");
         cJSON *s = cJSON_GetObjectItem(t, "system");
         cJSON *ft = cJSON_GetObjectItem(t, "tools");
         cJSON *to = cJSON_GetObjectItem(t, "timeout");
         const char *role = (r && cJSON_IsString(r)) ? r->valuestring : "execute";
         const char *prompt = (p && cJSON_IsString(p)) ? p->valuestring : "";
         const char *sys = (s && cJSON_IsString(s)) ? s->valuestring : NULL;
         int use_tools = global_tools || (ft && cJSON_IsTrue(ft));
         int task_timeout = (to && cJSON_IsNumber(to)) ? to->valueint : global_timeout;

         char task_id[64];
         generate_task_id(task_id, sizeof(task_id));
         char result_path[MAX_PATH_LEN];
         snprintf(result_path, sizeof(result_path), "%s/%s.json", tasks_dir, task_id);

         if (platform_trace_run_task(&cfg, role, sys, prompt, task_timeout, use_tools, result_path,
                                     i) != 0)
            continue;

         /* Parent: record task ID */
         cJSON *entry = cJSON_CreateObject();
         cJSON_AddStringToObject(entry, "task_id", task_id);
         cJSON_AddStringToObject(entry, "role", role);
         cJSON_AddItemToArray(id_arr, entry);
      }

      char *json = cJSON_Print(id_arr);
      if (json)
      {
         printf("%s\n", json);
         free(json);
      }
      cJSON_Delete(id_arr);
      cJSON_Delete(tasks_json);
      return;
   }

   /* Synchronous: run all tasks in parallel threads, wait for completion */
   agent_task_t *tasks = calloc((size_t)n, sizeof(agent_task_t));
   agent_result_t *results = calloc((size_t)n, sizeof(agent_result_t));
   if (!tasks || !results)
   {
      free(tasks);
      free(results);
      cJSON_Delete(tasks_json);
      fatal("memory allocation failed");
   }

   for (int i = 0; i < n; i++)
   {
      cJSON *t = cJSON_GetArrayItem(tasks_json, i);
      cJSON *r = cJSON_GetObjectItem(t, "role");
      cJSON *p = cJSON_GetObjectItem(t, "prompt");
      tasks[i].role = (r && cJSON_IsString(r)) ? r->valuestring : "execute";
      tasks[i].user_prompt = (p && cJSON_IsString(p)) ? p->valuestring : "";
      tasks[i].system_prompt = NULL;
      tasks[i].max_tokens = 0;
      tasks[i].temperature = 0.3;
   }

   agent_http_init();
   int successes = agent_run_parallel(&cfg, tasks, n, results, 0 /* no deadline */);
   agent_http_cleanup();

   /* Output results */
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = agent_result_to_json(&results[i]);
      cJSON_AddStringToObject(obj, "role", tasks[i].role);
      cJSON_AddItemToArray(arr, obj);
      free(results[i].response);
   }

   char *json = cJSON_Print(arr);
   if (json)
   {
      printf("%s\n", json);
      free(json);
   }
   cJSON_Delete(arr);
   cJSON_Delete(tasks_json);
   free(tasks);
   free(results);
   printf("%d/%d tasks succeeded.\n", successes, n);
}

/* --- cmd_queue (deprecated alias for cmd_dispatch) --- */

void cmd_queue(app_ctx_t *ctx, int argc, char **argv)
{
   fprintf(stderr, "aimee: 'queue' is deprecated, use 'dispatch' instead\n");
   cmd_dispatch(ctx, argc, argv);
}

/* --- cmd_context --- */

void cmd_context(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   (void)argc;
   (void)argv;

   /* Print the assembled execution context */
   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0 || cfg.agent_count == 0)
      fatal("no agents configured");
   agent_print_context(&cfg);
}

/* --- cmd_manifest --- */

void cmd_manifest(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   if (argc < 1)
      fatal("usage: aimee manifest list|show <id>");
   if (strcmp(argv[0], "list") == 0)
   {
      /* List manifests from config dir */
      char manifest_dir[MAX_PATH_LEN];
      snprintf(manifest_dir, sizeof(manifest_dir), "%s/manifests", config_default_dir());
      printf("Manifests in %s:\n", manifest_dir);
      DIR *d = opendir(manifest_dir);
      if (d)
      {
         struct dirent *ent;
         while ((ent = readdir(d)) != NULL)
         {
            if (ent->d_name[0] != '.')
               printf("  %s\n", ent->d_name);
         }
         closedir(d);
      }
   }
   else if (strcmp(argv[0], "show") == 0 && argc >= 2)
   {
      char path[MAX_PATH_LEN];
      snprintf(path, sizeof(path), "%s/manifests/%s.json", config_default_dir(), argv[1]);
      FILE *f = fopen(path, "r");
      if (!f)
         fatal("manifest not found: %s", argv[1]);
      char buf[4096];
      while (fgets(buf, sizeof(buf), f))
         fputs(buf, stdout);
      fclose(f);
   }
}

/* --- cmd_trace --- */

void cmd_trace(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   if (argc < 1)
      fatal("usage: aimee trace list|show <turn>");
   config_t db1_cfg;
   config_load(&db1_cfg);
   if (db1_init(db1_cfg.db1_path) != 0)
      fatal("trace: could not initialize DB1");

   if (strcmp(argv[0], "list") == 0)
   {
      db1_execution_trace_recent_row_t rows[50];
      int count = db1_execution_trace_list_recent(rows, 50);
      if (count > 0)
      {
         printf("%-6s %-5s %-12s %-16s %s\n", "ID", "Turn", "Direction", "Tool", "Time");
         for (int i = 0; i < count; i++)
            printf("%-6d %-5d %-12s %-16s %s\n", rows[i].id, rows[i].turn, rows[i].direction,
                   rows[i].tool_name[0] ? rows[i].tool_name : "--", rows[i].created_at);
      }
   }
   else if (strcmp(argv[0], "show") == 0 && argc >= 2)
   {
      int trace_id = atoi(argv[1]);
      db1_execution_trace_detail_t row;
      if (db1_execution_trace_get(trace_id, &row) == 0)
      {
         printf("Turn: %d\n", row.turn);
         printf("Direction: %s\n", row.direction);
         if (row.content[0])
            printf("Content:\n%.*s\n", 2000, row.content);
         if (row.tool_name[0])
            printf("Tool: %s\n", row.tool_name);
         if (row.tool_args[0])
            printf("Args: %s\n", row.tool_args);
         if (row.tool_result[0])
            printf("Result:\n%.*s\n", 2000, row.tool_result);
      }
      else
      {
         printf("Trace entry %d not found.\n", trace_id);
      }
   }
}

/* --- cmd_jobs --- */

void cmd_jobs(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("usage: aimee jobs list|status|show <id>|logs <id>|cancel <id>");

   if (strcmp(argv[0], "list") == 0)
   {
      config_t db1_cfg;
      config_load(&db1_cfg);
      if (db1_init(db1_cfg.db1_path) != 0)
         fatal("jobs list: could not initialize DB1");
      db1_agent_job_t jobs[20];
      int n = db1_agent_job_list_recent(jobs, 20, 0); /* list view omits prompt/result */
      printf("%-6s %-12s %-10s %-6s %-12s %s\n", "ID", "Role", "Status", "Turn", "Agent",
             "Created");
      for (int i = 0; i < n; i++)
      {
         char turn[16];
         if (jobs[i].cursor_turn > 0)
            snprintf(turn, sizeof(turn), "%d", jobs[i].cursor_turn);
         else
            snprintf(turn, sizeof(turn), "--");
         printf("%-6d %-12s %-10s %-6s %-12s %s\n", jobs[i].id, jobs[i].role, jobs[i].status, turn,
                jobs[i].agent_name, jobs[i].created_at);
         db1_agent_job_free(&jobs[i]);
      }
   }
   else if ((strcmp(argv[0], "status") == 0 || strcmp(argv[0], "show") == 0) && argc >= 2)
   {
      config_t db1_cfg;
      config_load(&db1_cfg);
      if (db1_init(db1_cfg.db1_path) != 0)
         fatal("jobs status: could not initialize DB1");
      int jid = atoi(argv[1]);
      db1_agent_job_t job;
      if (db1_agent_job_get(jid, &job) == 0)
      {
         printf("Job #%d\n", job.id);
         printf("Role:      %s\n", job.role);
         printf("Prompt:    %.*s\n", 200, job.prompt);
         printf("Status:    %s\n", job.status);
         if (job.cursor_turn > 0)
            printf("Cursor:    turn %d\n", job.cursor_turn);
         printf("Agent:     %s\n", job.agent_name);
         if (job.result[0])
            printf("Result:    %.*s\n", 500, job.result);
         if (job.heartbeat_at[0])
            printf("Heartbeat: %s\n", job.heartbeat_at);
         printf("Created:   %s\n", job.created_at);
         printf("Updated:   %s\n", job.updated_at);
         db1_agent_job_free(&job);
      }
      else
      {
         printf("Job %d not found.\n", jid);
      }
   }
   else if (strcmp(argv[0], "cancel") == 0 && argc >= 2)
   {
      config_t db1_cfg;
      config_load(&db1_cfg);
      if (db1_init(db1_cfg.db1_path) != 0)
         fatal("jobs cancel: could not initialize DB1");
      int jid = atoi(argv[1]);
      db1_agent_job_update(jid, "cancelled", 0, NULL);
      printf("Job %d cancelled.\n", jid);
   }
   else if (strcmp(argv[0], "resume") == 0 && argc >= 2)
   {
      int jid = atoi(argv[1]);
      agent_config_t acfg;
      if (agent_load_config(&acfg) != 0 || acfg.agent_count == 0)
         fatal("no agents configured");

      agent_http_init();
      agent_result_t result;
      memset(&result, 0, sizeof(result));
      int rc = agent_job_resume(&acfg, jid, &result);
      agent_http_cleanup();

      if (ctx->json_output)
      {
         cJSON *obj = agent_result_to_json(&result);
         char *json = cJSON_Print(obj);
         if (json)
         {
            printf("%s\n", json);
            free(json);
         }
         cJSON_Delete(obj);
      }
      else if (rc == 0 && result.response)
      {
         printf("Job %d resumed and completed.\n%s\n", jid, result.response);
      }
      else
      {
         fprintf(stderr, "Job %d resume failed: %s\n", jid, result.error);
      }
      free(result.response);
   }
   else if (strcmp(argv[0], "wait") == 0 && argc >= 2)
   {
      const char *task_id = argv[1];
      int timeout_sec = 120;
      for (int i = 2; i < argc; i++)
      {
         if (strncmp(argv[i], "--timeout=", 10) == 0)
            timeout_sec = atoi(argv[i] + 10);
      }

      char path[MAX_PATH_LEN];
      snprintf(path, sizeof(path), "%s/tasks/%s.json", config_default_dir(), task_id);

      int elapsed = 0;
      int done = 0;
      while (elapsed < timeout_sec)
      {
         FILE *f = fopen(path, "r");
         if (f)
         {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            if (sz > 2)
            {
               fseek(f, 0, SEEK_SET);
               char *buf = malloc((size_t)sz + 1);
               if (buf)
               {
                  size_t nread = fread(buf, 1, (size_t)sz, f);
                  buf[nread] = '\0';
                  fclose(f);
                  printf("%s\n", buf);
                  free(buf);
                  done = 1;
                  break;
               }
            }
            fclose(f);
         }
         usleep(500000);
         elapsed++;
      }
      if (!done)
      {
         fprintf(stderr, "timeout waiting for %s (%ds)\n", task_id, timeout_sec);
         exit(1);
      }
   }
   else if (strcmp(argv[0], "collect") == 0 && argc >= 2)
   {
      /* Collect results from multiple background tasks.
       * Usage: aimee jobs collect <id1> [id2] [id3] ... [--timeout=N]
       * Waits for all tasks to complete, returns JSON array of results. */
      int timeout_sec = 120;
      int task_count = 0;
      const char *task_ids[512];

      for (int i = 1; i < argc; i++)
      {
         if (strncmp(argv[i], "--timeout=", 10) == 0)
            timeout_sec = atoi(argv[i] + 10);
         else if (task_count < 512)
            task_ids[task_count++] = argv[i];
      }

      if (task_count == 0)
         fatal("usage: aimee jobs collect <id1> [id2...] [--timeout=N]");

      /* Poll all tasks until all complete or timeout */
      char *task_results[512];
      int task_done[512];
      memset(task_results, 0, sizeof(task_results));
      memset(task_done, 0, sizeof(task_done));

      int all_done = 0;
      int elapsed = 0;
      while (!all_done && elapsed < timeout_sec)
      {
         all_done = 1;
         for (int i = 0; i < task_count; i++)
         {
            if (task_done[i])
               continue;

            char path[MAX_PATH_LEN];
            snprintf(path, sizeof(path), "%s/tasks/%s.json", config_default_dir(), task_ids[i]);

            FILE *f = fopen(path, "r");
            if (f)
            {
               fseek(f, 0, SEEK_END);
               long sz = ftell(f);
               if (sz > 2)
               {
                  fseek(f, 0, SEEK_SET);
                  char *buf = malloc((size_t)sz + 1);
                  if (buf)
                  {
                     size_t nread = fread(buf, 1, (size_t)sz, f);
                     buf[nread] = '\0';
                     task_results[i] = buf;
                     task_done[i] = 1;
                  }
               }
               fclose(f);
            }

            if (!task_done[i])
               all_done = 0;
         }

         if (!all_done)
         {
            usleep(500000);
            elapsed++;
         }
      }

      /* Build JSON array of results */
      cJSON *arr = cJSON_CreateArray();
      int success_count = 0;
      for (int i = 0; i < task_count; i++)
      {
         cJSON *entry = cJSON_CreateObject();
         cJSON_AddStringToObject(entry, "task_id", task_ids[i]);

         if (task_results[i])
         {
            cJSON *result = cJSON_Parse(task_results[i]);
            if (result)
            {
               cJSON_AddItemToObject(entry, "result", result);
               cJSON_AddStringToObject(entry, "status", "done");
               success_count++;
            }
            else
            {
               cJSON_AddStringToObject(entry, "status", "error");
               cJSON_AddStringToObject(entry, "error", "failed to parse result");
            }
            free(task_results[i]);
         }
         else
         {
            cJSON_AddStringToObject(entry, "status", "timeout");
         }
         cJSON_AddItemToArray(arr, entry);
      }

      char *json = cJSON_Print(arr);
      if (json)
      {
         printf("%s\n", json);
         free(json);
      }
      cJSON_Delete(arr);

      if (success_count < task_count)
      {
         fprintf(stderr, "%d/%d tasks completed (%d timed out)\n", success_count, task_count,
                 task_count - success_count);
      }
   }
}
