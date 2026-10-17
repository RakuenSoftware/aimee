#include "claude_headless.h"
#include "agent_exec.h"
#include "platform_test_util.h"
#include "server_session_control.h"
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long long cancel_at;
static int events, reject_after, binds, reject_bind;
static pid_t child;
static long long now_ms(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
int agent_request_cancelled(void)
{
   return cancel_at && now_ms() >= cancel_at;
}
static int capture(const aimee_session_event_t *event, void *context)
{
   (void)context;
   cJSON *typed = aimee_ir_session_event_json(event);
   assert(typed);
   cJSON_Delete(typed);
   events++;
   server_control_scope_t *scope = server_control_scope_current();
   if (scope && event->kind == AIMEE_SESSION_CONTROL_REQUEST)
   {
      assert(server_control_register(scope, event->request_id, event->control_kind,
                                     event->control) == 0);
      cJSON *reply = cJSON_Parse("{\"decision\":\"allow\"}");
      char id[256];
      snprintf(id, sizeof id, "test-turn:%s", event->request_id);
      assert(server_control_reply("aimee-one", id, "alice", reply) == 0);
      cJSON_Delete(reply);
   }

   return reject_after && events >= reject_after ? -1 : 0;
}
static int bind_session(const char *id, void *context)
{
   assert(context == &binds);
   assert(!strcmp(id, "native-one"));
   binds++;
   return reject_bind ? -1 : 0;
}
static int spawn_fixture(const provider_cli_cfg_t *cfg, const char *prompt, int *input, int *output,
                         pid_t *pid)
{
   assert(!strcmp(cfg->home, "/runtime/credential-home"));
   assert(!strcmp(cfg->resume_id, "resume-one"));
   assert(!strcmp(cfg->session_id, "aimee-one"));
   assert(cfg->primary == 1 && !strcmp(prompt, "hello"));
   int in[2], out[2];
   assert(pipe(in) == 0 && pipe(out) == 0);
   child = fork();
   assert(child >= 0);
   if (!child)
   {
      setsid();
      dup2(in[0], STDIN_FILENO);
      dup2(out[1], STDOUT_FILENO);
      close(in[0]);
      close(in[1]);
      close(out[0]);
      close(out[1]);
      execl(cfg->agent->cli_cmd, cfg->agent->cli_cmd, (char *)NULL);
      _exit(127);
   }
   close(in[0]);
   close(out[1]);
   *input = in[1];
   *output = out[0];
   *pid = child;
   return 0;
}
const provider_cli_adapter_t claude_provider_cli_adapter = {.spawn = spawn_fixture};
static const char *success_frames =
    "printf '%s\\n' 'diagnostic, never conversation content'\n"
    "cat <<'FRAMES'\n"
    "{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"native-one\"}\n"
    "{\"type\":\"stream_event\",\"event\":{\"type\":\"message_start\",\"message\":{\"id\":\"m1\","
    "\"role\":\"assistant\"}}}\n"
    "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_start\",\"index\":0,\"content_"
    "block\":{\"type\":\"text\",\"text\":\"\"}}}\n"
    "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"
    "\"type\":\"text_delta\",\"text\":\"reply\"}}}\n"
    "{\"type\":\"assistant\",\"session_id\":\"native-one\",\"message\":{\"id\":\"m1\",\"role\":"
    "\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"reply\"}]}}\n"
    "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_stop\",\"index\":0}}\n"
    "{\"type\":\"stream_event\",\"event\":{\"type\":\"message_stop\"}}\n"
    "{\"type\":\"result\",\"session_id\":\"native-one\",\"is_error\":false}\n"
    "FRAMES\n";
static void script(const char *path, const char *body)
{
   FILE *f = fopen(path, "w");
   assert(f);
   fputs("#!/bin/sh\npayload=$(cat)\n[ \"$payload\" = hello ] || exit 9\n", f);
   fputs(body, f);
   assert(!fclose(f));
   assert(!chmod(path, 0700));
}
static void interactive_script(const char *path)
{
   FILE *f = fopen(path, "w");
   assert(f);
   fputs("#!/usr/bin/python3\nimport sys,json\n"
         "init=json.loads(sys.stdin.readline())\n"
         "assert init['request']['subtype']=='initialize'\n"
         "print(json.dumps({'type':'control_response','response':{'subtype':'success','request_id':"
         "init['request_id'],'response':{}}}),flush=True)\n"
         "user=json.loads(sys.stdin.readline())\n"
         "assert user['type']=='user' and user['message']['content']=='hello'\n"
         "print(json.dumps({'type':'control_request','request_id':'permission','request':{'subtype'"
         ":'can_use_tool','tool_name':'Read','input':{'path':'source.c'}}}),flush=True)\n"
         "reply=json.loads(sys.stdin.readline())['response']\n"
         "assert reply['request_id']=='permission' and reply['response']['behavior']=='allow'\n"
         "assert reply['response']['updatedInput']=={'path':'source.c'}\n",
         f);
   const char *line = strchr(success_frames, '{');
   while (line && line[0] == '{')
   {
      const char *end = strchr(line, '\n');
      assert(end);
      char *raw = strndup(line, (size_t)(end - line));
      assert(raw);
      cJSON *value = cJSON_CreateString(raw);
      char *literal = cJSON_PrintUnformatted(value);
      assert(literal);
      fprintf(f, "print(%s,flush=True)\n", literal);
      free(raw);
      free(literal);
      cJSON_Delete(value);
      line = end + 1;
   }
   fputs("sys.stdin.read()\n", f);
   assert(!fclose(f) && !chmod(path, 0700));
}
int main(void)
{
   char path[4096];
   snprintf(path, sizeof(path), "%s/aimee-headless-XXXXXX", platform_tmpdir());
   int fd = mkstemp(path);
   assert(fd >= 0);
   close(fd);
   agent_t agent = {0};
   assert(strlen(path) < sizeof(agent.cli_cmd));
   memcpy(agent.cli_cmd, path, strlen(path) + 1);
   agent.cli_idle_timeout_ms = 2000;
   provider_cli_cfg_t cfg = {.agent = &agent,
                             .user_prompt = "hello",
                             .home = "/runtime/credential-home",
                             .primary = 1,
                             .resume_id = "resume-one",
                             .session_id = "aimee-one"};
   agent_result_t out;
   aimee_ir_session_set_sink(capture, NULL);
   script(path, success_frames);
   assert(claude_headless_run(&cfg, bind_session, &binds, &out) == 0 && out.success);
   assert(events == 6 && binds == 1 && !strcmp(out.response, "reply"));
   free(out.response);
   assert(waitpid(child, NULL, WNOHANG) == -1 && errno == ECHILD);

   events = 0;
   binds = 0;
   reject_after = 2;
   assert(claude_headless_run(&cfg, bind_session, &binds, &out) < 0 && !out.success);
   assert(events == 2);
   reject_after = 0;
   reject_bind = 1;
   events = 0;
   assert(claude_headless_run(&cfg, bind_session, &binds, &out) < 0 && !out.success && !events);
   reject_bind = 0;

   script(path, "printf '%s\\n' '{\"type\":\"result\",\"is_error\":true}'\n");
   assert(claude_headless_run(&cfg, NULL, NULL, &out) < 0 && !out.success);
   script(path, "printf '%s\\n' "
                "'{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\",\"index\":"
                "0,\"delta\":{\"type\":\"text_delta\",\"text\":\"invalid\"}}}'\n");
   assert(claude_headless_run(&cfg, NULL, NULL, &out) < 0 && !out.success);
   script(path, "sleep 30\n");
   cancel_at = now_ms() + 100;
   long long start = now_ms();
   assert(claude_headless_run(&cfg, NULL, NULL, &out) < 0 && !out.success);
   assert(now_ms() - start < 4000);
   cancel_at = 0;
   assert(waitpid(child, NULL, WNOHANG) == -1 && errno == ECHILD);
   aimee_ir_session_set_sink(capture, NULL);
   events = 0;
   binds = 0;
   server_control_scope_t *scope = server_control_scope_open("aimee-one", "test-turn", "alice");
   assert(scope);
   server_control_scope_bind(scope);
   cfg.control_channel = 1;
   interactive_script(path);
   assert(claude_headless_run(&cfg, bind_session, &binds, &out) == 0 && out.success);
   assert(events == 8 && binds == 1 && out.response && !strcmp(out.response, "reply"));
   free(out.response);
   server_control_scope_close(scope);
   scope = server_control_scope_open("aimee-one", "test-turn", "alice");
   assert(scope);
   server_control_scope_bind(scope);
   aimee_ir_session_set_sink(capture, NULL);
   events = 0;
   reject_after = 1;
   assert(claude_headless_run(&cfg, NULL, NULL, &out) == AGENT_RC_CAPTURE_FAILED);
   assert(!out.success && events == 1 && !strcmp(out.stop_reason, "capture_failed"));
   reject_after = 0;
   server_control_scope_close(scope);
   aimee_ir_session_set_sink(NULL, NULL);
   unlink(path);
   puts("Claude headless lifecycle tests passed");
   return 0;
}
