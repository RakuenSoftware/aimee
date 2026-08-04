/* server_cron.c: cron job RPC handlers and runtime dispatch. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "server_cron.h"

#include "agent_config.h"
#include "agent_exec.h"
#include "db1/db1_cron_jobs.h"
#include "events.h"
#include "trigger_scheduler.h"
#include "json_fluent.h" /* jo_ok */
#include "util.h"        /* run_cmd_set_cwd — thread-local cwd, never the process CWD */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CRON_SCRIPT_OUTPUT_CAP (64 * 1024)
#define CRON_LLM_MAX_TOKENS    2048

static pthread_mutex_t g_cron_workdir_lock = PTHREAD_MUTEX_INITIALIZER;

static void cron_sync_config_jobs(void)
{
   int n = config_cron_job_count();
   for (int i = 0; i < n && i < CRON_JOBS_MAX; i++)
   {
      cron_job_t job;
      if (config_cron_job_at(i, &job) == 0)
         (void)db1_cron_job_upsert(&job);
   }
}

static int find_cron_job(const char *id, cron_job_t *out)
{
   if (!id || !id[0] || !out)
      return -1;
   int n = config_cron_job_count();
   for (int i = 0; i < n && i < CRON_JOBS_MAX; i++)
   {
      cron_job_t job;
      if (config_cron_job_at(i, &job) == 0 && strcmp(job.id, id) == 0)
      {
         *out = job;
         return 0;
      }
   }
   return db1_cron_job_get(id, out);
}

static void cron_skills_csv(const cron_job_t *job, char *buf, size_t bufsz)
{
   if (!buf || bufsz == 0)
      return;
   buf[0] = '\0';
   if (!job)
      return;
   for (int i = 0; i < job->skill_count && i < CRON_JOB_MAX_SKILLS; i++)
   {
      if (!job->skills[i][0])
         continue;
      if (buf[0])
         strncat(buf, ",", bufsz - strlen(buf) - 1);
      strncat(buf, job->skills[i], bufsz - strlen(buf) - 1);
   }
}

static void cron_hash_text(const char *text, char *out, size_t out_len)
{
   unsigned long long h = 1469598103934665603ULL;
   const unsigned char *p = (const unsigned char *)(text ? text : "");
   while (*p)
   {
      h ^= (unsigned long long)*p++;
      h *= 1099511628211ULL;
   }
   snprintf(out, out_len, "%016llx", h);
}

static int cron_deliver_output(const cron_job_t *job, const char *status, const char *output,
                               const char *error)
{
   if (!job || !job->deliver_target[0])
      return 0;

   delivery_target_t target;
   if (delivery_target_parse(job->deliver_target, &target) != 0)
      return 0;
   if (delivery_target_is_origin(&target))
      return 0;

   char message[4096];
   if (error && error[0])
      snprintf(message, sizeof(message), "cron job %s %s: %s\n%s", job->id,
               status && status[0] ? status : "failed", error, output ? output : "");
   else
      snprintf(message, sizeof(message), "%s", output ? output : "");

   notify_target_t notify_target;
   memset(&notify_target, 0, sizeof(notify_target));
   notify_target.is_delivery = 1;
   notify_target.target = target;
   if (strcmp(target.platform, "ntfy") == 0)
      snprintf(notify_target.base_url, sizeof(notify_target.base_url), "%s", "https://ntfy.sh");

   char event_name[128];
   snprintf(event_name, sizeof(event_name), "cron:%s", job->id);
   return notify_deliver_target(&notify_target, event_name, message) == 0 ? 1 : 0;
}

