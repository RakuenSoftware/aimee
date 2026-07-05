/* ===================================================================
 * /v1 thin-client routing: route CLI subcommands through the server's native /v1 HTTP endpoints.
 * Unported commands fail in cli_main before reaching the server.
 * =================================================================== */

#include "cli_v1_routes_internal.h"
#include "platform_path.h"
#include "cli_client.h"
#define V1_PROTOCOL_VERSION 1
#include "util.h"         /* safe_exec_capture (workspace.mirror-sync ships the client diff) */
#include "aimee_client.h" /* aimee_client_request: transport-agnostic /v1 client (Windows path) */
#include "code_collect.h" /* code_collect_files + code_collect_discover_repos (thin-client push) */
#if !defined(_WIN32) && !defined(_WIN64)
#include "aimee_home.h"
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif /* !_WIN32 (preamble guard) */

static void print_worktree_gc(cJSON *resp)
{
   if (!resp)
      return;
   const char *git_root = json_str(resp, "git_root");
   int dry_run = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "dry_run"));
   int force = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "force"));
   cJSON *jdays = cJSON_GetObjectItemCaseSensitive(resp, "max_age_days");
   int days = cJSON_IsNumber(jdays) ? (int)jdays->valuedouble : 0;
   cJSON *jscan = cJSON_GetObjectItemCaseSensitive(resp, "scanned");
   int scanned = cJSON_IsNumber(jscan) ? (int)jscan->valuedouble : 0;
   cJSON *jrem = cJSON_GetObjectItemCaseSensitive(resp, "removed");
   int removed = cJSON_IsNumber(jrem) ? (int)jrem->valuedouble : 0;

   printf("Worktree GC — %s\n", git_root && git_root[0] ? git_root : "(unknown)");
   printf("  threshold: idle %dd%s%s\n", days, force ? ", --force" : "",
          dry_run ? ", --dry-run" : "");
   printf("  scanned:   %d worktree(s)\n", scanned);
   printf("  %s:   %d\n", dry_run ? "would remove" : "removed", removed);

   cJSON *cands = cJSON_GetObjectItemCaseSensitive(resp, "candidates");
   if (!cJSON_IsArray(cands) || cJSON_GetArraySize(cands) == 0)
   {
      printf("\n  No candidates.\n");
      return;
   }
   printf("\n  Candidates:\n");
   cJSON *c;
   cJSON_ArrayForEach(c, cands)
   {
      const char *path = json_str(c, "path");
      const char *branch = json_str(c, "branch");
      const char *reason = json_str(c, "reason");
      int eligible = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "eligible"));
      printf("    [%s] %s\n", eligible ? "x" : " ", path && path[0] ? path : "(unknown)");
      if (branch && branch[0])
         printf("        branch: %s\n", branch);
      if (reason && reason[0])
         printf("        %s\n", reason);
   }
}

static void print_work_item_line(cJSON *item)
{
   const char *id = json_str(item, "id");
   const char *title = json_str(item, "title");
   const char *source = json_str(item, "source");
   const char *status = json_str(item, "status");
   const char *claimed = json_str(item, "claimed_by");
   const char *claimed_at = json_str(item, "claimed_at");
   const char *result = json_str(item, "result");

   printf("[%s] %-10s %s", id[0] ? id : "?", status[0] ? status : "?", title[0] ? title : "?");
   if (claimed[0])
      printf("  (session: %.8s)", claimed);
   if (claimed_at[0] && status[0] && strcmp(status, "claimed") == 0)
      printf("  (%s)", claimed_at);
   if (source[0])
      printf("  [%s]", source);
   if (result[0])
      printf("  -> %s", result);
   printf("\n");
}

static void print_work_list(cJSON *resp)
{
   cJSON *items = cJSON_GetObjectItemCaseSensitive(resp, "items");
   if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) == 0)
   {
      printf("No work items found.\n");
      return;
   }

   cJSON *item;
   cJSON_ArrayForEach(item, items) print_work_item_line(item);
}

static void format_work_board_age(cJSON *item, char *buf, size_t cap)
{
   if (!buf || cap == 0)
      return;
   buf[0] = '\0';

   cJSON *age = cJSON_GetObjectItemCaseSensitive(item, "age_minutes");
   if (!cJSON_IsNumber(age))
      return;

   int age_min = (int)age->valuedouble;
   if (age_min < 60)
      snprintf(buf, cap, "%dm", age_min);
   else if (age_min < 24 * 60)
      snprintf(buf, cap, "%dh%dm", age_min / 60, age_min % 60);
   else
      snprintf(buf, cap, "%dd%dh", age_min / (24 * 60), (age_min / 60) % 24);
}

static void print_work_board(cJSON *resp)
{
   static const char *statuses[] = {"pending", "claimed", "done", "failed", "cancelled"};
   cJSON *board = cJSON_GetObjectItemCaseSensitive(resp, "board");
   if (!cJSON_IsObject(board))
   {
      printf("No work items found.\n");
      return;
   }

   printf("Work board\n");
   int total = 0;
   for (int s = 0; s < 5; s++)
   {
      cJSON *items = cJSON_GetObjectItemCaseSensitive(board, statuses[s]);
      int count = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
      total += count;
      printf("\n%s (%d)\n", statuses[s], count);
      if (count == 0)
      {
         printf("  (empty)\n");
         continue;
      }

      cJSON *item;
      cJSON_ArrayForEach(item, items)
      {
         char age[32];
         format_work_board_age(item, age, sizeof(age));
         printf("  [%s] %s", json_str(item, "id"), json_str(item, "title"));
         if (age[0])
            printf("  age:%s", age);
         if (json_str(item, "claimed_by")[0])
            printf("  owner:%.8s", json_str(item, "claimed_by"));
         if (json_str(item, "source")[0])
            printf("  source:%s", json_str(item, "source"));
         if (json_str(item, "result")[0])
            printf("  result:%s", json_str(item, "result"));
         printf("\n");
      }
   }

   if (total == 0)
      printf("\nNo work items found.\n");
}

static void print_work_claim(cJSON *resp)
{
   if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "claimed")))
   {
      const char *msg = json_str(resp, "message");
      printf("%s\n", msg[0] ? msg : "No pending work items to claim.");
      return;
   }

   printf("Claimed: [%s] %s\n", json_str(resp, "id"), json_str(resp, "title"));
   const char *source = json_str(resp, "source");
   const char *desc = json_str(resp, "description");
   if (source[0])
      printf("  Source: %s\n", source);
   if (desc[0])
      printf("  Description: %s\n", desc);
}

static void print_work_finish(cJSON *resp, const char *status_name)
{
   if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "updated")))
   {
      const char *msg = json_str(resp, "message");
      printf("%s\n", msg[0] ? msg : "No matching claimed work item found for this session.");
      return;
   }

   printf("Marked %s: [%s] %s\n", status_name, json_str(resp, "id"), json_str(resp, "title"));
}

static void print_work_simple_item(cJSON *resp, const char *field, const char *verb,
                                   const char *missing)
{
   if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, field)))
   {
      const char *msg = json_str(resp, "message");
      printf("%s\n", msg[0] ? msg : missing);
      return;
   }
   printf("%s: [%s] %s\n", verb, json_str(resp, "id"), json_str(resp, "title"));
}

static void print_work_stats(cJSON *resp)
{
   int total = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "total"));
   int pending = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "pending"));
   int claimed = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "claimed"));
   int done = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "done"));
   int failed = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "failed"));
   int cancelled = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "cancelled"));
   int timed =
       (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "completed_with_timing"));
   double avg =
       cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "avg_completion_minutes"));

   printf("Queue stats:\n");
   printf("  Total items: %d\n", total);
   printf("  Pending:     %d\n", pending);
   printf("  Claimed:     %d\n", claimed);
   printf("  Completed:   %d", done);
   if (timed > 0)
      printf(" (avg %.0fm)", avg);
   printf("\n");
   printf("  Failed:      %d\n", failed);
   printf("  Cancelled:   %d\n", cancelled);
}

static void print_work_gc(cJSON *resp)
{
   cJSON *released_items = cJSON_GetObjectItemCaseSensitive(resp, "released_items");
   if (cJSON_IsArray(released_items))
   {
      cJSON *item;
      cJSON_ArrayForEach(item, released_items)
          printf("Released stale claim: [%s] %s\n", json_str(item, "id"), json_str(item, "title"));
   }

   int released = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "released"));
   int closed = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "closed"));
   int cancelled = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "cancelled"));
   int hours = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "max_age_hours"));

   if (released == 0 && closed == 0 && cancelled == 0)
      printf("No stale claims found (threshold: %d hours).\n", hours);
   else
   {
      if (released > 0)
         printf("Released %d stale claims.\n", released);
      if (closed > 0 || cancelled > 0)
         printf("Synced proposals: %d closed as done, %d cancelled.\n", closed, cancelled);
   }
}

static void print_work_sync_proposals(cJSON *resp)
{
   int closed = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "closed"));
   int cancelled = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "cancelled"));
   if (closed == 0 && cancelled == 0)
      printf("No work items to sync - all sources are still in pending/.\n");
   else
      printf("Synced work queue: %d closed as done, %d cancelled.\n", closed, cancelled);
}

static void print_session_close(cJSON *resp)
{
   cJSON *sid = cJSON_GetObjectItemCaseSensitive(resp, "session_id");
   if (cJSON_IsString(sid) && sid->valuestring[0])
      printf("closed session %s\n", sid->valuestring);
   else
      printf("closed session\n");
}

static void print_rules_delete(cJSON *resp)
{
   cJSON *id = cJSON_GetObjectItemCaseSensitive(resp, "id");
   if (cJSON_IsNumber(id))
      printf("deleted rule %d\n", (int)id->valuedouble);
   else
      printf("deleted rule\n");
}

static void print_agent_list(cJSON *resp)
{
   cJSON *agents = cJSON_GetObjectItemCaseSensitive(resp, "agents");
   if (!cJSON_IsArray(agents) || cJSON_GetArraySize(agents) == 0)
   {
      printf("No agents configured.\n");
      return;
   }
   cJSON *ag;
   cJSON_ArrayForEach(ag, agents)
   {
      cJSON *name = cJSON_GetObjectItemCaseSensitive(ag, "name");
      cJSON *enabled = cJSON_GetObjectItemCaseSensitive(ag, "enabled");
      cJSON *tier = cJSON_GetObjectItemCaseSensitive(ag, "cost_tier");
      cJSON *parallel = cJSON_GetObjectItemCaseSensitive(ag, "max_parallel");
      cJSON *model = cJSON_GetObjectItemCaseSensitive(ag, "model");
      cJSON *endpoint = cJSON_GetObjectItemCaseSensitive(ag, "endpoint");
      cJSON *tools = cJSON_GetObjectItemCaseSensitive(ag, "tools_enabled");
      printf("%-16s %-6s tier=%d parallel=%d model=%s endpoint=%s%s\n",
             cJSON_IsString(name) ? name->valuestring : "?", cJSON_IsTrue(enabled) ? "ON" : "OFF",
             cJSON_IsNumber(tier) ? tier->valueint : 0,
             cJSON_IsNumber(parallel) ? parallel->valueint : 0,
             cJSON_IsString(model) ? model->valuestring : "",
             cJSON_IsString(endpoint) ? endpoint->valuestring : "",
             cJSON_IsTrue(tools) ? " [tools]" : "");
   }
}

static void print_agent_local(cJSON *resp)
{
   printf("Local delegate '%s' registered: %s @ %s\n", json_str(resp, "name"),
          json_str(resp, "model"), json_str(resp, "endpoint"));
   cJSON *parallel = cJSON_GetObjectItemCaseSensitive(resp, "max_parallel");
   cJSON *ctx = cJSON_GetObjectItemCaseSensitive(resp, "context_window");
   cJSON *tools = cJSON_GetObjectItemCaseSensitive(resp, "tools_enabled");
   printf("  max_parallel=%d", cJSON_IsNumber(parallel) ? parallel->valueint : 0);
   if (cJSON_IsNumber(ctx))
      printf(" context_window=%d", ctx->valueint);
   printf(" tools=%s\n", cJSON_IsTrue(tools) ? "on" : "off");
   cJSON *mp = cJSON_GetObjectItemCaseSensitive(resp, "model_probe");
   cJSON *sp = cJSON_GetObjectItemCaseSensitive(resp, "slot_probe");
   if (cJSON_IsString(mp) && mp->valuestring[0])
      printf("  warning: %s\n", mp->valuestring);
   if (cJSON_IsString(sp) && sp->valuestring[0])
      printf("  warning: %s\n", sp->valuestring);
}

static void print_agent_add(cJSON *resp)
{
   printf("Delegate '%s' saved: %s @ %s\n", json_str(resp, "name"), json_str(resp, "model"),
          json_str(resp, "endpoint"));
}

static void print_agent_remove(cJSON *resp)
{
   printf("Delegate '%s' removed.\n", json_str(resp, "name"));
}

