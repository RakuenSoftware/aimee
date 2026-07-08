#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aimee.h"
#include "db1.h"
#include "server.h"
#include "cJSON.h"

#include <sys/stat.h> /* mkdir */
/* agent_job_to_json() serializes default_max_turns = delegate_default_max_turns_for_role
 * (role_template_max_turns), which reads the role_templates dir under
 * config_default_dir() (-1 when absent). The handler tests assert default_max_turns==20
 * for a review job, so point config_default_dir at a temp dir and lay down the template. */
static char g_roles_dir[256];
const char *config_default_dir(void)
{
   return g_roles_dir;
}
static void setup_role_templates(void)
{
   snprintf(g_roles_dir, sizeof(g_roles_dir), "/tmp/aimee-test-jobsaux-XXXXXX");
   assert(mkdtemp(g_roles_dir));
   char sub[512];
   snprintf(sub, sizeof(sub), "%s/role_templates", g_roles_dir);
   assert(mkdir(sub, 0700) == 0);
   char path[600];
   snprintf(path, sizeof(path), "%s/review.md", sub);
   FILE *f = fopen(path, "w");
   assert(f);
   fprintf(f, "---\nmax_turns: 20\n---\nbody\n");
   fclose(f);
}

static cJSON *g_last_response = NULL;
static char g_last_error[256];

int server_send_response(server_conn_t *conn, cJSON *resp)
{
   (void)conn;
   if (g_last_response)
      cJSON_Delete(g_last_response);
   g_last_response = cJSON_Duplicate(resp, 1);
   return 0;
}

int server_send_error(server_conn_t *conn, const char *message, const char *request_id)
{
   (void)conn;
   (void)request_id;
   snprintf(g_last_error, sizeof(g_last_error), "%s", message ? message : "");
   return 0;
}

int config_load(config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));
   cfg->aux_enabled = 1;
   snprintf(cfg->aux_default_provider, sizeof(cfg->aux_default_provider), "local");
   snprintf(cfg->aux_default_model, sizeof(cfg->aux_default_model), "small");
   cfg->aux_default_max_tokens = 128;
   cfg->aux_task_count = 1;
   snprintf(cfg->aux_tasks[0].task, sizeof(cfg->aux_tasks[0].task), "title");
   snprintf(cfg->aux_tasks[0].provider, sizeof(cfg->aux_tasks[0].provider), "ollama");
   snprintf(cfg->aux_tasks[0].model, sizeof(cfg->aux_tasks[0].model), "qwen");
   cfg->aux_tasks[0].max_tokens = 64;
   return 0;
}

char *aux_call(const config_t *cfg, const char *task_name, const char *prompt, int max_tokens)
{
   (void)cfg;
   assert(strcmp(task_name, "title") == 0);
   assert(strcmp(prompt, "summarize this") == 0);
   assert(max_tokens == 32);
   return strdup("aux ok");
}

#include "../server/server_jobs_aux.c"

static void reset_last_response(void)
{
   if (g_last_response)
      cJSON_Delete(g_last_response);
   g_last_response = NULL;
   g_last_error[0] = '\0';
}

