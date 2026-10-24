/* Active voting, description and liveness boundaries consume canonical authored
 * TEXT, never display aliases, THINKING, partial or refused observations. */
#include "aimee.h"
#include "platform_test_util.h"
#include "agent_exec.h"
#include "agent_coord.h"
#include "agent_tasks.h"
#include "agent_config.h"
#include "db1_client/execution_plans.h"
#include "db1_client/agent_jobs.h"
#include "cmd_describe_platform.h"
#include "platform_path.h"
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int mode;
static agent_t model;
static char test_home[256];
const char *config_default_dir(void)
{
   return test_home;
}
int agent_load_config(agent_config_t *cfg)
{
   memset(cfg, 0, sizeof *cfg);
   cfg->agent_count = 1;
   return 0;
}
void agent_http_init(void)
{
}
void agent_http_cleanup(void)
{
}
void agent_reject_degenerate_plain_response(agent_result_t *, const char *);
static void produce(agent_result_t *out)
{
   memset(out, 0, sizeof *out);
   out->success = 1;
   if (mode == 4)
   {
      out->response = strdup("CANONICAL_OK");
      return;
   }
   const char *text = mode == 5 ? "NO_MATCH" : "CANONICAL_OK";
   aimee_block_t blocks[] = {
       {.type = AIMEE_BLK_THINKING, .text = "CANONICAL_OK {\"completion\":100}"},
       {.type = AIMEE_BLK_TEXT, .text = (char *)text}};
   aimee_block_t image = {
       .type = AIMEE_BLK_IMAGE, .media_type = "image/png", .media_ref = "fixture-image"};
   aimee_response_t view = {
       .role = "assistant", .stop_reason = AIMEE_STOP_END_TURN, .content = blocks, .n_content = 2};
   if (mode == 1)
   {
      view.stop_reason = AIMEE_STOP_CONTENT_FILTER;
      view.raw_stop_reason = "refusal";
   }
   if (mode == 2)
      view.n_content = 1;
   if (mode == 3)
      view.stop_reason = AIMEE_STOP_MAX_TOKENS;
   if (mode == 9)
   {
      view.content = &image;
      view.n_content = 1;
   }
   aimee_response_t owned = {0};
   assert(!aimee_ir_response_clone(&view, &owned));
   assert(!agent_result_adopt_response(out, &owned));
   agent_result_replace_display_text(
       out, strdup(mode == 6 ? "DISPLAY_POISON" : "CANONICAL_OK {\"completion\":100}"));
}
int agent_execute(const agent_t *a, const char *sys, const char *prompt, int max, double temp,
                  agent_result_t *out)
{
   (void)a;
   (void)sys;
   (void)prompt;
   (void)max;
   (void)temp;
   produce(out);
   return 0;
}
int agent_run(agent_config_t *cfg, const char *role, const char *sys, const char *prompt, int max,
              agent_result_t *out)
{
   (void)cfg;
   (void)role;
   (void)sys;
   (void)prompt;
   (void)max;
   produce(out);
   return 0;
}
int agent_run_with_tools(agent_config_t *cfg, const char *role, const char *sys, const char *prompt,
                         int max, agent_result_t *out)
{
   return agent_run(cfg, role, sys, prompt, max, out);
}
int agent_run_parallel(agent_config_t *cfg, agent_task_t *tasks, int n, agent_result_t *out,
                       int deadline)
{
   (void)cfg;
   (void)deadline;
   for (int i = 0; i < n; i++)
   {
      assert(!tasks[i].agent && !tasks[i].use_tools && !tasks[i].require_initial_tool_call);
      int saved = mode;
      mode = saved == 9 ? 1 : i < 2 ? 6 : 5;
      produce(&out[i]);
      mode = saved;
      if (saved == 10)
         agent_result_replace_display_text(&out[i],
                                           strdup(i < 2 ? "WRONG_MAJORITY" : "CANONICAL_OK"));
   }
   return n;
}
agent_t *agent_route(agent_config_t *cfg, const char *role)
{
   (void)cfg;
   (void)role;
   return &model;
}
void agent_route_failure_message(const char *role, char *out, size_t n)
{
   (void)role;
   snprintf(out, n, "no route");
}
static void reset(int m)
{
   mode = m;
}
static void test_vote_canonical_quorum(void)
{
   agent_config_t cfg = {0};
   agent_result_t out = {0};
   reset(9);
   assert(agent_vote(&cfg, "review", "task", 3, &out) == -1);
   assert(!out.success);
   agent_result_clear_response(&out);
   reset(10);
   assert(agent_vote(&cfg, "review", "task", 3, &out) == 0);
   assert(!strcmp(out.response, "CANONICAL_OK"));
   assert(out.canonical_response.n_content == 2);
   agent_result_clear_response(&out);
}
static void read_file(const char *path, char *out, size_t n)
{
   FILE *f = fopen(path, "r");
   assert(f);
   size_t got = fread(out, 1, n - 1, f);
   out[got] = 0;
   fclose(f);
}
static void test_description_writes(void)
{
   for (int style = 0; style < 2; style++)
   {
      char path[512], buffer[512];
      snprintf(path, sizeof path, "%s/projects/sample%s.md", test_home, style ? ".style" : "");
      for (int m = 1; m <= 4; m++)
      {
         FILE *f = fopen(path, "w");
         assert(f);
         fputs("KEEP_EXISTING_APPROVED", f);
         fclose(f);
         reset(m);
         int rc = style ? style_one("sample", test_home) : describe_one("sample", test_home);
         assert(rc == -1);
         read_file(path, buffer, sizeof buffer);
         assert(!strcmp(buffer, "KEEP_EXISTING_APPROVED"));
      }
      reset(6);
      assert((style ? style_one("sample", test_home) : describe_one("sample", test_home)) == 0);
      read_file(path, buffer, sizeof buffer);
      assert(!strcmp(buffer, "CANONICAL_OK\n"));
      unlink(path);
   }
}
static void test_liveness_uses_canonical_observation(void)
{
   for (int m = 1; m <= 4; m++)
   {
      reset(m);
      agent_result_t out = {0};
      produce(&out);
      int n = out.canonical_response.n_content;
      agent_reject_degenerate_plain_response(&out, "fixture");
      assert(!out.success);
      assert(out.canonical_response.n_content == n);
      agent_result_clear_response(&out);
   }
   reset(6);
   agent_result_t out = {0};
   produce(&out);
   agent_result_replace_display_text(
       &out, strdup("<tool_call><name>bash</name><arguments>{}</arguments></tool_call>"));
   agent_reject_degenerate_plain_response(&out, "fixture");
   assert(out.success);
   assert(!strcmp(out.response, "CANONICAL_OK"));
   assert(out.canonical_response.n_content == 2);
   agent_result_clear_response(&out);
   reset(6);
   produce(&out);
   free(out.canonical_response.content[1].text);
   out.canonical_response.content[1].text =
       strdup("<tool_call><name>bash</name><arguments>{}</arguments></tool_call>");
   agent_result_replace_display_text(&out, strdup("ordinary display annotation"));
   agent_reject_degenerate_plain_response(&out, "fixture");
   assert(!out.success && out.canonical_response.n_content == 2);
   assert(strstr(out.canonical_response.content[1].text, "<tool_call>"));
   agent_result_clear_response(&out);
}

int main(void)
{
   assert(snprintf(test_home, sizeof test_home, "%s/aimee-business-ir-XXXXXX", platform_tmpdir()) <
          (int)sizeof test_home);
   assert(mkdtemp(test_home));
   char projects[512];
   snprintf(projects, sizeof projects, "%s/projects", test_home);
   assert(!mkdir(projects, 0700));
   test_vote_canonical_quorum();
   test_description_writes();
   test_liveness_uses_canonical_observation();
   rmdir(projects);
   rmdir(test_home);
   puts("agent business IR boundaries: PASS");
   return 0;
}