static void print_agent_enabled(cJSON *resp, int enabled)
{
   printf("Delegate '%s' %s.\n", json_str(resp, "name"), enabled ? "enabled" : "disabled");
}

static void print_agent_probe(cJSON *resp)
{
   printf("%s\n", json_str(resp, "name"));
   cJSON *models_status = cJSON_GetObjectItemCaseSensitive(resp, "models_status");
   cJSON *model_available = cJSON_GetObjectItemCaseSensitive(resp, "model_available");
   cJSON *slots_status = cJSON_GetObjectItemCaseSensitive(resp, "slots_status");
   cJSON *slots = cJSON_GetObjectItemCaseSensitive(resp, "detected_slots");
   cJSON *ctx = cJSON_GetObjectItemCaseSensitive(resp, "detected_context_window");
   cJSON *slots_source = cJSON_GetObjectItemCaseSensitive(resp, "slots_source");
   printf("  models: %s (%d)\n", cJSON_IsTrue(model_available) ? "ok" : "warn",
          cJSON_IsNumber(models_status) ? models_status->valueint : 0);
   cJSON *mp = cJSON_GetObjectItemCaseSensitive(resp, "model_probe");
   if (cJSON_IsString(mp) && mp->valuestring[0])
      printf("  model_probe: %s\n", mp->valuestring);
   printf("  slots: %d", cJSON_IsNumber(slots) ? slots->valueint : 0);
   if (cJSON_IsNumber(ctx))
      printf(" context_window=%d", ctx->valueint);
   if (cJSON_IsString(slots_source) && strcmp(slots_source->valuestring, "config") == 0)
      printf(" (configured)\n");
   else
      printf(" (%d)\n", cJSON_IsNumber(slots_status) ? slots_status->valueint : 0);
   cJSON *sp = cJSON_GetObjectItemCaseSensitive(resp, "slot_probe");
   if (cJSON_IsString(sp) && sp->valuestring[0])
      printf("  slot_probe: %s\n", sp->valuestring);
   cJSON *run = cJSON_GetObjectItemCaseSensitive(resp, "execution_ok");
   if (run)
   {
      cJSON *lat = cJSON_GetObjectItemCaseSensitive(resp, "latency_ms");
      printf("  execution: %s latency=%dms %s\n", cJSON_IsTrue(run) ? "ok" : "failed",
             cJSON_IsNumber(lat) ? lat->valueint : 0, json_str(resp, "execution_message"));
   }
}

static void print_trigger_list(cJSON *resp)
{
   cJSON *triggers = cJSON_GetObjectItemCaseSensitive(resp, "triggers");
   if (!cJSON_IsArray(triggers) || cJSON_GetArraySize(triggers) == 0)
   {
      printf("No trigger runs found.\n");
      return;
   }

   printf("%-16s %-16s %-10s %-40s %s\n", "ID", "SOURCE", "STATUS", "TASK", "QUEUED_AT");
   printf("%-16s %-16s %-10s %-40s %s\n", "--", "------", "------", "----", "---------");
   cJSON *t;
   cJSON_ArrayForEach(t, triggers)
   {
      const char *task = json_str(t, "task");
      char task_buf[41];
      if (strlen(task) > 40)
      {
         memcpy(task_buf, task, 37);
         task_buf[37] = '.';
         task_buf[38] = '.';
         task_buf[39] = '.';
         task_buf[40] = '\0';
         task = task_buf;
      }
      printf("%-16s %-16s %-10s %-40s %s\n", json_str(t, "id"), json_str(t, "source"),
             json_str(t, "status"), task, json_str(t, "queued_at"));
   }
}

static cJSON *trigger_object(cJSON *resp)
{
   cJSON *trigger = cJSON_GetObjectItemCaseSensitive(resp, "trigger");
   return cJSON_IsObject(trigger) ? trigger : resp;
}

static void print_trigger_status(cJSON *resp)
{
   cJSON *t = trigger_object(resp);
   printf("id:          %s\n", json_str(t, "id"));
   printf("source:      %s\n", json_str(t, "source"));
   printf("event:       %s\n", json_str(t, "event"));
   printf("status:      %s\n", json_str(t, "status"));
   printf("task:        %s\n", json_str(t, "task"));
   printf("workspace:   %s\n", json_str(t, "workspace"));
   printf("pipeline_id: %s\n", json_str(t, "pipeline_id"));
   printf("queued_at:   %s\n", json_str(t, "queued_at"));
   printf("started_at:  %s\n", json_str(t, "started_at"));
   printf("finished_at: %s\n", json_str(t, "finished_at"));
   if (json_str(t, "error")[0])
      printf("error:       %s\n", json_str(t, "error"));
}

static void print_trigger_fire(cJSON *resp)
{
   printf("trigger_id: %s\n", json_str(resp, "trigger_id"));
   if (json_str(resp, "pipeline_id")[0])
      printf("pipeline_id: %s\n", json_str(resp, "pipeline_id"));
   printf("status: %s\n", json_str(resp, "trigger_status")[0] ? json_str(resp, "trigger_status")
                                                              : json_str(resp, "status"));
}

static void print_trigger_cancel(cJSON *resp)
{
   const char *id = json_str(resp, "trigger_id");
   if (id[0])
      printf("cancelled trigger %s\n", id);
   else
      printf("cancelled trigger\n");
}

static void print_cron_list(cJSON *resp)
{
   cJSON *jobs = cJSON_GetObjectItemCaseSensitive(resp, "jobs");
   if (!cJSON_IsArray(jobs) || cJSON_GetArraySize(jobs) == 0)
   {
      printf("No cron jobs configured.\n");
      return;
   }

   printf("%-24s %-12s %-14s %-8s %-18s %s\n", "ID", "MODE", "SCHEDULE", "ENABLED", "LAST_STATUS",
          "TARGET");
   cJSON *job;
   cJSON_ArrayForEach(job, jobs)
   {
      cJSON *enabled = cJSON_GetObjectItemCaseSensitive(job, "enabled");
      printf("%-24s %-12s %-14s %-8s %-18s %s\n", json_str(job, "id"), json_str(job, "mode"),
             json_str(job, "schedule"), cJSON_IsTrue(enabled) ? "yes" : "no",
             json_str(job, "last_run_status"), json_str(job, "deliver_target"));
   }
}

static void print_cron_show(cJSON *resp)
{
   cJSON *job = cJSON_GetObjectItemCaseSensitive(resp, "job");
   if (!cJSON_IsObject(job))
      job = resp;
   printf("id:                    %s\n", json_str(job, "id"));
   printf("schedule:              %s\n", json_str(job, "schedule"));
   printf("mode:                  %s\n", json_str(job, "mode"));
   printf("enabled:               %s\n",
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(job, "enabled")) ? "true" : "false");
   printf("workdir:               %s\n", json_str(job, "workdir"));
   printf("deliver_target:        %s\n", json_str(job, "deliver_target"));
   printf("skills:                %s\n", json_str(job, "skills_csv"));
   printf("context_from:          %s\n", json_str(job, "context_from"));
   printf("when_context_contains: %s\n", json_str(job, "when_context_contains"));
}

static void print_cron_history(cJSON *resp)
{
   cJSON *runs = cJSON_GetObjectItemCaseSensitive(resp, "runs");
   if (!cJSON_IsArray(runs) || cJSON_GetArraySize(runs) == 0)
   {
      printf("No cron job runs found.\n");
      return;
   }

   printf("%-6s %-24s %-10s %-7s %-9s %s\n", "ID", "JOB", "STATUS", "SILENT", "DELIVERED",
          "COMPLETED");
   cJSON *run;
   cJSON_ArrayForEach(run, runs)
   {
      printf("%-6d %-24s %-10s %-7s %-9s %d\n", json_int(run, "id", 0), json_str(run, "job_id"),
             json_str(run, "status"),
             cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(run, "silent")) ? "yes" : "no",
             cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(run, "delivered")) ? "yes" : "no",
             json_int(run, "completed_at", 0));
   }
}

static void print_cron_run(cJSON *resp)
{
   printf("job_id: %s\n", json_str(resp, "job_id"));
   printf("run_id: %d\n", json_int(resp, "run_id", 0));
   printf("status: %s\n", json_str(resp, "run_status"));
   printf("silent: %s\n",
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "silent")) ? "true" : "false");
   if (json_str(resp, "trigger_id")[0])
      printf("trigger_id: %s\n", json_str(resp, "trigger_id"));
   if (json_str(resp, "pipeline_id")[0])
      printf("pipeline_id: %s\n", json_str(resp, "pipeline_id"));
   if (json_str(resp, "error")[0])
      printf("error: %s\n", json_str(resp, "error"));
}

static void print_memory_row(cJSON *m)
{
   if (!cJSON_IsObject(m))
      return;
   cJSON *id = cJSON_GetObjectItemCaseSensitive(m, "id");
   const char *tier = json_str(m, "tier");
   const char *kind = json_str(m, "kind");
   const char *key = json_str(m, "key");
   const char *content = json_str(m, "content");
   printf("%lld  %-3s %-12s %s", cJSON_IsNumber(id) ? (long long)id->valuedouble : 0, tier, kind,
          key[0] ? key : "(no key)");
   if (content[0])
      printf(": %s", content);
   putchar('\n');
}

static void print_memory_search(cJSON *resp)
{
   cJSON *facts = cJSON_GetObjectItemCaseSensitive(resp, "facts");
   cJSON *windows = cJSON_GetObjectItemCaseSensitive(resp, "windows");
   int fact_count = cJSON_IsArray(facts) ? cJSON_GetArraySize(facts) : 0;
   int window_count = cJSON_IsArray(windows) ? cJSON_GetArraySize(windows) : 0;
   if (fact_count == 0 && window_count == 0)
   {
      printf("No memories found.\n");
      return;
   }
   if (fact_count > 0)
   {
      printf("Facts:\n");
      cJSON *m;
      cJSON_ArrayForEach(m, facts)
      {
         printf("  ");
         print_memory_row(m);
      }
   }
   if (window_count > 0)
   {
      printf("Conversation windows:\n");
      cJSON *w;
      cJSON_ArrayForEach(w, windows)
      {
         cJSON *score = cJSON_GetObjectItemCaseSensitive(w, "score");
         printf("  %.4f  %s:%lld  %s\n", cJSON_IsNumber(score) ? score->valuedouble : 0.0,
                json_str(w, "session_id"),
                (long long)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(w, "seq")),
                json_str(w, "summary"));
      }
   }
}

static void print_memory_list(cJSON *resp)
{
   cJSON *memories = cJSON_GetObjectItemCaseSensitive(resp, "memories");
   if (!cJSON_IsArray(memories) || cJSON_GetArraySize(memories) == 0)
   {
      printf("No memories.\n");
      return;
   }
   cJSON *m;
   cJSON_ArrayForEach(m, memories)
   {
      print_memory_row(m);
   }
}

static void print_memory_stats(cJSON *resp)
{
   cJSON *stats = cJSON_GetObjectItemCaseSensitive(resp, "stats");
   if (!cJSON_IsObject(stats))
   {
      printf("No memory statistics available.\n");
      return;
   }
   cJSON *total = cJSON_GetObjectItemCaseSensitive(stats, "total");
   cJSON *conflicts = cJSON_GetObjectItemCaseSensitive(stats, "conflicts");
   printf("Memory statistics:\n");
   printf("  total:     %lld\n", (long long)(cJSON_IsNumber(total) ? total->valuedouble : 0));
   printf("  conflicts: %lld\n",
          (long long)(cJSON_IsNumber(conflicts) ? conflicts->valuedouble : 0));
   cJSON *tiers = cJSON_GetObjectItemCaseSensitive(stats, "tier_counts");
   if (cJSON_IsArray(tiers) && cJSON_GetArraySize(tiers) > 0)
   {
      printf("  tier counts:");
      cJSON *t;
      cJSON_ArrayForEach(t, tiers)
          printf(" %lld", (long long)(cJSON_IsNumber(t) ? t->valuedouble : 0));
      printf("\n");
   }
   cJSON *kinds = cJSON_GetObjectItemCaseSensitive(stats, "kind_counts");
   if (cJSON_IsArray(kinds) && cJSON_GetArraySize(kinds) > 0)
   {
      printf("  kind counts:");
      cJSON *k;
      cJSON_ArrayForEach(k, kinds)
          printf(" %lld", (long long)(cJSON_IsNumber(k) ? k->valuedouble : 0));
      printf("\n");
   }
}