static void test_jobs_handlers(void)
{
   server_ctx_t ctx = {0};
   server_conn_t conn = {0};
   int list_job_id = db1_agent_job_create("review", "list this job", "codex", "unit-test");
   assert(list_job_id > 0);
   db1_agent_job_update(list_job_id, "running", 2, "progress");

   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "limit", 1);
   assert(handle_jobs_list(&ctx, &conn, req) == 0);
   cJSON *jobs = cJSON_GetObjectItem(g_last_response, "jobs");
   assert(cJSON_IsArray(jobs));
   assert(cJSON_GetArraySize(jobs) == 1);
   cJSON *job = cJSON_GetArrayItem(jobs, 0);
   assert(cJSON_GetObjectItem(job, "id")->valueint == list_job_id);
   assert(strcmp(cJSON_GetObjectItem(job, "status")->valuestring, "running") == 0);
   assert(cJSON_GetObjectItem(job, "prompt") == NULL);
   assert(cJSON_GetObjectItem(job, "default_max_turns")->valueint == 20);
   assert(cJSON_GetObjectItem(job, "final_after_turns") == NULL);
   cJSON_Delete(req);
   reset_last_response();

   req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "job_id", list_job_id);
   assert(handle_jobs_status(&ctx, &conn, req) == 0);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "job_status")->valuestring, "running") == 0);
   job = cJSON_GetObjectItem(g_last_response, "job");
   assert(cJSON_IsObject(job));
   assert(strcmp(cJSON_GetObjectItem(job, "prompt")->valuestring, "list this job") == 0);
   assert(strcmp(cJSON_GetObjectItem(job, "result")->valuestring, "progress") == 0);
   assert(cJSON_GetObjectItem(job, "default_max_turns")->valueint == 20);
   assert(cJSON_GetObjectItem(job, "final_after_turns") == NULL);
   cJSON_Delete(req);
   reset_last_response();

   req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "job_id", list_job_id);
   assert(handle_jobs_logs(&ctx, &conn, req) == 0);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "job_status")->valuestring, "running") == 0);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "log")->valuestring, "progress") == 0);
   job = cJSON_GetObjectItem(g_last_response, "job");
   assert(cJSON_IsObject(job));
   assert(strcmp(cJSON_GetObjectItem(job, "result")->valuestring, "progress") == 0);
   cJSON_Delete(req);
   reset_last_response();

   req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "job_id", 999999);
   assert(handle_jobs_logs(&ctx, &conn, req) == 0);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "job_status")->valuestring, "not_found") ==
          0);
   assert(cJSON_GetObjectItem(g_last_response, "job") == NULL);
   assert(cJSON_GetObjectItem(g_last_response, "log") == NULL);
   cJSON_Delete(req);
   reset_last_response();

   req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "job_id", 0);
   assert(handle_jobs_logs(&ctx, &conn, req) == 0);
   assert(strcmp(g_last_error, "missing or invalid job_id") == 0);
   assert(g_last_response == NULL);
   cJSON_Delete(req);
   reset_last_response();

   int cancel_job_id = db1_agent_job_create("code", "cancel this job", "codex", "unit-test");
   assert(cancel_job_id > 0);
   req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "job_id", cancel_job_id);
   cJSON_AddStringToObject(req, "reason", "unit test");
   assert(handle_jobs_cancel(&ctx, &conn, req) == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(g_last_response, "cancelled")));
   db1_agent_job_t row;
   assert(db1_agent_job_get(cancel_job_id, &row) == 0);
   assert(strcmp(row.status, "cancelled") == 0);
   assert(strcmp(row.result, "cancelled: unit test") == 0);
   cJSON_Delete(req);
   reset_last_response();

   req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "job_id", 999999);
   assert(handle_jobs_cancel(&ctx, &conn, req) == 0);
   assert(cJSON_IsFalse(cJSON_GetObjectItem(g_last_response, "cancelled")));
   assert(cJSON_GetObjectItem(g_last_response, "changed")->valueint == 0);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "message")->valuestring,
                 "No pending or running job found.") == 0);
   cJSON_Delete(req);
   reset_last_response();

   req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "job_id", -1);
   assert(handle_jobs_cancel(&ctx, &conn, req) == 0);
   assert(strcmp(g_last_error, "missing or invalid job_id") == 0);
   assert(g_last_response == NULL);
   cJSON_Delete(req);
   reset_last_response();

   printf("  PASS: test_jobs_handlers\n");
}

static int create_test_plan(void)
{
   cJSON *steps = cJSON_CreateArray();
   assert(steps != NULL);
   cJSON *step = cJSON_CreateObject();
   assert(step != NULL);
   cJSON_AddStringToObject(step, "action", "edit one file");
   cJSON_AddItemToArray(steps, step);
   step = cJSON_CreateObject();
   assert(step != NULL);
   cJSON_AddStringToObject(step, "action", "run focused test");
   cJSON_AddItemToArray(steps, step);

   int plan_id = db1_execution_plan_create("unit", "coord job test", steps);
   cJSON_Delete(steps);
   assert(plan_id > 0);
   return plan_id;
}