static int cron_capture_script(const char *script, const char *workdir, char **out, int *exit_code)
{
   if (out)
      *out = NULL;
   if (exit_code)
      *exit_code = -1;
   if (!script || !script[0] || !out)
      return -1;

   char path[] = "/tmp/aimee-cron-XXXXXX";
   int fd = mkstemp(path);
   if (fd < 0)
      return -1;
   FILE *fp = fdopen(fd, "w");
   if (!fp)
   {
      close(fd);
      unlink(path);
      return -1;
   }
   fputs(script, fp);
   fputc('\n', fp);
   fclose(fp);

   /* The SHELL changes directory, not this process.
    *
    * popen() forks a shell that inherits the caller's CWD, which is why running a
    * job "in its workdir" used to mean chdir()ing aimee-server itself. Prefixing
    * the command is the same technique run_cmd() already uses for exactly this
    * reason (see tl_run_cwd in util.c) and it keeps the directory change inside
    * the child, where it cannot be observed by any other thread. */
   char cmd[MAX_PATH_LEN + 512];
   if (workdir && workdir[0])
      snprintf(cmd, sizeof(cmd), "cd '%s' && /bin/sh '%s' 2>&1", workdir, path);
   else
      snprintf(cmd, sizeof(cmd), "/bin/sh '%s' 2>&1", path);
   FILE *pipe = popen(cmd, "r");
   if (!pipe)
   {
      unlink(path);
      return -1;
   }

   size_t cap = 4096;
   size_t len = 0;
   char *buf = malloc(cap);
   if (!buf)
   {
      pclose(pipe);
      return -1;
   }
   for (;;)
   {
      if (len + 1024 >= cap)
      {
         size_t next = cap * 2;
         if (next > CRON_SCRIPT_OUTPUT_CAP + 1)
            next = CRON_SCRIPT_OUTPUT_CAP + 1;
         if (next <= cap)
            break;
         char *nb = realloc(buf, next);
         if (!nb)
            break;
         buf = nb;
         cap = next;
      }
      size_t n = fread(buf + len, 1, cap - len - 1, pipe);
      len += n;
      if (n == 0)
      {
         if (feof(pipe) || ferror(pipe))
            break;
      }
      if (len >= CRON_SCRIPT_OUTPUT_CAP)
         break;
   }
   buf[len] = '\0';
   int rc = pclose(pipe);
   unlink(path);
   if (exit_code)
      *exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
   *out = buf;
   return 0;
}

static int cron_run_llm_prompt(const char *prompt, char **out, char *err, size_t err_len)
{
   if (out)
      *out = NULL;
   if (!prompt || !prompt[0] || !out)
      return -1;

   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0 || acfg.agent_count == 0)
   {
      snprintf(err, err_len, "failed to load agent config");
      return -1;
   }

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = agent_run(&acfg, "execute", NULL, prompt, CRON_LLM_MAX_TOKENS, &result);
   if (rc != 0)
   {
      snprintf(err, err_len, "%s", result.error[0] ? result.error : "cron LLM turn failed");
      free(result.response);
      return -1;
   }
   *out = result.response ? result.response : strdup("");
   return 0;
}