static void print_index_scan(cJSON *resp)
{
   /* An error response (e.g. KB unavailable) carries no projects/files counts;
    * surface the message instead of formatting absent numbers, which would
    * otherwise print INT_MIN via (int)cJSON_GetNumberValue(NULL) == (int)NaN. */
   if (strcmp(json_str(resp, "status"), "error") == 0)
   {
      const char *msg = json_str(resp, "message");
      printf("==> Scan failed: %s\n", msg[0] ? msg : "knowledge service unavailable");
      return;
   }

   cJSON *skipped = cJSON_GetObjectItemCaseSensitive(resp, "skipped");
   if (cJSON_IsTrue(skipped))
   {
      const char *reason = json_str(resp, "reason");
      cJSON *retry = cJSON_GetObjectItemCaseSensitive(resp, "retry_after");
      if (strcmp(reason, "cooldown") == 0 && cJSON_IsNumber(retry))
         printf("==> Scan skipped: cooldown active (%lds remaining)\n", (long)retry->valuedouble);
      else if (strcmp(reason, "busy") == 0)
         printf("==> Scan skipped: another scan is already running\n");
      else
         printf("==> Scan skipped: %s\n", reason[0] ? reason : "no reason given");
      return;
   }

   int projects = json_int(resp, "projects", 0);
   int files = json_int(resp, "files", 0);
   int inspected = json_int(resp, "inspected", 0);
   int unchanged = inspected - files;
   if (inspected > 0 && unchanged > 0)
      printf("==> Scan complete: %d project(s), %d file(s) re-indexed (%d unchanged)\n", projects,
             files, unchanged);
   else
      printf("==> Scan complete: %d project(s), %d file(s) re-indexed\n", projects, files);
}

static void print_index_list(cJSON *resp)
{
   cJSON *projects = cJSON_GetObjectItemCaseSensitive(resp, "projects");
   if (!cJSON_IsArray(projects) || cJSON_GetArraySize(projects) == 0)
   {
      printf("No indexed projects.\n");
      return;
   }
   printf("%-30s %s\n", "PROJECT", "ROOT");
   printf("%-30s %s\n", "-------", "----");
   cJSON *p;
   cJSON_ArrayForEach(p, projects)
   {
      printf("%-30s %s\n", json_str(p, "name"), json_str(p, "root"));
   }
}

static void print_index_find(cJSON *resp)
{
   cJSON *hits = cJSON_GetObjectItemCaseSensitive(resp, "hits");
   if (!cJSON_IsArray(hits) || cJSON_GetArraySize(hits) == 0)
   {
      printf("No matches.\n");
      return;
   }
   cJSON *h;
   cJSON_ArrayForEach(h, hits)
   {
      cJSON *line = cJSON_GetObjectItemCaseSensitive(h, "line");
      printf("  %s:%d  %-12s [%s]\n", json_str(h, "file_path"),
             cJSON_IsNumber(line) ? (int)line->valuedouble : 0, json_str(h, "kind"),
             json_str(h, "project"));
   }
}

static void print_index_blast_radius(cJSON *resp)
{
   printf("Blast radius for %s:\n", json_str(resp, "file"));
   cJSON *dependents = cJSON_GetObjectItemCaseSensitive(resp, "dependents");
   cJSON *dependencies = cJSON_GetObjectItemCaseSensitive(resp, "dependencies");
   int shown = 0;
   if (cJSON_IsArray(dependents) && cJSON_GetArraySize(dependents) > 0)
   {
      printf("  Dependents (%d):\n", cJSON_GetArraySize(dependents));
      cJSON *d;
      cJSON_ArrayForEach(d, dependents)
      {
         if (cJSON_IsString(d))
            printf("    %s\n", d->valuestring);
      }
      shown = 1;
   }
   if (cJSON_IsArray(dependencies) && cJSON_GetArraySize(dependencies) > 0)
   {
      printf("  Dependencies (%d):\n", cJSON_GetArraySize(dependencies));
      cJSON *d;
      cJSON_ArrayForEach(d, dependencies)
      {
         if (cJSON_IsString(d))
            printf("    %s\n", d->valuestring);
      }
      shown = 1;
   }
   if (!shown)
      printf("  No dependents or dependencies found.\n");
}

static void print_index_structure(cJSON *resp)
{
   cJSON *defs = cJSON_GetObjectItemCaseSensitive(resp, "definitions");
   if (!cJSON_IsArray(defs) || cJSON_GetArraySize(defs) == 0)
   {
      printf("No definitions found.\n");
      return;
   }
   cJSON *d;
   cJSON_ArrayForEach(d, defs)
   {
      cJSON *line = cJSON_GetObjectItemCaseSensitive(d, "line");
      printf("  %4d  %-12s %s\n", cJSON_IsNumber(line) ? (int)line->valuedouble : 0,
             json_str(d, "kind"), json_str(d, "name"));
   }
}

static void print_index_callers(cJSON *resp)
{
   cJSON *hits = cJSON_GetObjectItemCaseSensitive(resp, "hits");
   if (!cJSON_IsArray(hits) || cJSON_GetArraySize(hits) == 0)
   {
      printf("No callers found.\n");
      return;
   }
   cJSON *h;
   cJSON_ArrayForEach(h, hits)
   {
      cJSON *line = cJSON_GetObjectItemCaseSensitive(h, "line");
      const char *caller = json_str(h, "caller");
      if (caller[0])
         printf("  %s:%d in %s() [%s]\n", json_str(h, "file_path"),
                cJSON_IsNumber(line) ? (int)line->valuedouble : 0, caller, json_str(h, "project"));
      else
         printf("  %s:%d (file scope) [%s]\n", json_str(h, "file_path"),
                cJSON_IsNumber(line) ? (int)line->valuedouble : 0, json_str(h, "project"));
   }
}

/* Print the AMBIGUOUS candidate rows shared by the --review view and the --dry-run
 * inline section. */
static void print_index_deps_ambiguous(cJSON *amb)
{
   cJSON *a;
   cJSON_ArrayForEach(a, amb)
   {
      cJSON *score = cJSON_GetObjectItemCaseSensitive(a, "evidence_score");
      printf("  %s -> %s  [%s, score %.2f]  %s\n", json_str(a, "caller_repo"),
             json_str(a, "candidate_definer"), json_str(a, "review_class"),
             cJSON_IsNumber(score) ? score->valuedouble : 0.0, json_str(a, "symbol"));
   }
}

/* S6: `aimee index deps`. Three response shapes: the AMBIGUOUS review queue
 * (--review / status=ambiguous) carries an `ambiguous` array + `overflow.dropped`;
 * --dry-run carries BOTH a `deps` array (down to LOW) and an inline `ambiguous`
 * array with `dry_run:true`; the default carries a `deps` edge array + `truncated`. */
static void print_index_deps(cJSON *resp)
{
   int dry_run = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "dry_run"));
   cJSON *amb = cJSON_GetObjectItemCaseSensitive(resp, "ambiguous");
   /* --review view: an `ambiguous` array with no `deps` (dry-run has both). */
   if (cJSON_IsArray(amb) && !dry_run)
   {
      if (cJSON_GetArraySize(amb) == 0)
         printf("No ambiguous candidates in the review queue.\n");
      print_index_deps_ambiguous(amb);
      cJSON *ov = cJSON_GetObjectItemCaseSensitive(resp, "overflow");
      cJSON *dropped = ov ? cJSON_GetObjectItemCaseSensitive(ov, "dropped") : NULL;
      if (cJSON_IsNumber(dropped) && dropped->valuedouble > 0)
         printf("WARNING: %d ambiguous candidate(s) dropped from the review queue (overflow); "
                "tune distinctiveness or widen review_queue_max.\n",
                (int)dropped->valuedouble);
      return;
   }

   if (dry_run)
      printf("[dry-run: candidate inspection — every band incl. LOW/AMBIGUOUS; nothing written]\n");

   cJSON *deps = cJSON_GetObjectItemCaseSensitive(resp, "deps");
   if (!cJSON_IsArray(deps) || cJSON_GetArraySize(deps) == 0)
   {
      printf("No cross-repo dependencies found.\n");
      if (!dry_run)
         return;
   }
   cJSON *d;
   cJSON_ArrayForEach(d, deps)
   {
      cJSON *sc = cJSON_GetObjectItemCaseSensitive(d, "symbol_count");
      cJSON *cc = cJSON_GetObjectItemCaseSensitive(d, "call_site_count");
      cJSON *ex = cJSON_GetObjectItemCaseSensitive(d, "example");
      const char *etype = json_str(d, "evidence_type");
      const char *bkind = json_str(d, "build_kind");
      char evtag[48] = "";
      if (etype[0] && strcmp(etype, "symbol_resolved") != 0)
         snprintf(evtag, sizeof(evtag), "  [%s%s%s]", etype, bkind[0] ? ":" : "", bkind);
      printf("  %s -> %s  %-7s  (%d symbol(s), %d call site(s))%s\n", json_str(d, "caller_repo"),
             json_str(d, "definer_repo"), json_str(d, "tier"),
             cJSON_IsNumber(sc) ? (int)sc->valuedouble : 0,
             cJSON_IsNumber(cc) ? (int)cc->valuedouble : 0, evtag);
      if (ex && json_str(ex, "symbol")[0])
      {
         cJSON *line = cJSON_GetObjectItemCaseSensitive(ex, "line");
         printf("      e.g. %s at %s:%d\n", json_str(ex, "symbol"), json_str(ex, "file"),
                cJSON_IsNumber(line) ? (int)line->valuedouble : 0);
      }
   }
   cJSON *trunc = cJSON_GetObjectItemCaseSensitive(resp, "truncated");
   if (cJSON_IsBool(trunc) && cJSON_IsTrue(trunc))
      printf("(results truncated; narrow the query)\n");

   /* --dry-run: also print the AMBIGUOUS candidates the pipeline held back. */
   if (dry_run && cJSON_IsArray(amb))
   {
      int na = cJSON_GetArraySize(amb);
      printf("ambiguous candidates (%d):\n", na);
      if (na == 0)
         printf("  (none)\n");
      print_index_deps_ambiguous(amb);
   }
}

static void print_hud_status(cJSON *resp)
{
   cJSON *mode = cJSON_GetObjectItemCaseSensitive(resp, "mode");
   cJSON *total_calls = cJSON_GetObjectItemCaseSensitive(resp, "total_calls");
   cJSON *successful_calls = cJSON_GetObjectItemCaseSensitive(resp, "successful_calls");
   cJSON *failed_calls = cJSON_GetObjectItemCaseSensitive(resp, "failed_calls");
   cJSON *prompt = cJSON_GetObjectItemCaseSensitive(resp, "total_prompt_tokens");
   cJSON *completion = cJSON_GetObjectItemCaseSensitive(resp, "total_completion_tokens");
   cJSON *cache_w = cJSON_GetObjectItemCaseSensitive(resp, "total_cache_write_tokens");
   cJSON *cache_r = cJSON_GetObjectItemCaseSensitive(resp, "total_cache_read_tokens");
   cJSON *cost = cJSON_GetObjectItemCaseSensitive(resp, "total_estimated_cost_usd");
   cJSON *turns = cJSON_GetObjectItemCaseSensitive(resp, "total_turns");
   cJSON *tools = cJSON_GetObjectItemCaseSensitive(resp, "total_tool_calls");
   cJSON *latency = cJSON_GetObjectItemCaseSensitive(resp, "avg_latency_ms");
   cJSON *recent = cJSON_GetObjectItemCaseSensitive(resp, "recent_calls");
   cJSON *recent_ok = cJSON_GetObjectItemCaseSensitive(resp, "recent_successes");

   long long prompt_tokens = cJSON_IsNumber(prompt) ? (long long)prompt->valuedouble : 0;
   long long completion_tokens =
       cJSON_IsNumber(completion) ? (long long)completion->valuedouble : 0;
   long long cache_write = cJSON_IsNumber(cache_w) ? (long long)cache_w->valuedouble : 0;
   long long cache_read = cJSON_IsNumber(cache_r) ? (long long)cache_r->valuedouble : 0;
   const char *mode_value = "unknown";

   if (cJSON_IsString(mode) && mode->valuestring[0])
      mode_value = mode->valuestring;

   printf("Session Status\n");
   printf("  Mode:        %s\n", mode_value);
   printf("  Delegates:   %d total  (%d ok, %d failed)\n",
          cJSON_IsNumber(total_calls) ? (int)total_calls->valuedouble : 0,
          cJSON_IsNumber(successful_calls) ? (int)successful_calls->valuedouble : 0,
          cJSON_IsNumber(failed_calls) ? (int)failed_calls->valuedouble : 0);
   printf("  Tokens:      %lld prompt / %lld completion", prompt_tokens, completion_tokens);
   if (cache_write > 0 || cache_read > 0)
      printf(" | cache: %lldw / %lldr", cache_write, cache_read);
   if (cJSON_IsNumber(cost) && cost->valuedouble > 0.0)
      printf(" | ~$%.4f", cost->valuedouble);
   printf("\n");
   printf("  Turns:       %d  |  Tool calls: %d\n",
          cJSON_IsNumber(turns) ? (int)turns->valuedouble : 0,
          cJSON_IsNumber(tools) ? (int)tools->valuedouble : 0);
   printf("  Avg latency: %.0f ms\n", cJSON_IsNumber(latency) ? latency->valuedouble : 0.0);
   printf("  Recent (5m): %d calls  (%d ok)\n",
          cJSON_IsNumber(recent) ? (int)recent->valuedouble : 0,
          cJSON_IsNumber(recent_ok) ? (int)recent_ok->valuedouble : 0);
}

