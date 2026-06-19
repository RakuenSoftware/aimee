/* test_cron_runtime.c: cron job runtime behavior with stubbed LLM/delivery. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "db1_cron_jobs.h"
#include "db1_trigger.h"
#include "pipelines.h"
#include "events.h"
#include "log.h"
#include "platform_test_util.h"
#include "../server_cron.h"

static char g_prior_output[16384];
static char g_last_hash[64];
static char g_last_output[16384];
static char g_output_a[16384];
static char g_output_b[16384];
static char g_last_status[32];
static char g_last_error[512];
static int g_last_silent;
static int g_last_delivered;
static int g_next_run_id = 1;
static int g_delivery_count;
static const char *g_agent_response = "ok";
static char g_agent_prompt[16384];
static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;

static void reset_state(void)
{
   g_prior_output[0] = '\0';
   g_last_hash[0] = '\0';
   g_last_output[0] = '\0';
   g_output_a[0] = '\0';
   g_output_b[0] = '\0';
   g_last_status[0] = '\0';
   g_last_error[0] = '\0';
   g_agent_prompt[0] = '\0';
   g_last_silent = 0;
   g_last_delivered = 0;
   g_delivery_count = 0;
   g_agent_response = "ok";
}

int db1_cron_job_upsert(const cron_job_t *job)
{
   (void)job;
   return 0;
}

int db1_cron_job_get(const char *job_id, cron_job_t *out)
{
   (void)job_id;
   (void)out;
   return -1;
}

int db1_cron_jobs_load(cron_job_t *out, int max, int enabled_only)
{
   (void)out;
   (void)max;
   (void)enabled_only;
   return 0;
}

int db1_cron_job_set_enabled(const char *job_id, int enabled)
{
   (void)job_id;
   (void)enabled;
   return 0;
}

int db1_cron_jobs_set_enabled_all(int enabled)
{
   (void)enabled;
   return 0;
}

int db1_cron_job_delete(const char *job_id)
{
   (void)job_id;
   return 0;
}

int db1_cron_job_record_run(const char *job_id, const char *status, int silent, int delivered,
                            const char *output, const char *error, const char *output_hash)
{
   (void)job_id;
   pthread_mutex_lock(&g_state_lock);
   snprintf(g_last_status, sizeof(g_last_status), "%s", status ? status : "");
   snprintf(g_last_output, sizeof(g_last_output), "%s", output ? output : "");
   if (strcmp(job_id, "cwd-a") == 0)
      snprintf(g_output_a, sizeof(g_output_a), "%s", output ? output : "");
   else if (strcmp(job_id, "cwd-b") == 0)
      snprintf(g_output_b, sizeof(g_output_b), "%s", output ? output : "");
   snprintf(g_last_error, sizeof(g_last_error), "%s", error ? error : "");
   snprintf(g_last_hash, sizeof(g_last_hash), "%s", output_hash ? output_hash : "");
   g_last_silent = silent ? 1 : 0;
   g_last_delivered = delivered ? 1 : 0;
   int run_id = g_next_run_id++;
   pthread_mutex_unlock(&g_state_lock);
   return run_id;
}

char *db1_cron_jobs_list_json(void)
{
   return strdup("[]");
}

char *db1_cron_job_history_json(const char *job_id, int limit)
{
   (void)job_id;
   (void)limit;
   return strdup("[]");
}

char *db1_cron_job_latest_output(const char *job_id)
{
   (void)job_id;
   return strdup(g_prior_output);
}

char *db1_cron_job_last_output_hash(const char *job_id)
{
   (void)job_id;
   return strdup(g_last_hash);
}

int agent_load_config(agent_config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));
   cfg->agent_count = 1;
   return 0;
}

int agent_run(agent_config_t *cfg, const char *role, const char *system_prompt,
              const char *user_prompt, int max_tokens, agent_result_t *out)
{
   (void)cfg;
   (void)role;
   (void)system_prompt;
   (void)max_tokens;
   memset(out, 0, sizeof(*out));
   snprintf(g_agent_prompt, sizeof(g_agent_prompt), "%s", user_prompt ? user_prompt : "");
   out->response = strdup(g_agent_response ? g_agent_response : "");
   out->success = 1;
   return 0;
}

int notify_deliver_target(const notify_target_t *target, const char *event_name,
                          const char *message)
{
   (void)target;
   (void)event_name;
   (void)message;
   g_delivery_count++;
   return 0;
}

int config_load(config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));
   return 0;
}

void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
{
   (void)level;
   (void)module;
   (void)fmt;
}

int platform_random_bytes(void *buf, size_t len)
{
   memset(buf, 0xAB, len);
   return 0;
}

int db1_trigger_insert(const char *id, const char *source, const char *event, const char *task,
                       const char *workspace, const char *metadata)
{
   (void)id;
   (void)source;
   (void)event;
   (void)task;
   (void)workspace;
   (void)metadata;
   return 0;
}

int db1_trigger_status_set(const char *id, const char *status, const char *pipeline_id,
                           const char *error)
{
   (void)id;
   (void)status;
   (void)pipeline_id;
   (void)error;
   return 0;
}

int db1_pipeline_create(const char *task, const char *request_classification,
                        const char *plan_depth, int *out_id)
{
   (void)task;
   (void)request_classification;
   (void)plan_depth;
   if (out_id)
      *out_id = 1;
   return 0;
}

int db1_pipeline_cancel(int pipeline_id)
{
   (void)pipeline_id;
   return 0;
}

int server_send_response(server_conn_t *conn, cJSON *resp)
{
   (void)conn;
   (void)resp;
   return 0;
}

int server_send_error(server_conn_t *conn, const char *message, const char *request_id)
{
   (void)conn;
   (void)message;
   (void)request_id;
   return -1;
}

static cron_job_t base_job(const char *mode)
{
   cron_job_t job;
   memset(&job, 0, sizeof(job));
   snprintf(job.id, sizeof(job.id), "pve-pulse");
   snprintf(job.schedule, sizeof(job.schedule), "every 10m");
   snprintf(job.mode, sizeof(job.mode), "%s", mode);
   snprintf(job.prompt, sizeof(job.prompt), "Check pve.");
   snprintf(job.deliver_target, sizeof(job.deliver_target), "local");
   job.enabled = 1;
   return job;
}

static void test_llm_silent_response_records_no_delivery(void)
{
   reset_state();
   cron_job_t job = base_job("llm");
   g_agent_response = "[SILENT]\n";

   cJSON *resp = NULL;
   assert(cron_run_config_job(&job, &resp) == 0);
   assert(resp != NULL);
   assert(strcmp(g_last_status, "complete") == 0);
   assert(g_last_silent == 1);
   assert(g_last_delivered == 0);
   assert(g_delivery_count == 0);
   assert(strcmp(g_last_output, "[SILENT]\n") == 0);
   cJSON_Delete(resp);
}

static void test_llm_only_if_changed_dedupes_final_output(void)
{
   reset_state();
   cron_job_t job = base_job("llm");
   job.deliver_only_if_changed = 1;
   g_agent_response = "pve unreachable";

   cJSON *resp = NULL;
   assert(cron_run_config_job(&job, &resp) == 0);
   cJSON_Delete(resp);
   assert(g_last_silent == 0);
   assert(g_last_delivered == 1);
   assert(g_delivery_count == 1);

   assert(cron_run_config_job(&job, &resp) == 0);
   cJSON_Delete(resp);
   assert(g_last_silent == 1);
   assert(g_last_delivered == 0);
   assert(g_delivery_count == 1);
}

static void test_context_from_injects_prior_output(void)
{
   reset_state();
   cron_job_t job = base_job("llm");
   snprintf(job.context_from, sizeof(job.context_from), "pve-pulse");
   snprintf(g_prior_output, sizeof(g_prior_output), "pve unreachable");
   g_agent_response = "diagnosed";

   cJSON *resp = NULL;
   assert(cron_run_config_job(&job, &resp) == 0);
   cJSON_Delete(resp);
   assert(strstr(g_agent_prompt, "PRIOR JOB OUTPUT:") != NULL);
   assert(strstr(g_agent_prompt, "pve unreachable") != NULL);
}

static void test_when_context_contains_skips_absent_substring(void)
{
   reset_state();
   cron_job_t job = base_job("llm");
   snprintf(job.context_from, sizeof(job.context_from), "pve-pulse");
   snprintf(job.when_context_contains, sizeof(job.when_context_contains), "pve unreachable");
   snprintf(g_prior_output, sizeof(g_prior_output), "OK");

   cJSON *resp = NULL;
   assert(cron_run_config_job(&job, &resp) == 0);
   cJSON_Delete(resp);
   assert(strcmp(g_last_status, "skipped") == 0);
   assert(g_last_silent == 1);
   assert(g_delivery_count == 0);
   assert(g_agent_prompt[0] == '\0');
}

static void test_wake_gate_false_skips_hybrid_llm(void)
{
   reset_state();
   cron_job_t job = base_job("hybrid");
   snprintf(job.script, sizeof(job.script), "echo '{\"wake\":false,\"reason\":\"no errors\"}'");
   job.pre_wake_gate = 1;

   cJSON *resp = NULL;
   assert(cron_run_config_job(&job, &resp) == 0);
   cJSON_Delete(resp);
   assert(strcmp(g_last_status, "skipped") == 0);
   assert(g_last_silent == 1);
   assert(g_delivery_count == 0);
   assert(g_agent_prompt[0] == '\0');
}

static void test_workdir_applies_to_script_and_restores_cwd(void)
{
   reset_state();
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-cron-runtime-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char old_cwd[1024];
   assert(getcwd(old_cwd, sizeof(old_cwd)) != NULL);

   cron_job_t job = base_job("script");
   snprintf(job.script, sizeof(job.script), "pwd");
   snprintf(job.workdir, sizeof(job.workdir), "%s", tmpdir);
   job.deliver_target[0] = '\0';

   cJSON *resp = NULL;
   assert(cron_run_config_job(&job, &resp) == 0);
   cJSON_Delete(resp);
   assert(strstr(g_last_output, tmpdir) != NULL);

   char now_cwd[1024];
   assert(getcwd(now_cwd, sizeof(now_cwd)) != NULL);
   assert(strcmp(old_cwd, now_cwd) == 0);

   platform_test_rmrf(tmpdir);
}

typedef struct
{
   cron_job_t job;
} cron_thread_arg_t;

static void *run_cron_thread(void *arg)
{
   cron_thread_arg_t *cta = (cron_thread_arg_t *)arg;
   cJSON *resp = NULL;
   assert(cron_run_config_job(&cta->job, &resp) == 0);
   cJSON_Delete(resp);
   return NULL;
}

static void test_parallel_workdir_jobs_are_serialized(void)
{
   reset_state();
   char dir_a[512];
   char dir_b[512];
   snprintf(dir_a, sizeof(dir_a), "%s/aimee-test-cron-runtime-a-XXXXXX", platform_tmpdir());
   snprintf(dir_b, sizeof(dir_b), "%s/aimee-test-cron-runtime-b-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(dir_a) != NULL);
   assert(platform_mkdtemp(dir_b) != NULL);

   char old_cwd[1024];
   assert(getcwd(old_cwd, sizeof(old_cwd)) != NULL);

   cron_thread_arg_t a;
   cron_thread_arg_t b;
   memset(&a, 0, sizeof(a));
   memset(&b, 0, sizeof(b));
   a.job = base_job("script");
   b.job = base_job("script");
   snprintf(a.job.id, sizeof(a.job.id), "cwd-a");
   snprintf(b.job.id, sizeof(b.job.id), "cwd-b");
   snprintf(a.job.script, sizeof(a.job.script), "pwd; sleep 0.2; pwd");
   snprintf(b.job.script, sizeof(b.job.script), "pwd; sleep 0.2; pwd");
   snprintf(a.job.workdir, sizeof(a.job.workdir), "%s", dir_a);
   snprintf(b.job.workdir, sizeof(b.job.workdir), "%s", dir_b);
   a.job.deliver_target[0] = '\0';
   b.job.deliver_target[0] = '\0';

   pthread_t ta;
   pthread_t tb;
   assert(pthread_create(&ta, NULL, run_cron_thread, &a) == 0);
   assert(pthread_create(&tb, NULL, run_cron_thread, &b) == 0);
   assert(pthread_join(ta, NULL) == 0);
   assert(pthread_join(tb, NULL) == 0);
   assert(strstr(g_output_a, dir_a) != NULL);
   assert(strstr(g_output_a, dir_b) == NULL);
   assert(strstr(g_output_b, dir_b) != NULL);
   assert(strstr(g_output_b, dir_a) == NULL);

   char now_cwd[1024];
   assert(getcwd(now_cwd, sizeof(now_cwd)) != NULL);
   assert(strcmp(old_cwd, now_cwd) == 0);

   platform_test_rmrf(dir_a);
   platform_test_rmrf(dir_b);
}

int main(void)
{
   test_llm_silent_response_records_no_delivery();
   test_llm_only_if_changed_dedupes_final_output();
   test_context_from_injects_prior_output();
   test_when_context_contains_skips_absent_substring();
   test_wake_gate_false_skips_hybrid_llm();
   test_workdir_applies_to_script_and_restores_cwd();
   test_parallel_workdir_jobs_are_serialized();
   printf("cron runtime tests passed\n");
   return 0;
}

const char *config_embedding_command(const config_t *cfg, const char *requested)
{
   if (requested && requested[0])
      return requested;
   if (cfg && cfg->embedding_command[0])
      return cfg->embedding_command;
   return "builtin";
}