static int cron_run_config_job_at_cwd(const cron_job_t *job, cJSON **out_resp)
{
   if (out_resp)
      *out_resp = NULL;
   if (!job || !job->id[0])
      return -1;
   (void)db1_cron_job_upsert(job);

   char *prior = NULL;
   if (job->context_from[0])
      prior = db1_cron_job_latest_output(job->context_from);
   if (!cron_when_context_contains_allows(prior, job->when_context_contains))
   {
      int run_id = db1_cron_job_record_run(job->id, "skipped", 1, 0, "[SILENT]",
                                           "when_context_contains did not match", "");
      cJSON *resp = jo_ok();
      cJSON_AddNumberToObject(resp, "run_id", run_id);
      cJSON_AddStringToObject(resp, "run_status", "skipped");
      cJSON_AddBoolToObject(resp, "silent", 1);
      if (out_resp)
         *out_resp = resp;
      else
         cJSON_Delete(resp);
      free(prior);
      return run_id > 0 ? 0 : -1;
   }

   char *script_output = NULL;
   char error[512] = "";
   int exit_code = 0;
   int is_script = strcmp(job->mode, "script") == 0;
   int is_hybrid = strcmp(job->mode, "hybrid") == 0;
   if ((is_script || is_hybrid) && job->script[0])
   {
      if (cron_capture_script(job->script, job->workdir, &script_output, &exit_code) != 0)
         snprintf(error, sizeof(error), "failed to execute script");
      else if (exit_code != 0)
         snprintf(error, sizeof(error), "script exited %d", exit_code);
   }

   if (is_hybrid && job->pre_wake_gate)
   {
      char reason[256];
      if (!cron_wake_gate_should_wake(script_output, reason, sizeof(reason)))
      {
         int run_id = db1_cron_job_record_run(job->id, "skipped", 1, 0,
                                              script_output ? script_output : "[SILENT]",
                                              reason[0] ? reason : "wake gate suppressed LLM", "");
         cJSON *resp = jo_ok();
         cJSON_AddNumberToObject(resp, "run_id", run_id);
         cJSON_AddStringToObject(resp, "run_status", "skipped");
         cJSON_AddBoolToObject(resp, "silent", 1);
         if (out_resp)
            *out_resp = resp;
         else
            cJSON_Delete(resp);
         free(script_output);
         free(prior);
         return run_id > 0 ? 0 : -1;
      }
   }

   const char *output = script_output ? script_output : "";
   char output_hash[32];
   output_hash[0] = '\0';

   char prompt[12288];
   char skills_csv[CRON_JOB_MAX_SKILLS * (CRON_JOB_MAX_SKILL_NAME + 1)];
   char *llm_output = NULL;
   cron_skills_csv(job, skills_csv, sizeof(skills_csv));

   const char *status = error[0] ? "failed" : "complete";
   if (!is_script && !error[0])
   {
      int n = cron_build_job_prompt(prompt, sizeof(prompt), job->prompt, job->workdir, skills_csv,
                                    prior, script_output, job->deliver_target);
      if (n < 0 || n >= (int)sizeof(prompt) ||
          cron_run_llm_prompt(prompt, &llm_output, error, sizeof(error)) != 0)
      {
         if (!error[0])
            snprintf(error, sizeof(error), "failed to run cron agent prompt");
         status = "failed";
      }
      else
      {
         output = llm_output ? llm_output : "";
         cron_hash_text(output, output_hash, sizeof(output_hash));
         status = "complete";
      }
   }

   cron_hash_text(output, output_hash, sizeof(output_hash));
   int silent = cron_response_is_silent(output);
   char *last_hash = db1_cron_job_last_output_hash(job->id);
   if (job->deliver_first_run_silent && (!last_hash || !last_hash[0]))
      silent = 1;
   if (job->deliver_only_if_changed && last_hash && last_hash[0] &&
       strcmp(last_hash, output_hash) == 0)
      silent = 1;
   free(last_hash);

   int delivered = 0;
   if (!silent && strcmp(status, "queued") != 0)
      delivered = cron_deliver_output(job, status, output, error);

   int run_id =
       db1_cron_job_record_run(job->id, status, silent, delivered, output, error, output_hash);
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", run_id > 0 ? "ok" : "error");
   cJSON_AddStringToObject(resp, "job_id", job->id);
   cJSON_AddNumberToObject(resp, "run_id", run_id);
   cJSON_AddStringToObject(resp, "run_status", status);
   cJSON_AddBoolToObject(resp, "silent", silent);
   cJSON_AddBoolToObject(resp, "delivered", delivered);
   cJSON_AddStringToObject(resp, "output", output);
   if (error[0])
      cJSON_AddStringToObject(resp, "error", error);

   if (out_resp)
      *out_resp = resp;
   else
      cJSON_Delete(resp);
   free(llm_output);
   free(script_output);
   free(prior);
   return run_id > 0 ? 0 : -1;
}