static void print_init_run(cJSON *resp)
{
   cJSON *local = cJSON_GetObjectItemCaseSensitive(resp, "local_ready");
   cJSON *knowledge = cJSON_GetObjectItemCaseSensitive(resp, "knowledge_ready");
   cJSON *rules_generated = cJSON_GetObjectItemCaseSensitive(resp, "rules_generated");
   cJSON *rules_exists = cJSON_GetObjectItemCaseSensitive(resp, "rules_already_exists");

   printf("Initialized\n");
   printf("  Local:      %s\n", cJSON_IsTrue(local) ? "ready" : "unavailable");
   printf("  Knowledge:  %s\n", cJSON_IsTrue(knowledge) ? "ready" : "not ready");
   if (cJSON_IsTrue(rules_generated))
      printf("  Rules:      generated .aimee-rules\n");
   else if (cJSON_IsTrue(rules_exists))
      printf("  Rules:      existing .aimee-rules preserved\n");

   cJSON *k = cJSON_GetObjectItemCaseSensitive(resp, "knowledge");
   cJSON *msg = cJSON_IsObject(k) ? cJSON_GetObjectItemCaseSensitive(k, "message") : NULL;
   cJSON *rem = cJSON_IsObject(k) ? cJSON_GetObjectItemCaseSensitive(k, "remediation") : NULL;
   if (!cJSON_IsTrue(knowledge) && cJSON_IsString(msg) && msg->valuestring[0])
      printf("  Note:       %s\n", msg->valuestring);
   if (!cJSON_IsTrue(knowledge) && cJSON_IsString(rem) && rem->valuestring[0])
      printf("  Fix:        %s\n", rem->valuestring);
}

static void print_storage_migrate(cJSON *resp)
{
   cJSON *source = cJSON_GetObjectItemCaseSensitive(resp, "source");
   cJSON *backup = cJSON_GetObjectItemCaseSensitive(resp, "backup_path");
   cJSON *tables = cJSON_GetObjectItemCaseSensitive(resp, "tables_found");
   cJSON *source_rows = cJSON_GetObjectItemCaseSensitive(resp, "source_rows");
   cJSON *copied = cJSON_GetObjectItemCaseSensitive(resp, "rows_copied");
   cJSON *skipped = cJSON_GetObjectItemCaseSensitive(resp, "rows_skipped");
   cJSON *table_errors = cJSON_GetObjectItemCaseSensitive(resp, "table_errors");
   cJSON *regressions = cJSON_GetObjectItemCaseSensitive(resp, "row_count_regressions");

   printf("Migration complete\n");
   if (cJSON_IsString(source) && source->valuestring[0])
      printf("  Source:      %s\n", source->valuestring);
   printf("  Tables:      %d\n", cJSON_IsNumber(tables) ? tables->valueint : 0);
   printf("  Source rows: %d\n", cJSON_IsNumber(source_rows) ? source_rows->valueint : 0);
   printf("  Copied:      %d\n", cJSON_IsNumber(copied) ? copied->valueint : 0);
   printf("  Skipped:     %d\n", cJSON_IsNumber(skipped) ? skipped->valueint : 0);
   printf("  Errors:      %d\n", cJSON_IsNumber(table_errors) ? table_errors->valueint : 0);
   printf("  Regressions: %d\n", cJSON_IsNumber(regressions) ? regressions->valueint : 0);
   if (cJSON_IsString(backup) && backup->valuestring[0])
      printf("  Backup:      %s\n", backup->valuestring);
}

static void print_episode_list(cJSON *resp)
{
   cJSON *episodes = cJSON_GetObjectItemCaseSensitive(resp, "episodes");
   if (!cJSON_IsArray(episodes) || cJSON_GetArraySize(episodes) == 0)
   {
      printf("No episodes recorded.\n");
      return;
   }
   cJSON *ep;
   cJSON_ArrayForEach(ep, episodes)
   {
      const char *agent = json_str(ep, "agent");
      const char *role = json_str(ep, "role");
      const char *ts = json_str(ep, "created_at");
      int turns = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(ep, "turns"));
      int tools = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(ep, "tool_calls"));
      int conf = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(ep, "confidence"));
      int lat = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(ep, "latency_ms"));
      int ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ep, "success"));
      printf("%-16s [%-8s] %2d turns %2d tools %s conf=%d %dms  %s\n", agent[0] ? agent : "?",
             role[0] ? role : "?", turns, tools, ok ? "ok  " : "FAIL", conf, lat, ts[0] ? ts : "?");
   }
}

static void print_eval_run(cJSON *resp)
{
   cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "results");
   printf("%-30s %-12s %-12s %-6s %-6s %-8s %-10s\n", "Task", "Agent", "Ablation", "Pass", "Turns",
          "ToolOK", "Latency");
   if (cJSON_IsArray(rows))
   {
      cJSON *row;
      cJSON_ArrayForEach(row, rows)
      {
         double tool_ok =
             cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(row, "tool_call_success_rate"));
         printf("%-30s %-12s %-12s %-6s %-6d %-7.2f%% %-10dms\n", json_str(row, "task_name"),
                json_str(row, "agent_name"), json_str(row, "ablation"),
                json_int(row, "success", 0) ? "PASS" : "FAIL", json_int(row, "turns", 0),
                tool_ok * 100.0, json_int(row, "latency_ms", 0));
      }
   }
   printf("\n%d/%d passed.\n", json_int(resp, "passes", 0), json_int(resp, "total", 0));
}

static void print_eval_results(cJSON *resp)
{
   cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "results");
   if (!cJSON_IsArray(rows) || cJSON_GetArraySize(rows) == 0)
   {
      printf("No eval results.\n");
      return;
   }
   printf("%-15s %-25s %-12s %-12s %-6s %-6s %-6s %-10s %s\n", "Suite", "Task", "Agent", "Ablation",
          "Pass", "Turns", "Tools", "Latency", "Time");
   cJSON *row;
   cJSON_ArrayForEach(row, rows)
   {
      printf("%-15s %-25s %-12s %-12s %-6s %-6d %-6d %-10dms %s\n", json_str(row, "suite"),
             json_str(row, "task_name"), json_str(row, "agent_name"), json_str(row, "ablation"),
             json_int(row, "success", 0) ? "PASS" : "FAIL", json_int(row, "turns", 0),
             json_int(row, "tool_calls", 0), json_int(row, "latency_ms", 0),
             json_str(row, "created_at"));
   }
}

static void print_delegate_status_object(cJSON *job, cJSON *full_result, cJSON *result_limit)
{
   cJSON *id = cJSON_GetObjectItemCaseSensitive(job, "job_id");
   cJSON *status = cJSON_GetObjectItemCaseSensitive(job, "job_status");
   cJSON *role = cJSON_GetObjectItemCaseSensitive(job, "role");
   cJSON *agent = cJSON_GetObjectItemCaseSensitive(job, "agent_name");
   cJSON *turns = cJSON_GetObjectItemCaseSensitive(job, "cursor_turn");
   cJSON *result = cJSON_GetObjectItemCaseSensitive(job, "result");
   cJSON *tool = cJSON_GetObjectItemCaseSensitive(job, "current_tool");
   cJSON *calls = cJSON_GetObjectItemCaseSensitive(job, "api_call_count");
   cJSON *default_max_turns = cJSON_GetObjectItemCaseSensitive(job, "default_max_turns");
   cJSON *final_after_turns = cJSON_GetObjectItemCaseSensitive(job, "final_after_turns");
   cJSON *heartbeat = cJSON_GetObjectItemCaseSensitive(job, "heartbeat_at");
   cJSON *created = cJSON_GetObjectItemCaseSensitive(job, "created_at");
   cJSON *updated = cJSON_GetObjectItemCaseSensitive(job, "updated_at");

   printf("job_id: %d\n", cJSON_IsNumber(id) ? id->valueint : 0);
   printf("status: %s\n", cJSON_IsString(status) ? status->valuestring : "unknown");
   if (cJSON_IsString(role) && role->valuestring[0])
      printf("role: %s\n", role->valuestring);
   if (cJSON_IsString(agent) && agent->valuestring[0])
      printf("agent: %s\n", agent->valuestring);
   if (cJSON_IsNumber(turns))
      printf("turns: %d\n", turns->valueint);
   if (cJSON_IsString(tool) && tool->valuestring[0])
      printf("tool: %s\n", tool->valuestring);
   if (cJSON_IsNumber(calls) && calls->valueint > 0)
      printf("api_calls: %d\n", calls->valueint);
   if (cJSON_IsNumber(default_max_turns) && default_max_turns->valueint > 0)
      printf("max_turns: %d\n", default_max_turns->valueint);
   if (cJSON_IsNumber(final_after_turns) && final_after_turns->valueint > 0)
      printf("final_after: turn %d\n", final_after_turns->valueint);
   if (cJSON_IsString(heartbeat) && heartbeat->valuestring[0])
      printf("heartbeat_at: %s\n", heartbeat->valuestring);
   if (cJSON_IsString(updated) && updated->valuestring[0])
      printf("updated_at: %s\n", updated->valuestring);
   if (cJSON_IsString(created) && created->valuestring[0])
      printf("created_at: %s\n", created->valuestring);
   if (cJSON_IsString(result) && result->valuestring[0])
   {
      if (cJSON_IsTrue(full_result))
      {
         printf("result: %s\n", result->valuestring);
      }
      else
      {
         int limit = cJSON_IsNumber(result_limit) ? result_limit->valueint : 200;
         if (limit < 0)
            limit = 0;
         int len = (int)strlen(result->valuestring);
         printf("result: %.*s\n", limit, result->valuestring);
         if (len > limit)
            printf("result_truncated: true\nresult_chars: %d\n", len);
      }
   }
}

static void print_workspace_add(cJSON *resp)
{
   const char *path = json_str(resp, "path");
   cJSON *projects = cJSON_GetObjectItemCaseSensitive(resp, "projects");
   int count = cJSON_IsArray(projects) ? cJSON_GetArraySize(projects) : 0;
   printf("workspace: added %s (%d project(s) discovered)\n", path, count);
   if (cJSON_IsArray(projects))
   {
      cJSON *p;
      cJSON_ArrayForEach(p, projects)
      {
         const char *name = json_str(p, "name");
         if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "indexed")))
            printf("  indexed: %s\n", name);
         else
            printf("  %s (not indexed: %s)\n", name, json_str(p, "reason"));
      }
   }
}

static void print_workspace_get(cJSON *resp)
{
   cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "manifest");
   if (!cJSON_IsObject(m))
   {
      printf("workspace: no manifest returned\n");
      return;
   }
   printf("root:     %s\n", json_str(m, "root"));
   printf("provider: %s\n", json_str(m, "provider"));
   printf("exists:   %s\n",
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "exists")) ? "yes" : "no");
   cJSON *vcs = cJSON_GetObjectItemCaseSensitive(m, "vcs");
   if (cJSON_IsObject(vcs))
   {
      const char *remote = json_str(vcs, "remote");
      const char *head = json_str(vcs, "head");
      const char *branch = json_str(vcs, "branch");
      if ((remote && remote[0]) || (head && head[0]) || (branch && branch[0]))
      {
         printf("vcs:\n");
         if (remote && remote[0])
            printf("  remote: %s\n", remote);
         if (branch && branch[0])
            printf("  branch: %s\n", branch);
         if (head && head[0])
            printf("  head:   %s\n", head);
      }
      else
         printf("vcs:      (not a git repo)\n");
   }
}

static void print_workspace_list(cJSON *resp)
{
   cJSON *workspaces = cJSON_GetObjectItemCaseSensitive(resp, "workspaces");
   if (!cJSON_IsArray(workspaces) || cJSON_GetArraySize(workspaces) == 0)
   {
      printf("No workspaces configured. Use 'aimee workspace add <path>' to add one.\n");
      return;
   }
   cJSON *ws;
   cJSON_ArrayForEach(ws, workspaces)
   {
      /* Surface a non-default provider and (for mirror) its vcs coordinates so a
       * detached/mirror workspace is distinguishable from a plain co-located one. */
      const char *prov = json_str(ws, "provider");
      const char *remote = json_str(ws, "remote");
      const char *head = json_str(ws, "head");
      if (prov && prov[0] && strcmp(prov, "shared") != 0)
      {
         printf("%s  [%s", json_str(ws, "path"), prov);
         if (remote && remote[0])
            printf(" remote=%s", remote);
         if (head && head[0])
            printf(" head=%.10s", head);
         printf("]\n");
      }
      else
         printf("%s\n", json_str(ws, "path"));
      cJSON *projs = cJSON_GetObjectItemCaseSensitive(ws, "projects");
      if (cJSON_IsArray(projs))
      {
         cJSON *pr;
         cJSON_ArrayForEach(pr, projs) if (cJSON_IsString(pr)) printf("  %s\n", pr->valuestring);
      }
   }
}