static void test_coord_job_handlers(void)
{
   server_ctx_t ctx = {0};
   server_conn_t conn = {0};
   int plan_id = create_test_plan();

   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "plan_id", plan_id);
   cJSON_AddNumberToObject(req, "parallel", 2);
   assert(handle_coord_job_start(&ctx, &conn, req) == 0);
   int job_id = cJSON_GetObjectItem(g_last_response, "job_id")->valueint;
   assert(job_id > 0);
   assert(cJSON_GetObjectItem(g_last_response, "tasks")->valueint == 2);
   assert(cJSON_GetObjectItem(g_last_response, "max_concurrent")->valueint == 2);
   cJSON_Delete(req);
   reset_last_response();

   req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "limit", 1);
   assert(handle_coord_job_list(&ctx, &conn, req) == 0);
   cJSON *jobs = cJSON_GetObjectItem(g_last_response, "jobs");
   assert(cJSON_IsArray(jobs));
   assert(cJSON_GetArraySize(jobs) == 1);
   cJSON *job = cJSON_GetArrayItem(jobs, 0);
   assert(cJSON_GetObjectItem(job, "id")->valueint == job_id);
   assert(cJSON_GetObjectItem(job, "plan_id")->valueint == plan_id);
   cJSON_Delete(req);
   reset_last_response();

   req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "job_id", job_id);
   assert(handle_coord_job_status(&ctx, &conn, req) == 0);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "job_status")->valuestring, "pending") == 0);
   job = cJSON_GetObjectItem(g_last_response, "job");
   assert(cJSON_IsObject(job));
   assert(cJSON_GetObjectItem(job, "total")->valueint == 2);
   cJSON *tasks = cJSON_GetObjectItem(g_last_response, "tasks");
   assert(cJSON_IsArray(tasks));
   assert(cJSON_GetArraySize(tasks) == 2);
   cJSON_Delete(req);
   reset_last_response();

   req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "job_id", job_id);
   assert(handle_coord_job_cancel(&ctx, &conn, req) == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(g_last_response, "cancelled")));
   db1_coord_job_t row;
   assert(db1_coord_job_get(job_id, &row) == 0);
   assert(strcmp(row.status, "cancelled") == 0);
   cJSON_Delete(req);
   reset_last_response();

   printf("  PASS: test_coord_job_handlers\n");
}

static void test_aux_handlers(void)
{
   server_ctx_t ctx = {0};
   server_conn_t conn = {0};
   cJSON *req = cJSON_CreateObject();
   assert(handle_aux_config_show(&ctx, &conn, req) == 0);
   cJSON *aux = cJSON_GetObjectItem(g_last_response, "auxiliary");
   assert(cJSON_IsObject(aux));
   assert(cJSON_IsTrue(cJSON_GetObjectItem(aux, "enabled")));
   assert(strcmp(cJSON_GetObjectItem(aux, "default_provider")->valuestring, "local") == 0);
   cJSON *tasks = cJSON_GetObjectItem(aux, "tasks");
   assert(cJSON_IsArray(tasks));
   assert(cJSON_GetArraySize(tasks) == 1);
   cJSON_Delete(req);
   reset_last_response();

   req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "task", "title");
   cJSON_AddStringToObject(req, "prompt", "summarize this");
   cJSON_AddNumberToObject(req, "max_tokens", 32);
   assert(handle_aux_test(&ctx, &conn, req) == 0);
   assert(strcmp(cJSON_GetObjectItem(g_last_response, "response")->valuestring, "aux ok") == 0);
   cJSON_Delete(req);
   reset_last_response();

   printf("  PASS: test_aux_handlers\n");
}

int main(void)
{
   setup_role_templates();
   assert(db1_init(":memory:") == 0);
   printf("test_server_jobs_aux\n");
   test_jobs_handlers();
   test_coord_job_handlers();
   test_aux_handlers();
   db1_shutdown();
   reset_last_response();
   printf("All tests passed.\n");
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
