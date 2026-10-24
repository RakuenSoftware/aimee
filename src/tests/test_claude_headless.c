#include "claude_headless.h"
#include "cli_claude_native.h"
#include "log.h"
#include <stdarg.h>
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

static int profile_logs;
static char profile_metadata[512];
void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
{
   assert(level == LOG_ERROR && !strcmp(module, "native.claude"));
   va_list args;
   va_start(args, fmt);
   int length = vsnprintf(profile_metadata, sizeof profile_metadata, fmt, args);
   va_end(args);
   assert(length > 0 && (size_t)length < sizeof profile_metadata);
   profile_logs++;
}
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
   if (event->kind == AIMEE_SESSION_CONTROL_RESPONSE)
   {
      assert(!strcmp(
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event->control, "decision")),
          "allow"));
      assert(!cJSON_GetObjectItemCaseSensitive(event->control, "response"));
   }
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
   binds++;
   return reject_bind || strcmp(id, "native-one") ? -1 : 0;
}
static int spawn_calls;
int cli_claude_native_spawn(const provider_cli_cfg_t *cfg, int *input, int *output, pid_t *pid)
{
   spawn_calls++;
   assert(cfg->claude_host);
   assert(!strcmp(cfg->home, "/runtime/credential-home"));
   assert(!strcmp(cfg->resume_id, "resume-one"));
   assert(!strcmp(cfg->session_id, "aimee-one"));
   assert(cfg->primary == 1 && !strcmp(cfg->user_prompt, "hello"));
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
static int profile_init(const cJSON *frame, void *context);
static int prime_profile = 1;
static const char *owned_init =
    "{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"native-one\","
    "\"cwd\":\"/approved\",\"apiKeySource\":\"ANTHROPIC_API_KEY\",\"permissionMode\":\"default\","
    "\"tools\":[],\"mcp_servers\":[],\"plugins\":[]}";
static const char *success_frames =
    "printf '%s\\n' 'diagnostic, never conversation content'\n"
    "cat <<'FRAMES'\n"
    "{\"type\":\"stream_event\",\"event\":{\"type\":\"message_start\",\"message\":{\"id\":\"m1\","
    "\"role\":\"assistant\"}}}\n"
    "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_start\",\"index\":0,\"content_"
    "block\":{\"type\":\"text\",\"text\":\"\"}}}\n"
    "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"
    "\"type\":\"text_delta\",\"text\":\"reply\"}}}\n"
    "{\"type\":\"assistant\",\"session_id\":\"native-one\",\"message\":{\"id\":\"m1\",\"role\":"
    "\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"reply\"}]}}\n"
    "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_stop\",\"index\":0}}\n"
    "{\"type\":\"stream_event\",\"event\":{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":"
    "\"end_turn\"}}}\n"
    "{\"type\":\"stream_event\",\"event\":{\"type\":\"message_stop\"}}\n"
    "{\"type\":\"result\",\"session_id\":\"native-one\",\"is_error\":false}\n"
    "FRAMES\n";
static void script(const char *path, const char *body)
{
   FILE *f = fopen(path, "w");
   assert(f);
   fputs("#!/bin/sh\npayload=$(cat)\n[ \"$payload\" = hello ] || exit 9\n", f);
   if (prime_profile)
      fprintf(f, "printf '%%s\\n' '%s'\n", owned_init);
   fputs(body, f);
   assert(!fclose(f));
   assert(!chmod(path, 0700));
}
/* Emit exact bytes, including NUL/invalid UTF-8, without shell quoting or
 * C-string truncation in the fixture itself. The child consumes the real stdin. */
static void binary_script(const char *path, const unsigned char *bytes, size_t size, int split)
{
   FILE *f = fopen(path, "w");
   assert(f);
   fputs("#!/usr/bin/python3\nimport sys,os\nassert "
         "sys.stdin.buffer.read()==b'hello'\npayload=bytes.fromhex('",
         f);
   if (prime_profile)
   {
      for (size_t i = 0; i < strlen(owned_init); i++)
         fprintf(f, "%02x", (unsigned char)owned_init[i]);
      fputs("0a", f);
   }
   for (size_t i = 0; i < size; i++)
      fprintf(f, "%02x", bytes[i]);
   fputs("')\n", f);
   fputs(split ? "for b in payload: os.write(1,bytes([b]))\n" : "os.write(1,payload)\n", f);
   assert(!fclose(f) && !chmod(path, 0700));
}
static void strict_frames(const char *path, const provider_cli_cfg_t *cfg)
{
   static const char nul[] = "{\"type\":\"result\",\"is_error\":false}\0{}\n";
   static const char escaped[] =
       "{\"type\":\"system\",\"session_id\":\"native-one\\u0000hidden\"}\n";
   static const char duplicate[] = "{\"type\":\"result\",\"type\":\"system\"}\n";
   static const char trailing[] = "{\"type\":\"result\",\"is_error\":false} {}\n";
   static const char utf8[] = "{\"type\":\"system\",\"session_id\":\"bad\xff\"}\n";
   static const char eof_nul[] = "{\"type\":\"result\",\"is_error\":false}\0{}";
   const struct
   {
      const char *bytes;
      size_t length;
   } cases[] = {{nul, sizeof nul - 1},
                {escaped, sizeof escaped - 1},
                {duplicate, sizeof duplicate - 1},
                {trailing, sizeof trailing - 1},
                {utf8, sizeof utf8 - 1},
                {eof_nul, sizeof eof_nul - 1}};
   for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
   {
      events = binds = 0;
      aimee_ir_session_set_sink(capture, NULL);
      binary_script(path, (const unsigned char *)cases[i].bytes, cases[i].length, i % 2);
      agent_result_t out = {0};
      assert(claude_headless_run(cfg, bind_session, &binds, &out) == AGENT_RC_CAPTURE_FAILED);
      assert(!out.success && !out.response && !events && binds == 1);
      assert(!strcmp(out.error, "invalid Claude JSON frame"));
      assert(waitpid(child, NULL, WNOHANG) == -1 && errno == ECHILD);
   }
   /* A completed result cannot hide a malformed unterminated tail. */
   const char *first = strchr(success_frames, '{');
   const char *stop = strstr(first, "FRAMES\n");
   assert(first && stop && stop > first);
   size_t prefix = (size_t)(stop - first), extra = sizeof eof_nul - 1;
   unsigned char *payload = malloc(prefix + extra);
   assert(payload);
   memcpy(payload, first, prefix);
   memcpy(payload + prefix, eof_nul, extra);
   events = binds = 0;
   aimee_ir_session_set_sink(capture, NULL);
   binary_script(path, payload, prefix + extra, 0);
   agent_result_t out = {0};
   assert(claude_headless_run(cfg, bind_session, &binds, &out) == AGENT_RC_CAPTURE_FAILED);
   assert(!out.success && !out.response && events == 0 && binds == 1);
   assert(!strcmp(out.error, "invalid Claude JSON frame"));
   /* Exact valid EOF framing remains accepted, with arbitrary read fragmentation. */
   events = binds = 0;
   aimee_ir_session_set_sink(capture, NULL);
   assert(prefix && payload[prefix - 1] == '\n');
   binary_script(path, payload, prefix - 1, 1);
   assert(!claude_headless_run(cfg, bind_session, &binds, &out) && out.success);
   assert(events == 0 && binds == 1 && !out.response && !out.canonical_response.n_content);
   agent_result_clear_response(&out);
   free(payload);
}
static void api_failure_diagnostic(const char *path, const provider_cli_cfg_t *cfg)
{
   /* Newer print serializers mark diagnostics with snake case; SDK 2.1.177
    * omits that flag. Internal logs use camel case. Never copy diagnostic text. */
   const char *codes[] = {"rate_limit", "invalid_request"};
   const char *messages[] = {"Claude request was rate limited by the provider.",
                             "Claude provider rejected the request."};
   for (size_t category = 0; category < 2; category++)
   {
      char assistant[1024];
      snprintf(assistant, sizeof assistant,
               "{\"type\":\"assistant\",\"is_api_error_message\":true,\"error\":\"%s\","
               "\"session_id\":\"native-one\",\"message\":{\"id\":\"sdk-api-error\",\"type\":"
               "\"message\","
               "\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"PRIVATE_PROVIDER_"
               "DIAGNOSTIC\"}]}}\n",
               codes[category]);
      const char *failed = "{\"type\":\"result\",\"subtype\":\"error_during_execution\","
                           "\"is_error\":true,\"errors\":[\"PRIVATE_RESULT_DIAGNOSTIC\"]}\n";
      char frames[2048];
      snprintf(frames, sizeof frames, "%s%s", assistant, failed);
      binary_script(path, (const unsigned char *)frames, strlen(frames), 0);
      events = 0;
      agent_result_t out = {0};
      assert(claude_headless_run(cfg, NULL, NULL, &out) < 0 && !out.success);
      assert(!strcmp(out.error, messages[category]));
      assert(!events && !out.response && !out.canonical_response.n_content);
      agent_result_clear_response(&out);

      /* A diagnostic from a refused native session cannot label this execution. */
      cJSON *foreign = cJSON_Parse(assistant);
      assert(foreign && cJSON_ReplaceItemInObjectCaseSensitive(foreign, "session_id",
                                                               cJSON_CreateString("foreign-one")));
      char *foreign_json = cJSON_PrintUnformatted(foreign);
      assert(foreign_json);
      snprintf(frames, sizeof frames, "%s\n%s", foreign_json, failed);
      free(foreign_json);
      cJSON_Delete(foreign);
      binary_script(path, (const unsigned char *)frames, strlen(frames), 0);
      binds = events = 0;
      assert(claude_headless_run(cfg, bind_session, &binds, &out) == AGENT_RC_CAPTURE_FAILED);
      assert(!out.success && binds == 2 && !events && strcmp(out.error, messages[category]));
      agent_result_clear_response(&out);

      /* Missing terminal/cancellation never borrows a terminal failure category. */
      binary_script(path, (const unsigned char *)assistant, strlen(assistant), 0);
      assert(claude_headless_run(cfg, NULL, NULL, &out) < 0 && !out.success);
      assert(!strcmp(out.error, "Claude structured turn failed") && !events);
      char script_body[4096];
      snprintf(script_body, sizeof script_body, "cat <<'SDK_ERROR'\n%s%sSDK_ERROR\nsleep 30\n",
               assistant, failed);
      script(path, script_body);
      cancel_at = now_ms() + 100;
      assert(claude_headless_run(cfg, NULL, NULL, &out) < 0 && !out.success);
      assert(!strcmp(out.error, "Claude turn interrupted"));
      cancel_at = 0;

      /* SDK retries may eventually succeed: do not retain a transient diagnosis. */
      snprintf(script_body, sizeof script_body, "cat <<'SDK_ERROR'\n%sSDK_ERROR\n%s", assistant,
               success_frames);
      script(path, script_body);
      events = 0;
      assert(!claude_headless_run(cfg, NULL, NULL, &out) && out.success);
      assert(!out.error[0] && !out.response && !out.canonical_response.n_content && events == 0);
      agent_result_clear_response(&out);
   }
}
static void legacy_diagnostic(const char *path, const provider_cli_cfg_t *cfg)
{
   /* Exact legacy envelope shape, synthetic unknown diagnostic; no text authority. */
   const char *assistant =
       "{\"type\":\"assistant\",\"error\":\"unknown\",\"session_id\":\"native-one\","
       "\"uuid\":\"sdk-uuid\",\"parent_tool_use_id\":null,\"message\":{\"id\":\"diag\","
       "\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"PRIVATE_DIAGNOSTIC\"}]}}"
       "\n";
   char frames[2048];
   snprintf(frames, sizeof frames, "%s%s", assistant,
            "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":true}\n");
   binary_script(path, (const unsigned char *)frames, strlen(frames), 0);
   agent_result_t out = {0};
   binds = events = 0;
   assert(claude_headless_run(cfg, bind_session, &binds, &out) < 0 && !out.success);
   assert(!strcmp(out.error, "Claude structured turn failed"));
   assert(binds == 1 && !events && !out.response && !out.canonical_response.n_content);
   agent_result_clear_response(&out);
   binary_script(path, (const unsigned char *)assistant, strlen(assistant), 0);
   assert(claude_headless_run(cfg, bind_session, &binds, &out) < 0 && !out.success);
   assert(!strcmp(out.error, "Claude structured turn failed") && !events);
   agent_result_clear_response(&out);

   /* Constructed known-code legacy envelopes exercise the source contract;
    * they are not captured wire evidence for the failed live turn. */
   const char *codes[] = {"rate_limit", "invalid_request"};
   const char *errors[] = {"Claude request was rate limited by the provider.",
                           "Claude provider rejected the request."};
   for (size_t i = 0; i < 2; i++)
   {
      cJSON *legacy = cJSON_Parse(assistant);
      assert(legacy &&
             cJSON_ReplaceItemInObjectCaseSensitive(legacy, "error", cJSON_CreateString(codes[i])));
      char *raw = cJSON_PrintUnformatted(legacy);
      assert(raw);
      snprintf(frames, sizeof frames, "%s\n{\"type\":\"result\",\"is_error\":true}\n", raw);
      free(raw);
      cJSON_Delete(legacy);
      binary_script(path, (const unsigned char *)frames, strlen(frames), 0);
      events = binds = 0;
      assert(claude_headless_run(cfg, bind_session, &binds, &out) < 0 && !out.success);
      assert(!strcmp(out.error, errors[i]) && binds == 1 && !events && !out.response);
      agent_result_clear_response(&out);
   }

   /* A foreign ordinary assistant must be refused before any observation. */
   const char *foreign =
       "{\"type\":\"assistant\",\"session_id\":\"foreign-one\",\"message\":{\"id\":\"foreign\","
       "\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"FOREIGN_TEXT\"}]}}\n";
   binary_script(path, (const unsigned char *)foreign, strlen(foreign), 0);
   events = binds = 0;
   assert(claude_headless_run(cfg, bind_session, &binds, &out) == AGENT_RC_CAPTURE_FAILED);
   assert(binds == 2 && !events && !out.response && !out.canonical_response.n_content);
   agent_result_clear_response(&out);
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
   cJSON *init_value = cJSON_CreateString(owned_init);
   char *init_literal = cJSON_PrintUnformatted(init_value);
   assert(init_literal);
   fprintf(f, "print(%s,flush=True)\n", init_literal);
   free(init_literal);
   cJSON_Delete(init_value);
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
static int init_calls, init_refused;
static int profile_init(const cJSON *frame, void *context)
{
   (void)frame;
   (void)context;
   init_calls++;
   return init_refused;
}
/* The SDK must never receive user input while its required MCP server is
 * pending. Control-plane metadata is not conversation history. */
static void native_readiness(const char *path, const provider_cli_cfg_t *cfg)
{
   aimee_request_t catalog = {0};
   catalog.tools = calloc(1, sizeof *catalog.tools);
   assert(catalog.tools);
   catalog.tools[0].name = "read_file";
   cli_claude_host_launch_t host = {.cwd = "/approved", .catalog = &catalog, .init = profile_init};
   provider_cli_cfg_t owned = *cfg;
   agent_t agent = *cfg->agent;
   owned.agent = &agent;
   owned.control_channel = 1;
   owned.claude_host = &host;
   char trace[4096];
   assert(snprintf(trace, sizeof trace, "%s.input", path) < (int)sizeof trace);
   for (int mode = 0; mode <= 19; mode++)
   {
      catalog.n_tools = mode == 17 ? 0 : 1;
      agent.cli_idle_timeout_ms = 120000;
      server_control_scope_t *scope = server_control_scope_open("aimee-one", "readiness", "alice");
      assert(scope);
      server_control_scope_bind(scope);
      unlink(trace);
      cJSON *trace_value = cJSON_CreateString(trace);
      char *literal = cJSON_PrintUnformatted(trace_value);
      assert(literal);
      FILE *f = fopen(path, "w");
      assert(f);
      fprintf(f, "#!/usr/bin/python3\nimport sys,json,select,time\nmode=%d\ntrace=%s\n", mode,
              literal);
      free(literal);
      cJSON_Delete(trace_value);
      fputs("def read():\n"
            " q=json.loads(sys.stdin.readline())\n"
            " if q['type']=='user':\n"
            "  with open(trace,'a') as f: f.write('user\\n')\n"
            " return q\n"
            "def reply(q,servers):\n"
            " r={'type':'control_response','response':{'subtype':'success','request_id':q['request_"
            "id'],'response':{'mcpServers':servers}}}\n"
            " if "
            "mode==8:r['response']['subtype']='error';r['response']['error']='PRIVATE_RESPONSE_"
            "ERROR'\n"
            " if mode==9:r['response']['request_id']='stale-request'\n"
            " print(json.dumps(r),flush=True)\n"
            " if mode==10:print(json.dumps(r),flush=True)\n"
            "q=read();assert q['request']['subtype']=='initialize'\n"
            "ack={'type':'control_response','response':{'subtype':'success','request_id':q['"
            "request_id'],'response':{}}}\n"
            "if mode==15:ack['response']['request_id']='unowned-init'\n"
            "print(json.dumps(ack),flush=True)\n"
            "if mode==16:print(json.dumps(ack),flush=True)\n"
            "if mode==18:\n"
            " print(json.dumps({'type':'system','subtype':'init','session_id':'native-one','cwd':'/"
            "approved','apiKeySource':'ANTHROPIC_API_KEY','permissionMode':'default','tools':['"
            "read_file'],'mcp_servers':[{'name':'aimee','status':'connected'}],'plugins':[]}),"
            "flush=True)\n"
            "ids=set()\n"
            "if mode!=17:\n"
            " for state in ['pending','pending','connected']:\n"
            "  q=read();assert q['type']=='control_request' and "
            "q['request']['subtype']=='mcp_status'\n"
            "  assert q['request_id'] not in ids;ids.add(q['request_id'])\n"
            "  if mode==13:time.sleep(65)\n"
            "  if mode==14:time.sleep(5)\n"
            "  if mode==11:\n"
            "   "
            "print(json.dumps({'type':'control_request','request_id':'unowned-permission','request'"
            ":{'subtype':'can_use_tool','tool_name':'Read','input':{}}}),flush=True)\n"
            "  if mode in "
            "[1,2,3,4]:state={1:'failed',2:'needs-auth',3:'disabled',4:'PRIVATE_UNKNOWN_STATUS'}["
            "mode]\n"
            "  "
            "servers=[{'name':'aimee','status':state,'error':'PRIVATE_STATUS_ERROR','config':{'"
            "headers':{'Authorization':'PRIVATE_STATUS_TOKEN'}}}]\n"
            "  if mode==5:servers=[]\n"
            "  if mode==6:servers=servers*2\n"
            "  if mode==7:servers='PRIVATE_NOT_ARRAY'\n"
            "  reply(q,servers)\n"
            "  if state=='pending': assert not select.select([sys.stdin],[],[],0.08)[0]\n"
            "u=read();assert u['type']=='user' and u['message']['content']=='hello'\n"
            "if mode==19:print(json.dumps(ack),flush=True)\n"
            "tools=[] if mode in [12,17] else ['read_file']\n"
            "servers=[] if mode==17 else [{'name':'aimee','status':'connected'}]\n"
            "print(json.dumps({'type':'system','subtype':'init','session_id':'native-one','cwd':'/"
            "approved','apiKeySource':'ANTHROPIC_API_KEY','permissionMode':'default','tools':tools,"
            "'mcp_servers':servers,'plugins':[]}),flush=True)\n"
            "print(json.dumps({'type':'result','is_error':False}),flush=True)\n"
            "assert not sys.stdin.read()\n",
            f);
      assert(!fclose(f) && !chmod(path, 0700));
      aimee_ir_session_set_sink(capture, NULL);
      init_calls = init_refused = events = profile_logs = 0;
      cancel_at = mode == 14 ? now_ms() + 350 : 0;
      long long start = now_ms();
      agent_result_t out = {0};
      int rc = claude_headless_run(&owned, NULL, NULL, &out);
      if (mode == 0 || mode == 17)
         assert(!rc && out.success && init_calls == 1 && !profile_logs);
      else
         assert(rc < 0 && !out.success && !init_calls);
      assert(!events && !out.response);
      if (mode == 12)
         assert(profile_logs == 1 &&
                !strcmp(out.error, "Claude native launch profile was refused (tool count)"));
      else
         assert(!profile_logs);
      if (mode == 13)
         assert(!strcmp(out.error, "Claude control initialization timed out") &&
                now_ms() - start >= 60000 && now_ms() - start < 65000);
      if (mode == 14)
         assert(!strcmp(out.error, "Claude turn interrupted") && now_ms() - start < 4000);
      FILE *input = fopen(trace, "r");
      int users = 0;
      if (input)
      {
         char line[32];
         while (fgets(line, sizeof line, input))
         {
            assert(!strcmp(line, "user\n"));
            users++;
         }
         fclose(input);
      }
      assert(users == (mode == 0 || mode == 12 || mode == 17 || mode == 19));
      assert(!strstr(out.error, "PRIVATE_"));
      agent_result_clear_response(&out);
      cancel_at = 0;
      server_control_scope_close(scope);
   }
   unlink(trace);
   free(catalog.tools);
}
static void profile_refusals(const char *path, const provider_cli_cfg_t *cfg)
{
   prime_profile = 0;
   const char *init =
       "{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"native-one\","
       "\"cwd\":\"/approved\",\"apiKeySource\":\"ANTHROPIC_API_KEY\","
       "\"permissionMode\":\"default\",\"tools\":[],\"mcp_servers\":[],\"plugins\":[]}";
   aimee_request_t catalog = {0};
   cli_claude_host_launch_t host = {.cwd = "/approved", .catalog = &catalog};
   provider_cli_cfg_t owned = *cfg;
   owned.claude_host = &host;
   const char *expected[] = {"missing host callback", "host callback", "duplicate initialization",
                             "workspace"};
   for (int i = 0; i < 4; i++)
   {
      char body[4096];
      snprintf(body, sizeof body, "printf '%%s\\n' '%s'\n", init);
      if (i == 2)
         snprintf(body, sizeof body, "printf '%%s\\n' '%s' '%s'\n", init, init);
      script(path, body);
      host.init = i == 0 ? NULL : profile_init;
      host.cwd = i == 3 ? "/different" : "/approved";
      init_calls = 0;
      profile_logs = 0;
      init_refused = i == 1;
      agent_result_t out;
      aimee_ir_session_set_sink(capture, NULL);
      events = 0;
      assert(claude_headless_run(&owned, NULL, NULL, &out) < 0 && !out.success);
      char error[256];
      snprintf(error, sizeof error, "Claude native launch profile was refused (%s)", expected[i]);
      assert(!strcmp(out.error, error));
      assert(init_calls == (i == 1 || i == 2));
      assert(profile_logs == 1);
      assert(!strcmp(profile_metadata,
                     "init profile mismatch: expected_tools=0 reported_tools=0 mcp_servers=0 "
                     "aimee_connected=0 aimee_status=unknown plugins=0 missing_owned_sha256=- "
                     "unexpected_reported_sha256=-"));
      assert(events == 0);
   }
   /* Unknown state and nested SDK error text must not be copied into the
    * single profile diagnostic or canonical history. */
   cJSON *private_frame = cJSON_Parse(init);
   cJSON *private_server = cJSON_CreateObject();
   assert(private_frame && private_server);
   cJSON_AddStringToObject(private_server, "name", "aimee");
   cJSON_AddStringToObject(private_server, "status", "PRIVATE_STATUS_DIAGNOSTIC");
   cJSON_AddStringToObject(private_server, "error", "PRIVATE_MCP_ERROR_TEXT");
   cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(private_frame, "mcp_servers"),
                        private_server);
   char *private_json = cJSON_PrintUnformatted(private_frame);
   assert(private_json);
   char body[4096];
   snprintf(body, sizeof body, "printf '%%s\\n' '%s'\n", private_json);
   free(private_json);
   cJSON_Delete(private_frame);
   script(path, body);
   host.cwd = "/approved";
   host.init = profile_init;
   profile_logs = init_calls = events = 0;
   agent_result_t out = {0};
   assert(claude_headless_run(&owned, NULL, NULL, &out) < 0 && !out.success);
   assert(profile_logs == 1 && !init_calls && !events && !out.response);
   assert(strstr(profile_metadata, "aimee_status=unknown") &&
          !strstr(profile_metadata, "PRIVATE_STATUS_DIAGNOSTIC") &&
          !strstr(profile_metadata, "PRIVATE_MCP_ERROR_TEXT"));
   agent_result_clear_response(&out);
   init_refused = 0;
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
   aimee_request_t catalog = {0};
   cli_claude_host_launch_t host = {.cwd = "/approved", .catalog = &catalog, .init = profile_init};
   agent_result_t out = {0};
   script(path, success_frames);
   int spawned = spawn_calls;
   assert(claude_headless_run(&cfg, bind_session, &binds, &out) == AGENT_RC_CONTEXT_REFUSED);
   assert(spawn_calls == spawned && !out.success && !out.response &&
          !out.canonical_response.n_content);
   assert(strstr(out.error, "canonical Claude execution owner unavailable"));
   cfg.claude_host = &host;
   aimee_ir_session_set_sink(capture, NULL);
   assert(!claude_headless_run(&cfg, bind_session, &binds, &out) && out.success);
   assert(events == 0 && binds == 1 && !out.response && !out.canonical_response.n_content);
   agent_result_clear_response(&out);
   assert(waitpid(child, NULL, WNOHANG) == -1 && errno == ECHILD);
   reject_bind = 1;
   binds = events = 0;
   aimee_ir_session_set_sink(capture, NULL);
   assert(claude_headless_run(&cfg, bind_session, &binds, &out) == AGENT_RC_CAPTURE_FAILED);
   assert(!out.success && !events && binds == 1);
   reject_bind = 0;
   aimee_ir_session_set_sink(capture, NULL);
   legacy_diagnostic(path, &cfg);
   api_failure_diagnostic(path, &cfg);
   script(path, "printf '%s\\n' '{\"type\":\"result\",\"is_error\":true}'\n");
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
   assert(events == 2 && binds == 1 && !out.response && !out.canonical_response.n_content);
   agent_result_clear_response(&out);
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
   scope = server_control_scope_open("aimee-one", "test-turn", "alice");
   assert(scope);
   server_control_scope_bind(scope);
   aimee_ir_session_set_sink(capture, NULL);
   events = 0;
   reject_after = 2; /* Refuse the decision itself before any native approval bytes. */
   assert(claude_headless_run(&cfg, NULL, NULL, &out) == AGENT_RC_CAPTURE_FAILED);
   assert(!out.success && events == 2 && !strcmp(out.stop_reason, "capture_failed"));
   reject_after = 0;
   server_control_scope_close(scope);
   aimee_ir_session_set_sink(NULL, NULL);
   cfg.control_channel = 0;
   script(path,
          "printf '%s\\n' '{\"type\":\"result\",\"is_error\":false,\"result\":\"unobserved\"}'\n");
   assert(!claude_headless_run(&cfg, NULL, NULL, &out) && out.success);
   assert(!out.response && !out.canonical_response.n_content);
   strict_frames(path, &cfg);
   profile_refusals(path, &cfg);
   native_readiness(path, &cfg);
   aimee_ir_session_set_sink(NULL, NULL);
   unlink(path);
   puts("Claude headless lifecycle and strict byte-frame tests passed");
   return 0;
}