/* --- print_text_output dispatch: one printer per CLI method, table-driven.
 * Extracted verbatim from the former ~1200-line if/else-if ladder. Adding a
 * method's text rendering is now one printer + one table row. Each printer reads
 * only `resp` (plus `method` for the few that need it). The skill.* prefix branch
 * keeps its internal sub-dispatch and is matched after the exact table — exactly
 * as the ladder did (skill.list / skill.show were exact branches before it). --- */
void pt_print_init_run(const char *method, cJSON *resp)
{
   print_init_run(resp);
}
void pt_print_migrate_v2(const char *method, cJSON *resp)
{
   print_storage_migrate(resp);
}
void pt_print_rules_generate(const char *method, cJSON *resp)
{
   cJSON *c = cJSON_GetObjectItemCaseSensitive(resp, "content");
   if (cJSON_IsString(c) && c->valuestring[0])
      printf("%s", c->valuestring);
}
void pt_print_skill_list(const char *method, cJSON *resp)
{
   cJSON *skills = cJSON_GetObjectItemCaseSensitive(resp, "skills");
   cJSON *item;
   cJSON_ArrayForEach(item, skills)
   {
      printf("%-32s  %s\n", json_str(item, "name"), json_str(item, "source"));
   }
}
void pt_print_skill_show(const char *method, cJSON *resp)
{
   cJSON *c = cJSON_GetObjectItemCaseSensitive(resp, "content");
   if (cJSON_IsString(c) && c->valuestring)
   {
      fputs(c->valuestring, stdout);
      size_t len = strlen(c->valuestring);
      if (len == 0 || c->valuestring[len - 1] != '\n')
         fputc('\n', stdout);
   }
}
void pt_print_skill_group(const char *method, cJSON *resp)
{
   const char *status = json_str(resp, "status");
   if (strcmp(method, "skill.lint") == 0)
   {
      cJSON *report = cJSON_GetObjectItemCaseSensitive(resp, "report");
      if (cJSON_IsString(report) && report->valuestring[0])
         fputs(report->valuestring, stderr);
      else
         printf("skill lint: %d skills passed\n", json_int(resp, "checked", 0));
   }
   else if (strcmp(method, "skill.eval") == 0)
   {
      printf("skill eval: %s %s\n", json_str(resp, "name"),
             strcmp(status, "ok") == 0 ? "PASS" : "FAIL");
      printf("scenarios: %d\n", json_int(resp, "scenarios", 0));
      printf("baseline violations: %d/%d\n", json_int(resp, "baseline_violations", 0),
             json_int(resp, "scenarios", 0));
      printf("treatment compliance: %d/%d\n", json_int(resp, "treatment_compliances", 0),
             json_int(resp, "scenarios", 0));
      printf("compliance delta: %.2f\n", json_double(resp, "compliance_delta", 0.0));
      if (strcmp(status, "ok") != 0)
         fprintf(stderr, "skill eval: %s\n", json_str(resp, "message"));
   }
   else if (strcmp(method, "skill.lifecycle") == 0)
   {
      printf("skill lifecycle: %d considered, %d stale, %d archived, %d pinned skipped\n",
             json_int(resp, "considered", 0), json_int(resp, "stale_marked", 0),
             json_int(resp, "archived", 0), json_int(resp, "skipped_pinned", 0));
   }
   else if (strcmp(method, "skill.autostub") == 0)
   {
      if (strcmp(status, "disabled") == 0)
         printf("skill autostub: disabled (set skills.capability.autostub or pass --force)\n");
      else if (strcmp(status, "ok") == 0)
         printf("skill autostub: %d scanned, %d covered, %d proposed, %d skipped\n",
                json_int(resp, "scanned", 0), json_int(resp, "existing", 0),
                json_int(resp, "proposed", 0), json_int(resp, "skipped", 0));
      else
         fprintf(stderr, "skill autostub: %s\n", json_str(resp, "message"));
   }
   else if (strcmp(status, "ok") == 0)
      printf("skill %s: %s\n", json_str(resp, "action"), json_str(resp, "name"));
   else
      fprintf(stderr, "skill: %s\n", json_str(resp, "message"));
}
void pt_print_git_verify(const char *method, cJSON *resp)
{
   print_mcp_content(resp);
}
void pt_print_get_help(const char *method, cJSON *resp)
{
   print_mcp_content(resp);
}
void pt_print_server_health(const char *method, cJSON *resp)
{
   print_server_health(resp);
}
void pt_print_session_list(const char *method, cJSON *resp)
{
   print_session_list(resp);
}
void pt_print_session_get(const char *method, cJSON *resp)
{
   print_session_get(resp);
}
void pt_print_session_close(const char *method, cJSON *resp)
{
   print_session_close(resp);
}
void pt_print_session_brief(const char *method, cJSON *resp)
{
   print_session_brief(resp);
}
void pt_print_session_attach(const char *method, cJSON *resp)
{
   const char *aid = json_str(resp, "attach_id");
   printf("attached: %s\n", (aid && aid[0]) ? aid : "(none)");
}
void pt_print_session_detach(const char *method, cJSON *resp)
{
   cJSON *d = cJSON_GetObjectItemCaseSensitive(resp, "detached");
   printf("%s\n", cJSON_IsTrue(d) ? "detached" : "no such attachment");
}
void pt_print_session_presence(const char *method, cJSON *resp)
{
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(resp, "presences");
   int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
   if (n == 0)
      printf("(no live presences)\n");
   cJSON *p = NULL;
   cJSON_ArrayForEach(p, arr)
   {
      const char *sid = json_str(p, "session_id");
      const char *owner = json_str(p, "owner");
      cJSON *att = cJSON_GetObjectItemCaseSensitive(p, "attachments");
      cJSON *tif = cJSON_GetObjectItemCaseSensitive(p, "turn_in_flight");
      const char *tid = json_str(p, "turn_id");
      printf("%-32s owner=%-12s attachments=%d%s%s\n", sid ? sid : "(?)",
             (owner && owner[0]) ? owner : "-", cJSON_IsNumber(att) ? att->valueint : 0,
             cJSON_IsTrue(tif) ? " turn=" : "", cJSON_IsTrue(tif) && tid ? tid : "");
   }
}
void pt_print_trajectory_export(const char *method, cJSON *resp)
{
   cJSON *trajectory = cJSON_GetObjectItemCaseSensitive(resp, "trajectory");
   char *json = trajectory ? cJSON_PrintUnformatted(trajectory) : NULL;
   if (json)
   {
      puts(json);
      free(json);
   }
}
void pt_print_trajectory_batch(const char *method, cJSON *resp)
{
   char *json = cJSON_PrintUnformatted(resp);
   if (json)
   {
      puts(json);
      free(json);
   }
}
void pt_print_insights_overview(const char *method, cJSON *resp)
{
   print_insights_overview(resp);
}
void pt_print_worktree_gc(const char *method, cJSON *resp)
{
   print_worktree_gc(resp);
}
void pt_print_work_add(const char *method, cJSON *resp)
{
   printf("Added work item %s: %s\n", json_str(resp, "id"), json_str(resp, "title"));
}
void pt_print_work_add_batch(const char *method, cJSON *resp)
{
   int added = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "added"));
   int skipped = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "skipped"));
   printf("Added %d work items from proposals (%d already queued).\n", added, skipped);
}
void pt_print_work_claim(const char *method, cJSON *resp)
{
   print_work_claim(resp);
}
void pt_print_work_complete(const char *method, cJSON *resp)
{
   print_work_finish(resp, "done");
}
void pt_print_work_fail(const char *method, cJSON *resp)
{
   print_work_finish(resp, "failed");
}
void pt_print_work_list(const char *method, cJSON *resp)
{
   print_work_list(resp);
}
void pt_print_work_board(const char *method, cJSON *resp)
{
   print_work_board(resp);
}
void pt_print_work_cancel(const char *method, cJSON *resp)
{
   print_work_simple_item(resp, "cancelled", "Cancelled", "No pending item found.");
}
void pt_print_work_release(const char *method, cJSON *resp)
{
   print_work_simple_item(resp, "released", "Released", "No claimed item found.");
}
void pt_print_work_clear(const char *method, cJSON *resp)
{
   int cleared = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "cleared"));
   printf("Cleared %d %s items.\n", cleared, json_str(resp, "status_filter"));
}
void pt_print_work_gc(const char *method, cJSON *resp)
{
   print_work_gc(resp);
}
void pt_print_work_sync_proposals(const char *method, cJSON *resp)
{
   print_work_sync_proposals(resp);
}
void pt_print_work_stats(const char *method, cJSON *resp)
{
   print_work_stats(resp);
}
void pt_print_rules_delete(const char *method, cJSON *resp)
{
   print_rules_delete(resp);
}
void pt_print_memory_search(const char *method, cJSON *resp)
{
   print_memory_search(resp);
}
void pt_print_memory_store(const char *method, cJSON *resp)
{
   cJSON *id = cJSON_GetObjectItemCaseSensitive(resp, "id");
   if (cJSON_IsNumber(id))
      printf("stored memory %lld\n", (long long)id->valuedouble);
   else
      printf("stored memory\n");
}
void pt_print_memory_list(const char *method, cJSON *resp)
{
   print_memory_list(resp);
}
void pt_print_memory_get(const char *method, cJSON *resp)
{
   print_memory_row(resp);
}
void pt_print_memory_read(const char *method, cJSON *resp)
{
   cJSON *context = cJSON_GetObjectItemCaseSensitive(resp, "context");
   if (cJSON_IsString(context))
      printf("%s", context->valuestring);
}
void pt_print_memory_stats(const char *method, cJSON *resp)
{
   print_memory_stats(resp);
}
void pt_print_index_scan(const char *method, cJSON *resp)
{
   print_index_scan(resp);
}
void pt_print_index_list(const char *method, cJSON *resp)
{
   print_index_list(resp);
}
void pt_print_index_find(const char *method, cJSON *resp)
{
   print_index_find(resp);
}
void pt_print_index_blast_radius(const char *method, cJSON *resp)
{
   print_index_blast_radius(resp);
}
void pt_print_index_structure(const char *method, cJSON *resp)
{
   print_index_structure(resp);
}
void pt_print_index_find_callers(const char *method, cJSON *resp)
{
   print_index_callers(resp);
}
void pt_print_index_deps(const char *method, cJSON *resp)
{
   print_index_deps(resp);
}
void pt_print_repo_trust(const char *method, cJSON *resp)
{
   (void)method;
   cJSON *changed = cJSON_GetObjectItemCaseSensitive(resp, "changed");
   const char *prior = json_str(resp, "prior_trust");
   const char *now = json_str(resp, "new_trust");
   if (cJSON_IsBool(changed) && !cJSON_IsTrue(changed))
      printf("repo %s: trust already %s (no change)\n", json_str(resp, "project"), now);
   else
      printf("repo %s: trust %s -> %s\n", json_str(resp, "project"), prior[0] ? prior : "(unset)",
             now);
}
void pt_print_graph_sync_code(const char *method, cJSON *resp)
{
   printf("graph sync-code: project=%s generation=%s edges=%s\n", json_str(resp, "project"),
          json_str(resp, "generation_id"), json_str(resp, "edge_count"));
}
void pt_print_workspace_add(const char *method, cJSON *resp)
{
   print_workspace_add(resp);
}
void pt_print_workspace_list(const char *method, cJSON *resp)
{
   print_workspace_list(resp);
}
void pt_print_workspace_get(const char *method, cJSON *resp)
{
   print_workspace_get(resp);
}
void pt_print_workspace_remove(const char *method, cJSON *resp)
{
   printf("workspace: removed %s\n", json_str(resp, "removed"));
}
void pt_print_workspace_mirror_sync(const char *method, cJSON *resp)
{
   printf("workspace: synced client diff (%d bytes)\n", json_int(resp, "bytes", 0));
}
void pt_print_hud_status(const char *method, cJSON *resp)
{
   print_hud_status(resp);
}
void pt_print_agent_list(const char *method, cJSON *resp)
{
   print_agent_list(resp);
}
void pt_print_agent_local(const char *method, cJSON *resp)
{
   print_agent_local(resp);
}
void pt_print_agent_add(const char *method, cJSON *resp)
{
   print_agent_add(resp);
}
void pt_print_agent_remove(const char *method, cJSON *resp)
{
   print_agent_remove(resp);
}
void pt_print_agent_enable(const char *method, cJSON *resp)
{
   print_agent_enabled(resp, 1);
}
void pt_print_agent_disable(const char *method, cJSON *resp)
{
   print_agent_enabled(resp, 0);
}
void pt_print_agent_probe(const char *method, cJSON *resp)
{
   print_agent_probe(resp);
}
void pt_print_mcp_audit(const char *method, cJSON *resp)
{
   print_mcp_audit(resp);
}
void pt_print_mcp_recheck(const char *method, cJSON *resp)
{
   print_mcp_recheck(resp);
}
void pt_print_toolset_list(const char *method, cJSON *resp)
{
   print_toolset_list(resp);
}
void pt_print_toolset_show(const char *method, cJSON *resp)
{
   print_toolset_show(resp);
}
void pt_print_toolset_resolve(const char *method, cJSON *resp)
{
   print_toolset_resolve(resp);
}
void pt_print_trigger_list(const char *method, cJSON *resp)
{
   print_trigger_list(resp);
}
void pt_print_trigger_status(const char *method, cJSON *resp)
{
   print_trigger_status(resp);
}
void pt_print_trigger_fire(const char *method, cJSON *resp)
{
   print_trigger_fire(resp);
}
void pt_print_trigger_cancel(const char *method, cJSON *resp)
{
   print_trigger_cancel(resp);
}
void pt_print_cron_list(const char *method, cJSON *resp)
{
   print_cron_list(resp);
}
void pt_print_cron_add(const char *method, cJSON *resp)
{
   printf("cron job %s: %s\n", json_str(resp, "job_id"), json_str(resp, "status"));
}
void pt_print_cron_show(const char *method, cJSON *resp)
{
   print_cron_show(resp);
}
void pt_print_cron_history(const char *method, cJSON *resp)
{
   print_cron_history(resp);
}
void pt_print_cron_run(const char *method, cJSON *resp)
{
   print_cron_run(resp);
}
void pt_print_wm_get(const char *method, cJSON *resp)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(resp, "value");
   if (cJSON_IsString(v))
      printf("%s\n", v->valuestring);
}
void pt_print_wm_set(const char *method, cJSON *resp)
{
   cJSON *k = cJSON_GetObjectItemCaseSensitive(resp, "key");
   cJSON *v = cJSON_GetObjectItemCaseSensitive(resp, "value");
   if (cJSON_IsString(k) && cJSON_IsString(v))
      printf("set %s = %s\n", k->valuestring, v->valuestring);
}
static void pt_print_config_value(cJSON *v)
{
   if (cJSON_IsString(v))
      printf("%s\n", v->valuestring[0] ? v->valuestring : "(unset)");
   else if (cJSON_IsBool(v))
      printf("%s\n", cJSON_IsTrue(v) ? "true" : "false");
   else if (cJSON_IsNumber(v))
   {
      double d = v->valuedouble;
      if (d == (double)(long)d)
         printf("%ld\n", (long)d);
      else
         printf("%g\n", d);
   }
   else
      printf("(unset)\n");
}
void pt_print_config_get(const char *method, cJSON *resp)
{
   (void)method;
   pt_print_config_value(cJSON_GetObjectItemCaseSensitive(resp, "value"));
}
void pt_print_config_set(const char *method, cJSON *resp)
{
   (void)method;
   cJSON *k = cJSON_GetObjectItemCaseSensitive(resp, "key");
   cJSON *v = cJSON_GetObjectItemCaseSensitive(resp, "value");
   printf("set %s = ", cJSON_IsString(k) ? k->valuestring : "?");
   pt_print_config_value(v);
   /* Live/Restart verdict (live-config-reload P2). */
   cJSON *rl = cJSON_GetObjectItemCaseSensitive(resp, "reload");
   cJSON *live = cJSON_GetObjectItemCaseSensitive(resp, "applied_live");
   if (cJSON_IsString(rl) && live && !cJSON_IsTrue(live))
      printf("  (%s)\n", rl->valuestring);
}
void pt_print_config_show(const char *method, cJSON *resp)
{
   (void)method;
   cJSON *cfg = cJSON_GetObjectItemCaseSensitive(resp, "config");
   char *s = cJSON_Print(cfg ? cfg : resp);
   if (s)
   {
      printf("%s\n", s);
      free(s);
   }
}
void pt_print_wm_list(const char *method, cJSON *resp)
{
   cJSON *entries = cJSON_GetObjectItemCaseSensitive(resp, "entries");
   if (cJSON_IsArray(entries))
   {
      cJSON *el;
      int n = 0;
      cJSON_ArrayForEach(el, entries)
      {
         cJSON *cat = cJSON_GetObjectItemCaseSensitive(el, "category");
         cJSON *key = cJSON_GetObjectItemCaseSensitive(el, "key");
         cJSON *val = cJSON_GetObjectItemCaseSensitive(el, "value");
         if (cJSON_IsString(key) && cJSON_IsString(val))
            printf("[%s] %s: %s\n", cJSON_IsString(cat) ? cat->valuestring : "general",
                   key->valuestring, val->valuestring);
         n++;
      }
      if (n == 0)
         printf("(no entries)\n");
   }
}
void pt_print_delegate(const char *method, cJSON *resp)
{
   cJSON *job_id = cJSON_GetObjectItemCaseSensitive(resp, "job_id");
   if (cJSON_IsNumber(job_id))
   {
      cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "job_status");
      printf("job_id: %d\n", job_id->valueint);
      printf("status: %s\n", cJSON_IsString(status) ? status->valuestring : "running");
      return;
   }
   cJSON *r = cJSON_GetObjectItemCaseSensitive(resp, "response");
   if (cJSON_IsString(r) && r->valuestring[0])
      printf("%s\n", r->valuestring);
}
void pt_print_delegate_status(const char *method, cJSON *resp)
{
   cJSON *full_result = cJSON_GetObjectItemCaseSensitive(resp, "full_result");
   cJSON *result_limit = cJSON_GetObjectItemCaseSensitive(resp, "result_limit");
   cJSON *jobs = cJSON_GetObjectItemCaseSensitive(resp, "jobs");

   if (cJSON_IsArray(jobs))
   {
      int n = cJSON_GetArraySize(jobs);
      for (int i = 0; i < n; i++)
      {
         if (i > 0)
            printf("---\n");
         cJSON *job = cJSON_GetArrayItem(jobs, i);
         print_delegate_status_object(job, full_result, result_limit);
      }
      return;
   }

   print_delegate_status_object(resp, full_result, result_limit);
}
void pt_print_jobs_list(const char *method, cJSON *resp)
{
   print_jobs_list(resp);
}
void pt_print_jobs_status(const char *method, cJSON *resp)
{
   print_jobs_status(resp);
}
void pt_print_jobs_logs(const char *method, cJSON *resp)
{
   print_jobs_logs(resp);
}
void pt_print_jobs_cancel(const char *method, cJSON *resp)
{
   print_jobs_cancel(resp);
}
void pt_print_job_start(const char *method, cJSON *resp)
{
   print_coord_job_start(resp);
}
void pt_print_job_list(const char *method, cJSON *resp)
{
   print_coord_jobs_list(resp);
}
void pt_print_job_status(const char *method, cJSON *resp)
{
   print_coord_job_status(resp);
}
void pt_print_job_cancel(const char *method, cJSON *resp)
{
   print_coord_job_cancel(resp);
}
void pt_print_aux_config_show(const char *method, cJSON *resp)
{
   print_aux_config(resp);
}
void pt_print_aux_test(const char *method, cJSON *resp)
{
   cJSON *r = cJSON_GetObjectItemCaseSensitive(resp, "response");
   if (cJSON_IsString(r))
      printf("%s\n", r->valuestring);
}
void pt_print_delegate_log(const char *method, cJSON *resp)
{
   print_episode_list(resp);
}
void pt_print_delegate_launch(const char *method, cJSON *resp)
{
   cJSON *job_id = cJSON_GetObjectItemCaseSensitive(resp, "job_id");
   cJSON *plan_id = cJSON_GetObjectItemCaseSensitive(resp, "plan_id");
   cJSON *tasks = cJSON_GetObjectItemCaseSensitive(resp, "tasks");
   cJSON *max = cJSON_GetObjectItemCaseSensitive(resp, "max_concurrent");
   int id = cJSON_IsNumber(job_id) ? job_id->valueint : 0;
   printf("Queued coord job #%d from delegate plan #%d: %d tasks, max %d concurrent\n", id,
          cJSON_IsNumber(plan_id) ? plan_id->valueint : 0,
          cJSON_IsNumber(tasks) ? tasks->valueint : 0, cJSON_IsNumber(max) ? max->valueint : 0);
   printf("Inspect packet progress with: aimee job status %d\n", id);
   printf("Note: coord jobs are queued packet plans. `aimee jobs status` and "
          "`aimee delegate status` show durable background delegate jobs, not coord jobs.\n");
}
void pt_print_kb_search(const char *method, cJSON *resp)
{
   (void)method;
   /* The kb /v1/search response (relayed by kb.search) is
    * {"hits":[{artifact_id,score,excerpt}], ...}. Render that. (Older callers
    * returned {"results":[{file_path,heading_path,score}]} — kept as a fallback.) */
   cJSON *hits = cJSON_GetObjectItemCaseSensitive(resp, "hits");
   if (cJSON_IsArray(hits))
   {
      if (cJSON_GetArraySize(hits) == 0)
      {
         printf("No results.\n");
         return;
      }
      cJSON *h;
      cJSON_ArrayForEach(h, hits)
      {
         cJSON *score = cJSON_GetObjectItemCaseSensitive(h, "score");
         cJSON *id = cJSON_GetObjectItemCaseSensitive(h, "artifact_id");
         cJSON *ex = cJSON_GetObjectItemCaseSensitive(h, "excerpt");
         printf("  %.4f  %s\n", cJSON_IsNumber(score) ? score->valuedouble : 0.0,
                cJSON_IsString(id) ? id->valuestring : "?");
         if (cJSON_IsString(ex) && ex->valuestring[0])
            printf("        %.200s\n", ex->valuestring);
      }
      return;
   }

   cJSON *results = cJSON_GetObjectItemCaseSensitive(resp, "results");
   if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0)
   {
      printf("No results.\n");
      return;
   }
   cJSON *r;
   cJSON_ArrayForEach(r, results)
   {
      cJSON *score = cJSON_GetObjectItemCaseSensitive(r, "score");
      cJSON *fp = cJSON_GetObjectItemCaseSensitive(r, "file_path");
      cJSON *hp = cJSON_GetObjectItemCaseSensitive(r, "heading_path");
      printf("  %.4f  %s  %s\n", cJSON_IsNumber(score) ? score->valuedouble : 0.0,
             cJSON_IsString(fp) ? fp->valuestring : "?", cJSON_IsString(hp) ? hp->valuestring : "");
   }
}
void pt_print_kb_build(const char *method, cJSON *resp)
{
   cJSON *proj = cJSON_GetObjectItemCaseSensitive(resp, "project");
   cJSON *fi = cJSON_GetObjectItemCaseSensitive(resp, "files_indexed");
   cJSON *ca = cJSON_GetObjectItemCaseSensitive(resp, "chunks_added");
   cJSON *ea = cJSON_GetObjectItemCaseSensitive(resp, "embeddings_added");
   printf("project: %s\n", cJSON_IsString(proj) ? proj->valuestring : "?");
   printf("files indexed:    %d\n", cJSON_IsNumber(fi) ? (int)fi->valuedouble : 0);
   printf("chunks added:     %d\n", cJSON_IsNumber(ca) ? (int)ca->valuedouble : 0);
   printf("embeddings added: %d\n", cJSON_IsNumber(ea) ? (int)ea->valuedouble : 0);
}
void pt_print_kb_ingest(const char *method, cJSON *resp)
{
   cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
   if (cJSON_IsString(msg))
      printf("%s\n", msg->valuestring);
   else
   {
      cJSON *nq = cJSON_GetObjectItemCaseSensitive(resp, "projects_queued");
      printf("%d project(s) queued for ingest. Run `aimee kb ingest status` to monitor.\n",
             cJSON_IsNumber(nq) ? (int)nq->valuedouble : 0);
   }
}
void pt_print_kb_docs_push(const char *method, cJSON *resp)
{
   int total = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "total"));
   int uploaded = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "uploaded"));
   int skipped = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "skipped"));
   int failed = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "failed"));
   printf("Docs push complete: %d uploaded, %d skipped, %d failed (%d total).\n", uploaded, skipped,
          failed, total);
}
void pt_print_kb_ingest_status(const char *method, cJSON *resp)
{
   cJSON *queue = cJSON_GetObjectItemCaseSensitive(resp, "queue");
   cJSON *workers_obj = cJSON_GetObjectItemCaseSensitive(resp, "workers");
   cJSON *recent = cJSON_GetObjectItemCaseSensitive(resp, "recent");

   int pending = 0, running = 0, done24 = 0, failed24 = 0, configured = 0;
   if (cJSON_IsObject(queue))
   {
      pending = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(queue, "pending"));
      running = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(queue, "running"));
      done24 = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(queue, "done_last_24h"));
      failed24 =
          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(queue, "failed_last_24h"));
   }
   cJSON *slots = NULL;
   if (cJSON_IsObject(workers_obj))
   {
      configured =
          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(workers_obj, "configured"));
      slots = cJSON_GetObjectItemCaseSensitive(workers_obj, "slots");
   }

   if (cJSON_IsArray(recent) && cJSON_GetArraySize(recent) > 0)
   {
      printf("Jobs:\n");
      cJSON *item;
      cJSON_ArrayForEach(item, recent)
      {
         cJSON *pn = cJSON_GetObjectItemCaseSensitive(item, "project");
         cJSON *st = cJSON_GetObjectItemCaseSensitive(item, "status");
         cJSON *at = cJSON_GetObjectItemCaseSensitive(item, "completed_at");
         cJSON *fi = cJSON_GetObjectItemCaseSensitive(item, "files_indexed");
         cJSON *ca = cJSON_GetObjectItemCaseSensitive(item, "chunks_added");
         cJSON *er = cJSON_GetObjectItemCaseSensitive(item, "error");
         const char *status_str = cJSON_IsString(st) ? st->valuestring : "?";
         if (cJSON_IsString(er) && er->valuestring[0])
            printf("  %-24s  %-6s  %s  ERROR: %s\n", cJSON_IsString(pn) ? pn->valuestring : "?",
                   status_str, cJSON_IsString(at) ? at->valuestring : "", er->valuestring);
         else
            printf("  %-24s  %-6s  %s  %d files  %d chunks\n",
                   cJSON_IsString(pn) ? pn->valuestring : "?", status_str,
                   cJSON_IsString(at) ? at->valuestring : "",
                   cJSON_IsNumber(fi) ? (int)fi->valuedouble : 0,
                   cJSON_IsNumber(ca) ? (int)ca->valuedouble : 0);
      }
      printf("\n");
   }
   printf("Queue:    %d pending  %d running  %d done (24h)  %d failed (24h)\n", pending, running,
          done24, failed24);
   printf("Workers:  %d configured\n", configured);
   if (cJSON_IsArray(slots))
   {
      cJSON *slot;
      cJSON_ArrayForEach(slot, slots)
      {
         int idx = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(slot, "index"));
         int active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(slot, "active"));
         if (active)
         {
            const char *proj =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "project"));
            const char *phase =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "phase"));
            int elapsed =
                (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(slot, "elapsed_secs"));
            printf("  [%d] %-24s  %-10s  %ds\n", idx, proj ? proj : "?", phase ? phase : "?",
                   elapsed);
         }
         else
         {
            printf("  [%d] idle\n", idx);
         }
      }
   }
}
void pt_print_kb_status(const char *method, cJSON *resp)
{
   cJSON *proj = cJSON_GetObjectItemCaseSensitive(resp, "project");
   cJSON *chunks = cJSON_GetObjectItemCaseSensitive(resp, "chunks");
   cJSON *vpts = cJSON_GetObjectItemCaseSensitive(resp, "vector_points");
   if (cJSON_IsString(proj))
      printf("project: %s\n", proj->valuestring);
   if (cJSON_IsNumber(chunks))
      printf("chunks:        %d\n", (int)chunks->valuedouble);
   if (cJSON_IsNumber(vpts))
      printf("vector points: %d\n", (int)vpts->valuedouble);
   cJSON *iq = cJSON_GetObjectItemCaseSensitive(resp, "ingest_queue");
   if (cJSON_IsObject(iq))
   {
      cJSON *pending = cJSON_GetObjectItemCaseSensitive(iq, "pending");
      cJSON *running = cJSON_GetObjectItemCaseSensitive(iq, "running");
      cJSON *done24 = cJSON_GetObjectItemCaseSensitive(iq, "done_last_24h");
      int n_pending = cJSON_IsNumber(pending) ? (int)pending->valuedouble : 0;
      int n_running = cJSON_IsNumber(running) ? (int)running->valuedouble : 0;
      int n_done24 = cJSON_IsNumber(done24) ? (int)done24->valuedouble : 0;
      printf("Background ingest: %d pending", n_pending + n_running);
      if (n_running > 0)
         printf(" (%d running)", n_running);
      printf(", %d done last 24h\n", n_done24);
   }
}
void pt_print_audit(const char *method, cJSON *resp)
{
   if (strstr(method, "checkpoint"))
   {
      int ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "checkpointed"));
      printf("audit checkpoint: %s\n", ok ? "ok" : "failed");
      return;
   }
   if (strstr(method, "seal"))
   {
      int ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "sealed"));
      if (!ok)
      {
         printf("audit seal: failed\n");
         return;
      }
      int imm = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "immutable"));
      printf("audit seal: ok — %s (%s)\n", json_str(resp, "path") ? json_str(resp, "path") : "(?)",
             imm ? "OS-immutable" : "crypto-only (no CAP_LINUX_IMMUTABLE)");
      return;
   }
   const char *v = json_str(resp, "verify");
   long head = json_int(resp, "head_seq", 0);
   long ck = json_int(resp, "last_checkpoint_seq", 0);
   long un = json_int(resp, "unattested", 0);
   if (v && strcmp(v, "green") == 0)
      printf("audit verify: GREEN — chain + checkpoint MACs intact; head seq %ld attested by "
             "checkpoint seq %ld\n",
             head, ck);
   else if (v && strcmp(v, "amber") == 0)
      printf("audit verify: AMBER — chain intact; %ld row(s) after checkpoint seq %ld are "
             "unattested; run 'aimee audit checkpoint'\n",
             un, ck);
   else
   {
      const char *d = json_str(resp, "detail");
      printf("audit verify: RED — %s\n", d ? d : "integrity break");
   }
}