int cron_run_config_job(const cron_job_t *job, cJSON **out_resp)
{
   if (out_resp)
      *out_resp = NULL;
   if (!job || !job->id[0])
      return -1;

   if (!job->workdir[0])
      return cron_run_config_job_at_cwd(job, out_resp);

   /* NO chdir(). THE PROCESS CWD IS SHARED WITH EVERY OTHER SESSION.
    *
    * This used to getcwd(), chdir() into the job's workdir, run the job, and
    * chdir() back, serialized by g_cron_workdir_lock. That lock keeps cron jobs
    * from tripping over each other, and does nothing at all for the rest of the
    * thread pool: for the whole duration of a job, every other thread in
    * aimee-server sees a different current directory.
    *
    * Anything that resolves a path from the process CWD therefore reads state a
    * cron job can move. `aimee git verify` resolved its target that way and could
    * verify -- and PASS -- a repository the caller never named, whenever a cron
    * job with a workdir happened to be running. That is silent, it is timing
    * dependent, and on a box where several sessions share a repo it is exactly the
    * cwd clobbering this codebase already worked hard to avoid elsewhere.
    *
    * The mechanism to avoid it already exists and is used everywhere else:
    * run_cmd_set_cwd() keeps the directory in a __thread variable and prefixes
    * "cd '<dir>' && " onto the command, so the CHILD changes directory and no
    * other thread can observe it. cron_capture_script does the same for its
    * popen(). Between them every execution path a job actually uses is covered,
    * and the process CWD is left alone.
    *
    * The lock stays. It no longer guards the CWD, but it preserved job-to-job
    * serialization from the day it was written, and changing that is a separate
    * decision from fixing the shared-state bug. */
   pthread_mutex_lock(&g_cron_workdir_lock);

   int rc = 0;
   /* Preserves the previous failure mode: a workdir that cannot be entered is a
    * job failure with the same message, reported before anything runs. chdir()
    * used to be what discovered that. */
   if (access(job->workdir, X_OK) != 0)
   {
      (void)db1_cron_job_upsert(job);
      char error[512];
      snprintf(error, sizeof(error), "failed to enter workdir: %s", job->workdir);
      int delivered = cron_deliver_output(job, "failed", "", error);
      int run_id = db1_cron_job_record_run(job->id, "failed", 0, delivered, "", error, "");
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "status", run_id > 0 ? "ok" : "error");
      cJSON_AddStringToObject(resp, "job_id", job->id);
      cJSON_AddNumberToObject(resp, "run_id", run_id);
      cJSON_AddStringToObject(resp, "run_status", "failed");
      cJSON_AddBoolToObject(resp, "silent", 0);
      cJSON_AddBoolToObject(resp, "delivered", delivered);
      cJSON_AddStringToObject(resp, "output", "");
      cJSON_AddStringToObject(resp, "error", error);
      if (out_resp)
         *out_resp = resp;
      else
         cJSON_Delete(resp);
      rc = -1;
   }
   else
   {
      /* Thread-local, so concurrent threads keep their own view. Covers every
       * run_cmd()-based callee (agent_run and below); cron_capture_script's
       * popen() carries the workdir itself. */
      run_cmd_set_cwd(job->workdir);
      rc = cron_run_config_job_at_cwd(job, out_resp);
      run_cmd_set_cwd(NULL);
   }

   pthread_mutex_unlock(&g_cron_workdir_lock);
   return rc;
}

int handle_cron_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   cron_sync_config_jobs();

   char *json = db1_cron_jobs_list_json();
   if (!json)
      return server_send_error(conn, "failed to list cron jobs", NULL);
   cJSON *jobs = cJSON_Parse(json);
   free(json);
   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "jobs", jobs ? jobs : cJSON_CreateArray());
   return server_send_ok(conn, resp);
}

static int cron_req_bool(cJSON *req, const char *name, int def)
{
   cJSON *item = cJSON_GetObjectItemCaseSensitive(req, name);
   if (!item)
      return def;
   return cJSON_IsTrue(item) ? 1 : 0;
}

static void cron_req_string(cJSON *req, const char *name, char *dst, size_t dst_len)
{
   if (!dst || dst_len == 0)
      return;
   cJSON *item = cJSON_GetObjectItemCaseSensitive(req, name);
   if (cJSON_IsString(item) && item->valuestring)
      snprintf(dst, dst_len, "%s", item->valuestring);
}