void pt_print_workers(const char *method, cJSON *resp)
{
   cJSON *server_bg_obj = cJSON_GetObjectItemCaseSensitive(resp, "server_background");
   cJSON *request_obj = cJSON_GetObjectItemCaseSensitive(resp, "request");
   cJSON *compute_obj = cJSON_GetObjectItemCaseSensitive(resp, "compute");
   cJSON *kb_obj = cJSON_GetObjectItemCaseSensitive(resp, "kb");
   int ingest_cap =
       (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "ingest_max_concurrent"));

   if (cJSON_IsObject(server_bg_obj))
   {
      printf("aimee-server  background threads:\n");
      cJSON *threads = cJSON_GetObjectItemCaseSensitive(server_bg_obj, "threads");
      if (cJSON_IsArray(threads))
      {
         cJSON *thread;
         cJSON_ArrayForEach(thread, threads)
         {
            const char *name =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(thread, "name"));
            const char *state =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(thread, "state"));
            int active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(thread, "active"));
            cJSON *in_flight = cJSON_GetObjectItemCaseSensitive(thread, "in_flight");
            cJSON *max_concurrent = cJSON_GetObjectItemCaseSensitive(thread, "max_concurrent");
            if (cJSON_IsNumber(in_flight) || cJSON_IsNumber(max_concurrent))
               printf("  %-18s %-8s %-10s %d/%d in-flight\n", name ? name : "?",
                      active ? "active" : "idle", state ? state : "-",
                      cJSON_IsNumber(in_flight) ? (int)in_flight->valuedouble : 0,
                      cJSON_IsNumber(max_concurrent) ? (int)max_concurrent->valuedouble : 0);
            else
               printf("  %-18s %-8s %s\n", name ? name : "?", active ? "active" : "idle",
                      state ? state : "-");
         }
      }
   }

   if (cJSON_IsObject(request_obj))
   {
      int cfg =
          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(request_obj, "configured"));
      printf("aimee-server  request threads: %d configured\n", cfg);
      cJSON *slots = cJSON_GetObjectItemCaseSensitive(request_obj, "slots");
      if (cJSON_IsArray(slots))
      {
         cJSON *slot;
         cJSON_ArrayForEach(slot, slots)
         {
            int idx = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(slot, "index"));
            int active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(slot, "active"));
            if (active)
            {
               const char *desc =
                   cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "descriptor"));
               int elapsed = (int)cJSON_GetNumberValue(
                   cJSON_GetObjectItemCaseSensitive(slot, "elapsed_secs"));
               printf("  [%d] %-40s  %ds\n", idx, desc && desc[0] ? desc : "-", elapsed);
            }
            else
            {
               printf("  [%d] idle\n", idx);
            }
         }
      }
   }

   if (cJSON_IsObject(compute_obj))
   {
      int cfg =
          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(compute_obj, "configured"));
      if (ingest_cap > 0)
         printf("aimee-server  compute pool:   %d configured (ingest cap %d)\n", cfg, ingest_cap);
      else
         printf("aimee-server  compute pool:   %d configured\n", cfg);
      cJSON *slots = cJSON_GetObjectItemCaseSensitive(compute_obj, "slots");
      if (cJSON_IsArray(slots))
      {
         cJSON *slot;
         cJSON_ArrayForEach(slot, slots)
         {
            int idx = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(slot, "index"));
            int active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(slot, "active"));
            if (active)
            {
               const char *kind =
                   cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "kind"));
               const char *desc =
                   cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "descriptor"));
               int elapsed = (int)cJSON_GetNumberValue(
                   cJSON_GetObjectItemCaseSensitive(slot, "elapsed_secs"));
               printf("  [%d] %-10s  %-40s  %ds\n", idx, kind ? kind : "?",
                      desc && desc[0] ? desc : "-", elapsed);
            }
            else
            {
               printf("  [%d] idle\n", idx);
            }
         }
      }
   }

   cJSON *async_obj = cJSON_GetObjectItemCaseSensitive(resp, "async");
   if (cJSON_IsObject(async_obj))
   {
      int chat_active =
          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(async_obj, "chat_active"));
      int chat_limit =
          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(async_obj, "chat_limit"));
      int tool_active =
          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(async_obj, "tool_active"));
      int tool_limit =
          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(async_obj, "tool_limit"));
      if (chat_limit > 0)
         printf("aimee-server  async lanes:    chat %d/%d active, tool %d/%d active\n", chat_active,
                chat_limit, tool_active, tool_limit);
      else
         printf("aimee-server  async lanes:    chat %d active (uncapped), tool %d/%d active\n",
                chat_active, tool_active, tool_limit);
      cJSON *slots = cJSON_GetObjectItemCaseSensitive(async_obj, "slots");
      if (cJSON_IsArray(slots))
      {
         cJSON *slot;
         cJSON_ArrayForEach(slot, slots)
         {
            const char *lane = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "lane"));
            const char *sid =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "session_id"));
            const char *psid =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "provider_session_id"));
            const char *desc =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "descriptor"));
            int elapsed =
                (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(slot, "elapsed_secs"));
            if (psid && psid[0])
               printf("  %-5s session=%-18s provider=%-18s %-24s %ds\n", lane ? lane : "?",
                      sid ? sid : "?", psid, desc && desc[0] ? desc : "-", elapsed);
            else
               printf("  %-5s session=%-18s %-44s %ds\n", lane ? lane : "?", sid ? sid : "?",
                      desc && desc[0] ? desc : "-", elapsed);
         }
      }
   }

   cJSON *secondary = cJSON_GetObjectItemCaseSensitive(resp, "secondary_pools");
   if (cJSON_IsArray(secondary))
   {
      cJSON *pool_entry;
      cJSON_ArrayForEach(pool_entry, secondary)
      {
         const char *name =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(pool_entry, "name"));
         int cfg =
             (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(pool_entry, "configured"));
         printf("aimee-server  %s pool:%*s%d configured (ephemeral)\n", name ? name : "?",
                (int)(11 - strlen(name ? name : "?")), "", cfg);
         cJSON *slots = cJSON_GetObjectItemCaseSensitive(pool_entry, "slots");
         if (cJSON_IsArray(slots))
         {
            cJSON *slot;
            cJSON_ArrayForEach(slot, slots)
            {
               int idx = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(slot, "index"));
               int active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(slot, "active"));
               if (active)
               {
                  const char *kind =
                      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "kind"));
                  const char *desc =
                      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "descriptor"));
                  int elapsed = (int)cJSON_GetNumberValue(
                      cJSON_GetObjectItemCaseSensitive(slot, "elapsed_secs"));
                  printf("  [%d] %-10s  %-40s  %ds\n", idx, kind ? kind : "?",
                         desc && desc[0] ? desc : "-", elapsed);
               }
               else
               {
                  printf("  [%d] idle\n", idx);
               }
            }
         }
      }
   }

   if (cJSON_IsObject(kb_obj))
   {
      int cfg = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(kb_obj, "configured"));
      printf("aimee-kb      connection workers: %d configured\n", cfg);
      cJSON *threads = cJSON_GetObjectItemCaseSensitive(kb_obj, "threads");
      if (cJSON_IsArray(threads))
      {
         printf("aimee-kb      worker threads:\n");
         cJSON *thread;
         cJSON_ArrayForEach(thread, threads)
         {
            const char *name =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(thread, "name"));
            const char *state =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(thread, "state"));
            int active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(thread, "active"));
            printf("  %-22s %-8s %s\n", name ? name : "?", active ? "active" : "idle",
                   state ? state : "-");
         }
      }
      cJSON *slots = cJSON_GetObjectItemCaseSensitive(kb_obj, "slots");
      if (cJSON_IsArray(slots))
      {
         cJSON *slot;
         cJSON_ArrayForEach(slot, slots)
         {
            int idx = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(slot, "index"));
            int active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(slot, "active"));
            if (active)
            {
               const char *meth =
                   cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "method"));
               /* kb.workers is the introspection query itself — skip it */
               if (meth && strcmp(meth, "kb.workers") == 0)
                  continue;
               int elapsed = (int)cJSON_GetNumberValue(
                   cJSON_GetObjectItemCaseSensitive(slot, "elapsed_secs"));
               printf("  [%d] %-36s  %ds\n", idx, meth ? meth : "?", elapsed);
            }
            else
            {
               printf("  [%d] idle\n", idx);
            }
         }
      }
      /* aimee-kb autonomous tasks (curator drain, maintenance timer, etc.). */
      cJSON *bg = cJSON_GetObjectItemCaseSensitive(kb_obj, "background");
      if (cJSON_IsArray(bg) && cJSON_GetArraySize(bg) > 0)
      {
         printf("aimee-kb      autonomous tasks:\n");
         cJSON *slot;
         cJSON_ArrayForEach(slot, bg)
         {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "name"));
            const char *desc =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "descriptor"));
            int elapsed =
                (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(slot, "elapsed_secs"));
            printf("  %-12s  %-40s  %ds\n", name ? name : "?", desc && desc[0] ? desc : "-",
                   elapsed);
         }
      }
   }
}
void pt_print_provider_list(const char *method, cJSON *resp)
{
   cJSON *providers = cJSON_GetObjectItemCaseSensitive(resp, "providers");
   if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "json")))
   {
      char *json = cJSON_Print(providers);
      if (json)
      {
         printf("%s\n", json);
         free(json);
      }
   }
   else if (cJSON_IsArray(providers))
   {
      printf("%-20s  %-24s  %-10s  %s\n", "provider", "display", "auth", "status");
      cJSON *p;
      cJSON_ArrayForEach(p, providers)
      {
         const char *name = json_str(p, "name");
         const char *display = json_str(p, "display_name");
         const char *auth = json_str(p, "auth_type");
         const char *env_var = json_str(p, "env_var");
         int available = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "available"));
         printf("%-20s  %-24s  %-10s  %s\n", name, display, auth,
                env_var[0] ? (available ? "[key set]" : "[no key]") : "[local]");
      }
   }
}
void pt_print_provider_show(const char *method, cJSON *resp)
{
   cJSON *p = cJSON_GetObjectItemCaseSensitive(resp, "provider");
   if (cJSON_IsObject(p))
   {
      printf("name:          %s\n", json_str(p, "name"));
      printf("display_name:  %s\n", json_str(p, "display_name"));
      printf("description:   %s\n", json_str(p, "description"));
      printf("base_url:      %s\n", json_str(p, "base_url"));
      printf("models_url:    %s\n", json_str(p, "models_url"));
      printf("auth_type:     %s\n", json_str(p, "auth_type"));
      printf("default_model: %s\n", json_str(p, "default_model"));
      printf("aux_model:     %s\n", json_str(p, "default_aux_model"));
      cJSON *envs = cJSON_GetObjectItemCaseSensitive(p, "env_vars");
      if (cJSON_IsArray(envs))
      {
         cJSON *env;
         int idx = 0;
         cJSON_ArrayForEach(env, envs)
         {
            if (cJSON_IsString(env))
               printf("env[%d]:        %s\n", idx++, env->valuestring);
         }
      }
   }
}
void pt_print_provider_models(const char *method, cJSON *resp)
{
   cJSON *models = cJSON_GetObjectItemCaseSensitive(resp, "models");
   if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "json")))
   {
      char *json = cJSON_Print(models);
      if (json)
      {
         printf("%s\n", json);
         free(json);
      }
   }
   else if (cJSON_IsArray(models))
   {
      cJSON *m;
      cJSON_ArrayForEach(m, models)
      {
         if (cJSON_IsString(m))
            printf("%s\n", m->valuestring);
      }
      printf("%d models\n", cJSON_GetArraySize(models));
   }
}
void pt_print_provider_test(const char *method, cJSON *resp)
{
   const char *message = json_str(resp, "message");
   if (message[0])
      printf("%s\n", message);
}
void pt_print_provider_quota(const char *method, cJSON *resp)
{
   const char *quota = json_str(resp, "quota");
   if (quota[0])
      printf("%s", quota);
}
void pt_print_model_show(const char *method, cJSON *resp)
{
   cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "model");
   if (cJSON_IsObject(m))
   {
      printf("provider:       %s\n", json_str(m, "provider"));
      printf("model:          %s\n", json_str(m, "model"));
      printf("context_window: %.0f\n",
             cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(m, "context_window")));
      printf("max_output:     %.0f\n",
             cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(m, "max_output")));
      printf("cost_in/mtok:   %.6f\n",
             cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(m, "cost_in_per_mtok")));
      printf("cost_out/mtok:  %.6f\n",
             cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(m, "cost_out_per_mtok")));
      printf("capabilities:   %s\n", json_str(m, "capabilities"));
      printf("modalities:     %s\n", json_str(m, "modalities"));
      printf("cutoff:         %s\n", json_str(m, "knowledge_cutoff"));
      printf("open_weights:   %s\n",
             cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "open_weights")) ? "yes" : "no");
      printf("deprecated:     %s\n",
             cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "deprecated")) ? "yes" : "no");
   }
}
void pt_print_model_list(const char *method, cJSON *resp)
{
   cJSON *models = cJSON_GetObjectItemCaseSensitive(resp, "models");
   if (cJSON_IsArray(models))
   {
      printf("%-20s  %-14s  %-32s  %-8s  %s\n", "agent", "provider", "model", "context",
             "capabilities");
      cJSON *m;
      cJSON_ArrayForEach(m, models)
      {
         printf("%-20s  %-14s  %-32s  %-8.0f  %s\n", json_str(m, "agent"), json_str(m, "provider"),
                json_str(m, "model"),
                cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(m, "context_window")),
                json_str(m, "capabilities"));
      }
   }
}
void pt_print_provider_get(const char *method, cJSON *resp)
{
   cJSON *p = cJSON_GetObjectItemCaseSensitive(resp, "provider");
   if (cJSON_IsString(p))
      printf("%s\n", p->valuestring);
}
void pt_print_provider_set(const char *method, cJSON *resp)
{
   cJSON *p = cJSON_GetObjectItemCaseSensitive(resp, "provider");
   if (cJSON_IsString(p))
      printf("provider: %s\n", p->valuestring);
}
void pt_print_model_refresh(const char *method, cJSON *resp)
{
   const char *message = json_str(resp, "message");
   if (message[0])
      printf("%s\n", message);
}
void pt_print_dogfood_tag(const char *method, cJSON *resp)
{
   cJSON *id = cJSON_GetObjectItemCaseSensitive(resp, "record_id");
   printf("tagged %s\n", cJSON_IsString(id) ? id->valuestring : "(unknown)");
}
void pt_print_dogfood_report(const char *method, cJSON *resp)
{
   cJSON *month = cJSON_GetObjectItemCaseSensitive(resp, "month");
   cJSON *dir = cJSON_GetObjectItemCaseSensitive(resp, "log_dir");
   cJSON *rt = cJSON_GetObjectItemCaseSensitive(resp, "records_total");
   cJSON *rl = cJSON_GetObjectItemCaseSensitive(resp, "records_labelled");
   cJSON *ra = cJSON_GetObjectItemCaseSensitive(resp, "records_auto_labelled");
   cJSON *rz = cJSON_GetObjectItemCaseSensitive(resp, "records_retrieved_zero");
   printf("dogfood report for %s (%s)\n", cJSON_IsString(month) ? month->valuestring : "?",
          cJSON_IsString(dir) ? dir->valuestring : "?");
   printf("  records: %d (labelled: %d, auto-labelled: %d, retrieved-zero: %d)\n",
          cJSON_IsNumber(rt) ? rt->valueint : 0, cJSON_IsNumber(rl) ? rl->valueint : 0,
          cJSON_IsNumber(ra) ? ra->valueint : 0, cJSON_IsNumber(rz) ? rz->valueint : 0);
   cJSON *outcomes = cJSON_GetObjectItemCaseSensitive(resp, "outcomes");
   if (cJSON_IsObject(outcomes))
   {
      printf("  outcomes:\n");
      cJSON *entry;
      cJSON_ArrayForEach(entry, outcomes)
      {
         printf("    %-16s %d\n", entry->string, entry->valueint);
      }
   }
   cJSON *al = cJSON_GetObjectItemCaseSensitive(resp, "auto_labels");
   if (cJSON_IsObject(al) && cJSON_GetArraySize(al) > 0)
   {
      printf("  auto-labels:\n");
      cJSON *entry;
      cJSON_ArrayForEach(entry, al)
      {
         printf("    %-18s %d\n", entry->string, entry->valueint);
      }
   }
   cJSON *armed = cJSON_GetObjectItemCaseSensitive(resp, "review_reminder_armed");
   if (cJSON_IsTrue(armed))
      printf("  review reminder armed\n");
}
void pt_print_eval_run(const char *method, cJSON *resp)
{
   print_eval_run(resp);
}
void pt_print_eval_results(const char *method, cJSON *resp)
{
   print_eval_results(resp);
}
void pt_print_identity_show(const char *method, cJSON *resp)
{
   cJSON *charter = cJSON_GetObjectItemCaseSensitive(resp, "charter");
   cJSON *wp = cJSON_GetObjectItemCaseSensitive(resp, "working_profile");
   char *s = charter ? cJSON_Print(charter) : NULL;
   if (s)
   {
      printf("charter:\n%s\n", s);
      free(s);
   }
   s = wp ? cJSON_Print(wp) : NULL;
   if (s)
   {
      printf("working_profile:\n%s\n", s);
      free(s);
   }
}
void pt_print_api_status(const char *method, cJSON *resp)
{
   cJSON *report = cJSON_GetObjectItemCaseSensitive(resp, "report");
   if (cJSON_IsString(report) && report->valuestring[0])
   {
      fputs(report->valuestring, stdout);
      size_t len = strlen(report->valuestring);
      if (len == 0 || report->valuestring[len - 1] != '\n')
         fputc('\n', stdout);
   }
}
void pt_print_primary_set(const char *method, cJSON *resp)
{
   const char *agent = json_str(resp, "agent");
   printf("%s\n", (agent && agent[0]) ? agent : "(none — using default provider)");
}
void pt_print_identity_snapshot(const char *method, cJSON *resp)
{
   cJSON *path = cJSON_GetObjectItemCaseSensitive(resp, "path");
   printf("wrote %s\n", cJSON_IsString(path) ? path->valuestring : "(unknown)");
}
void pt_print_identity_diff(const char *method, cJSON *resp)
{
   cJSON *added = cJSON_GetObjectItemCaseSensitive(resp, "added");
   cJSON *removed = cJSON_GetObjectItemCaseSensitive(resp, "removed");
   cJSON *changed = cJSON_GetObjectItemCaseSensitive(resp, "changed");
   cJSON *flips = cJSON_GetObjectItemCaseSensitive(resp, "high_confidence_flips");
   printf("identity diff:\n");
   printf("  added:   %d\n", cJSON_IsArray(added) ? cJSON_GetArraySize(added) : 0);
   printf("  removed: %d\n", cJSON_IsArray(removed) ? cJSON_GetArraySize(removed) : 0);
   printf("  changed: %d\n", cJSON_IsArray(changed) ? cJSON_GetArraySize(changed) : 0);
   int nflips = cJSON_IsArray(flips) ? cJSON_GetArraySize(flips) : 0;
   if (nflips > 0)
      printf("  HIGH CONFIDENCE FLIPS: %d (investigate before continuing)\n", nflips);
}

/* delegate.roundtable: print the consolidated artifact in human mode (without
 * this, a non-JSON `aimee delegate roundtable` printed nothing — the result was
 * only reachable via --json or the run API). */
void pt_print_delegate_roundtable(const char *method, cJSON *resp)
{
   (void)method;
   cJSON *art = cJSON_GetObjectItemCaseSensitive(resp, "artifact");
   if (cJSON_IsString(art) && art->valuestring[0])
      printf("%s\n", art->valuestring);
   cJSON *rounds = cJSON_GetObjectItemCaseSensitive(resp, "rounds_run");
   cJSON *converged = cJSON_GetObjectItemCaseSensitive(resp, "converged");
   if (cJSON_IsNumber(rounds))
      fprintf(stderr, "[roundtable: %d round(s)%s]\n", (int)rounds->valuedouble,
              cJSON_IsTrue(converged) ? ", converged" : "");
}