static int cron_req_job(cJSON *req, cron_job_t *job, char *err, size_t err_len)
{
   if (!req || !job)
      return -1;
   memset(job, 0, sizeof(*job));
   snprintf(job->mode, sizeof(job->mode), "%s", "llm");
   job->enabled = cron_req_bool(req, "enabled", 1);

   cron_req_string(req, "job_id", job->id, sizeof(job->id));
   if (!job->id[0])
      cron_req_string(req, "id", job->id, sizeof(job->id));
   cron_req_string(req, "schedule", job->schedule, sizeof(job->schedule));
   cron_req_string(req, "mode", job->mode, sizeof(job->mode));
   cron_req_string(req, "script", job->script, sizeof(job->script));
   cron_req_string(req, "prompt", job->prompt, sizeof(job->prompt));
   cron_req_string(req, "workdir", job->workdir, sizeof(job->workdir));
   cron_req_string(req, "deliver_target", job->deliver_target, sizeof(job->deliver_target));
   cron_req_string(req, "context_from", job->context_from, sizeof(job->context_from));
   cron_req_string(req, "when_context_contains", job->when_context_contains,
                   sizeof(job->when_context_contains));
   job->deliver_only_if_changed = cron_req_bool(req, "deliver_only_if_changed", 0);
   job->deliver_first_run_silent = cron_req_bool(req, "deliver_first_run_silent", 0);
   job->pre_wake_gate = cron_req_bool(req, "pre_wake_gate", 0);

   cJSON *skills = cJSON_GetObjectItemCaseSensitive(req, "skills");
   if (cJSON_IsArray(skills))
   {
      cJSON *skill;
      cJSON_ArrayForEach(skill, skills)
      {
         if (job->skill_count >= CRON_JOB_MAX_SKILLS)
            break;
         if (cJSON_IsString(skill) && skill->valuestring && skill->valuestring[0])
         {
            snprintf(job->skills[job->skill_count], sizeof(job->skills[job->skill_count]), "%s",
                     skill->valuestring);
            job->skill_count++;
         }
      }
   }

   if (!job->id[0] || !job->schedule[0])
   {
      snprintf(err, err_len, "job_id and schedule are required");
      return -1;
   }
   if (strcmp(job->mode, "llm") != 0 && strcmp(job->mode, "script") != 0 &&
       strcmp(job->mode, "hybrid") != 0)
   {
      snprintf(err, err_len, "mode must be llm, script, or hybrid");
      return -1;
   }
   if ((strcmp(job->mode, "script") == 0 || strcmp(job->mode, "hybrid") == 0) && !job->script[0])
   {
      snprintf(err, err_len, "script is required for script and hybrid cron jobs");
      return -1;
   }
   if ((strcmp(job->mode, "llm") == 0 || strcmp(job->mode, "hybrid") == 0) && !job->prompt[0])
   {
      snprintf(err, err_len, "prompt is required for llm and hybrid cron jobs");
      return -1;
   }
   return 0;
}

int handle_cron_add(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cron_job_t job;
   char err[256] = "";
   if (cron_req_job(req, &job, err, sizeof(err)) != 0)
      return server_send_error(conn, err[0] ? err : "invalid cron job", NULL);
   if (db1_cron_job_upsert(&job) != 0)
      return server_send_error(conn, "failed to save cron job", NULL);

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "job_id", job.id);
   cJSON_AddBoolToObject(resp, "enabled", job.enabled);
   return server_send_ok(conn, resp);
}

int handle_cron_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "job_id");
   if (!cJSON_IsString(jid) || !jid->valuestring[0])
      return server_send_error(conn, "job_id is required", NULL);
   cron_sync_config_jobs();
   cron_job_t job_buf;
   if (find_cron_job(jid->valuestring, &job_buf) != 0)
      return server_send_error(conn, "cron job not found", NULL);
   const cron_job_t *job = &job_buf;

   char skills_csv[CRON_JOB_MAX_SKILLS * (CRON_JOB_MAX_SKILL_NAME + 1)];
   cron_skills_csv(job, skills_csv, sizeof(skills_csv));
   cJSON *resp = jo_ok();
   cJSON *obj = cJSON_AddObjectToObject(resp, "job");
   cJSON_AddStringToObject(obj, "id", job->id);
   cJSON_AddStringToObject(obj, "schedule", job->schedule);
   cJSON_AddStringToObject(obj, "mode", job->mode);
   cJSON_AddStringToObject(obj, "workdir", job->workdir);
   cJSON_AddStringToObject(obj, "deliver_target", job->deliver_target);
   cJSON_AddStringToObject(obj, "skills_csv", skills_csv);
   cJSON_AddBoolToObject(obj, "enabled", job->enabled);
   cJSON_AddBoolToObject(obj, "pre_wake_gate", job->pre_wake_gate);
   cJSON_AddStringToObject(obj, "context_from", job->context_from);
   cJSON_AddStringToObject(obj, "when_context_contains", job->when_context_contains);
   return server_send_ok(conn, resp);
}

int handle_cron_history(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "job_id");
   if (!cJSON_IsString(jid) || !jid->valuestring[0])
      return server_send_error(conn, "job_id is required", NULL);
   cJSON *jlimit = cJSON_GetObjectItemCaseSensitive(req, "limit");
   int limit = cJSON_IsNumber(jlimit) ? jlimit->valueint : 20;

   char *json = db1_cron_job_history_json(jid->valuestring, limit);
   if (!json)
      return server_send_error(conn, "failed to read cron history", NULL);
   cJSON *runs = cJSON_Parse(json);
   free(json);
   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "job_id", jid->valuestring);
   cJSON_AddItemToObject(resp, "runs", runs ? runs : cJSON_CreateArray());
   return server_send_ok(conn, resp);
}

int handle_cron_run(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "job_id");
   if (!cJSON_IsString(jid) || !jid->valuestring[0])
      return server_send_error(conn, "job_id is required", NULL);
   cron_sync_config_jobs();
   cron_job_t job_buf;
   if (find_cron_job(jid->valuestring, &job_buf) != 0)
      return server_send_error(conn, "cron job not found", NULL);
   const cron_job_t *job = &job_buf;

   cJSON *resp = NULL;
   if (cron_run_config_job(job, &resp) != 0)
   {
      if (resp)
      {
         return server_send_ok(conn, resp);
      }
      return server_send_error(conn, "cron run failed", NULL);
   }
   return server_send_ok(conn, resp);
}

static int handle_cron_set_enabled(server_ctx_t *ctx, server_conn_t *conn, cJSON *req, int enabled)
{
   (void)ctx;
   cJSON *all = cJSON_GetObjectItemCaseSensitive(req, "all");
   if (cJSON_IsTrue(all))
   {
      if (db1_cron_jobs_set_enabled_all(enabled) != 0)
         return server_send_error(conn, "failed to update cron jobs", NULL);
      cJSON *resp = jo_ok();
      cJSON_AddStringToObject(resp, "job_id", "--all");
      cJSON_AddBoolToObject(resp, "all", 1);
      cJSON_AddBoolToObject(resp, "enabled", enabled);
      return server_send_ok(conn, resp);
   }

   cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "job_id");
   if (!cJSON_IsString(jid) || !jid->valuestring[0])
      return server_send_error(conn, "job_id is required", NULL);
   if (db1_cron_job_set_enabled(jid->valuestring, enabled) != 0)
      return server_send_error(conn, "cron job not found", NULL);
   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "job_id", jid->valuestring);
   cJSON_AddBoolToObject(resp, "enabled", enabled);
   return server_send_ok(conn, resp);
}

int handle_cron_enable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return handle_cron_set_enabled(ctx, conn, req, 1);
}

int handle_cron_disable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return handle_cron_set_enabled(ctx, conn, req, 0);
}

int handle_cron_remove(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "job_id");
   if (!cJSON_IsString(jid) || !jid->valuestring[0])
      return server_send_error(conn, "job_id is required", NULL);
   if (db1_cron_job_delete(jid->valuestring) != 0)
      return server_send_error(conn, "cron job not found", NULL);
   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "job_id", jid->valuestring);
   return server_send_ok(conn, resp);
}
