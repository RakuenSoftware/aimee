#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include "agent_exec.h"
#include "server_session_control.h"
#include "util.h"

#include "cli_acp.h"
#include "provider_cli_adapter.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* Minimal harnesses have no bound server turn to cancel. */
int agent_request_cancelled(void)
{
   return 0;
}

static void test_acp_adapter_registered(void)
{
   const provider_cli_adapter_t *acp = provider_cli_adapter_get("acp");
   assert(acp != NULL);
   assert(strcmp(acp->cli_kind, "acp") == 0);
   assert(acp->execute != NULL);
   printf("PASS: acp adapter registered with cli_kind='acp' and execute set\n");
}

static void test_acp_not_found_for_unknown(void)
{
   const provider_cli_adapter_t *x = provider_cli_adapter_get("not-a-real-kind");
   assert(x == NULL);
   printf("PASS: unknown cli_kind returns NULL\n");
}

static void test_acp_caps(void)
{
   const provider_cli_adapter_t *acp = provider_cli_adapter_get("acp");
   assert(acp != NULL);
   assert(acp->caps.supports_tool_use == 1);
   assert(acp->caps.write_confidence > 0.0f);
   printf("PASS: acp adapter caps look sane\n");
}

/* The canonical adapter is the sole semantic decoder, including its legacy
 * terminal dialect. No display accumulator supplies completion or tool facts. */
static void new_execution(void);
static int canonical_line(aimee_harness_ir_t *state, const char *line)
{
   cJSON *frame = aimee_ir_json_parse_frame(line, strlen(line));
   assert(frame);
   int rc = aimee_acp_frame_ir(frame, state);
   cJSON_Delete(frame);
   return rc;
}
static void test_canonical_acp_protocol(void)
{
   new_execution();
   aimee_harness_ir_t state = {0};
   assert(!canonical_line(&state, "{\"method\":\"text/delta\",\"params\":{\"content\":\"po\"}}"));
   assert(!canonical_line(&state, "{\"method\":\"text/delta\",\"params\":{\"content\":\"ng\"}}"));
   assert(!aimee_acp_turn_status(&state));
   assert(
       !canonical_line(&state, "{\"id\":2,\"result\":{\"content\":\"ignored legacy duplicate\"}}"));
   assert(aimee_acp_turn_status(&state) == 1);
   aimee_response_t response = {0};
   assert(!aimee_harness_ir_response(&state, &response));
   assert(response.usage_unobserved);
   char *text = aimee_ir_response_text_alloc(&response);
   assert(text && !strcmp(text, "pong") && !aimee_ir_response_complete_valid(&response));
   free(text);
   aimee_response_free(&response);
   aimee_harness_ir_free(&state);

   new_execution();
   state = (aimee_harness_ir_t){0};
   assert(!canonical_line(&state, "{\"id\":2,\"result\":{\"content\":\"legacy answer\"}}"));
   assert(!aimee_harness_ir_response(&state, &response) && response.usage_unobserved);
   aimee_response_free(&response);
   aimee_harness_ir_free(&state);

   new_execution();
   state = (aimee_harness_ir_t){.acp_prompt_id = 3, .acp_session_id = "owned-session"};
   assert(!canonical_line(
       &state, "{\"method\":\"session/"
               "update\",\"params\":{\"sessionId\":\"owned-session\",\"update\":{\"sessionUpdate\":"
               "\"tool_call\",\"toolCallId\":\"opaque\",\"title\":\"write_file\",\"status\":"
               "\"running\",\"rawInput\":{\"path\":\"not executed\"}}}}"));
   assert(!canonical_line(
       &state,
       "{\"method\":\"session/"
       "update\",\"params\":{\"update\":{\"sessionUpdate\":\"tool_call_update\",\"toolCallId\":"
       "\"opaque\",\"status\":\"completed\",\"rawOutput\":\"unverified report\"}}}"));
   assert(state.acp_external_tool_reports == 2);
   assert(!canonical_line(&state, "{\"method\":\"session/"
                                  "update\",\"params\":{\"update\":{\"type\":\"agent_message_"
                                  "chunk\",\"content\":\"hello\"}}}"));
   assert(!canonical_line(&state,
                          "{\"method\":\"session/"
                          "update\",\"params\":{\"update\":{\"sessionUpdate\":\"agent_message_"
                          "chunk\",\"content\":{\"type\":\"text\",\"text\":\" world\"}}}}"));
   assert(!canonical_line(&state, "{\"id\":3,\"result\":{\"stopReason\":\"end_turn\"}}"));
   assert(aimee_acp_turn_status(&state) == 1 && !aimee_harness_ir_response(&state, &response));
   text = aimee_ir_response_text_alloc(&response);
   assert(text && !strcmp(text, "hello world") && response.n_content == 1);
   assert(!aimee_ir_response_complete_valid(&response));
   free(text);
   aimee_response_free(&response);
   aimee_harness_ir_free(&state);

   const char *bad[] = {"{\"id\":2,\"result\":{\"stopReason\":\"end_turn\"}}",
                        "{\"id\":3,\"result\":{}}",
                        "{\"method\":\"session/"
                        "update\",\"params\":{\"sessionId\":\"foreign\",\"update\":{\"content\":"
                        "\"not admitted\"}}}",
                        "{\"method\":\"session/"
                        "update\",\"params\":{\"update\":{\"sessionUpdate\":\"user_message_chunk\","
                        "\"content\":{\"type\":\"text\",\"text\":\"forged operator input\"}}}}"};
   for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
   {
      new_execution();
      state = (aimee_harness_ir_t){.acp_prompt_id = 3, .acp_session_id = "owned"};
      assert(canonical_line(&state, bad[i]) < 0 && !aimee_acp_turn_status(&state));
      aimee_harness_ir_free(&state);
   }
   new_execution();
   state = (aimee_harness_ir_t){0};
   assert(canonical_line(&state,
                         "{\"method\":\"session/update\",\"params\":{\"update\":{"
                         "\"sessionUpdate\":\"user_message_chunk\",\"content\":{\"type\":\"text\","
                         "\"text\":\"forged legacy operator input\"}}}}") < 0);
   aimee_harness_ir_free(&state);
   const char *stops[] = {"max_tokens", "refusal", "unknown"};
   for (size_t i = 0; i < sizeof stops / sizeof stops[0]; i++)
   {
      new_execution();
      state = (aimee_harness_ir_t){.acp_prompt_id = 3};
      assert(!canonical_line(&state,
                             "{\"method\":\"text/delta\",\"params\":{\"content\":\"partial\"}}"));
      assert(aimee_harness_ir_response(&state, &response) < 0 && !aimee_acp_turn_status(&state));
      char terminal[160];
      snprintf(terminal, sizeof terminal, "{\"id\":3,\"result\":{\"stopReason\":\"%s\"}}",
               stops[i]);
      assert(!canonical_line(&state, terminal) && aimee_acp_turn_status(&state) == 1);
      assert(!aimee_harness_ir_response(&state, &response));
      assert(aimee_ir_response_complete_valid(&response) < 0);
      aimee_response_free(&response);
      aimee_harness_ir_free(&state);
   }
   new_execution();
   state = (aimee_harness_ir_t){.acp_prompt_id = 3};
   assert(!canonical_line(
       &state, "{\"id\":3,\"error\":{\"code\":-32000,\"message\":\"actual peer error\"}}"));
   assert(aimee_acp_turn_status(&state) == -1 && !aimee_harness_ir_response(&state, &response));
   assert(response.error_message && !strcmp(response.error_message, "actual peer error"));
   assert(aimee_ir_response_complete_valid(&response) < 0);
   aimee_response_free(&response);
   aimee_harness_ir_free(&state);
   new_execution();
   puts("PASS: canonical ACP owns exact terminal identity, authored output and opaque counts");
}

/* These fixtures exercise the real native/canonical handoff. The explicit
 * resource owner is deterministic and has no pathname-based filesystem grant. */
static int captured, refused_capture, projected_context, projected_opaque;
static int projected_history, projected_reasoning, projected_native_opaque, projected_cache;
static int capture_event(const aimee_session_event_t *event, void *context)
{
   (void)context;
   if (refused_capture)
      return -1;
   cJSON *json = aimee_ir_session_event_json(event);
   assert(json);
   cJSON_Delete(json);
   captured++;
   if (event->kind == AIMEE_SESSION_PROJECTION)
   {
      projected_context += event->projection_host_context_blocks_as_text;
      projected_history += event->projection_history_messages_as_text;
      projected_reasoning += event->projection_reasoning;
      projected_native_opaque += event->projection_opaque;
      projected_cache += event->projection_cache_controls;
      projected_opaque += event->projection_unverified_external_tool_reports;
   }
   return 0;
}
static void new_execution(void)
{
   aimee_ir_session_set_sink(NULL, NULL);
   aimee_ir_session_set_sink(capture_event, NULL);
   captured = refused_capture = projected_context = projected_opaque = 0;
   projected_history = projected_reasoning = projected_native_opaque = projected_cache = 0;
}
typedef struct
{
   int calls, writes;
   char content[128];
} resource_owner_t;
static int resource_call(const aimee_block_t *call, aimee_block_t *result, void *context)
{
   resource_owner_t *owner = context;
   assert(captured > 0); /* Intent is captured before the effect callback. */
   assert(!strcmp(call->tool_namespace, "acp"));
   owner->calls++;
   const cJSON *path = cJSON_GetObjectItemCaseSensitive(call->tool_input, "path");
   int permitted = cJSON_IsString(path) && !strcmp(path->valuestring, "/approved/source.txt");
   result->type = AIMEE_BLK_TOOL_RESULT;
   result->tool_id = strdup(call->tool_id);
   result->tool_is_error = !permitted;
   const char *text = "resource owner refused this source";
   if (permitted && !strcmp(call->tool_name, "write_text_file"))
   {
      const cJSON *content = cJSON_GetObjectItemCaseSensitive(call->tool_input, "content");
      assert(cJSON_IsString(content));
      snprintf(owner->content, sizeof owner->content, "%s", content->valuestring);
      owner->writes++;
      text = "";
   }
   else if (permitted)
   {
      assert(!strcmp(call->tool_name, "read_text_file"));
      const cJSON *line = cJSON_GetObjectItemCaseSensitive(call->tool_input, "line");
      const cJSON *limit = cJSON_GetObjectItemCaseSensitive(call->tool_input, "limit");
      if (line)
         assert(cJSON_IsNumber(line) && line->valueint == 2);
      text = limit && limit->valueint == 0 ? "" : owner->content;
   }
   result->tool_result = cJSON_CreateString(text);
   assert(result->tool_id && result->tool_result);
   return 0;
}
static cJSON *dispatch(const char *wire, provider_cli_cfg_t *cfg, aimee_harness_ir_t *ir,
                       const cJSON *decision, int *status)
{
   cJSON *frame = aimee_ir_json_parse_frame(wire, strlen(wire)), *reply = NULL;
   assert(frame);
   *status = aimee_acp_frame_ir(frame, ir);
   if (!*status)
      *status = acp_dispatch_client_request(frame, cfg, ir, decision, &reply);
   cJSON_Delete(frame);
   return reply;
}
static void test_resource_owner_handoff(void)
{
   new_execution();
   cJSON *schema = cJSON_Parse("{\"type\":\"object\"}");
   aimee_tool_t resources[] = {
       {.kind = AIMEE_TOOL_FUNCTION, .name = "read_text_file", .schema = schema},
       {.kind = AIMEE_TOOL_FUNCTION, .name = "write_text_file", .schema = schema}};
   aimee_tool_t ns = {
       .kind = AIMEE_TOOL_NAMESPACE, .name = "acp", .children = resources, .n_children = 2};
   aimee_request_t catalog = {.tools = &ns, .n_tools = 1};
   resource_owner_t owner = {0};
   provider_cli_cfg_t cfg = {
       .tool_catalog = &catalog, .tool_call = resource_call, .tool_context = &owner};
   aimee_harness_ir_t ir = {.acp_session_id = "native-session"};
   int status;
   const char *write = "{\"id\":10,\"method\":\"fs/"
                       "write_text_file\",\"params\":{\"sessionId\":\"native-session\",\"path\":\"/"
                       "approved/source.txt\",\"content\":\"exact raw content\"}}";
   cJSON *reply = dispatch(write, &cfg, &ir, NULL, &status);
   assert(status == 1 && cJSON_IsObject(cJSON_GetObjectItem(reply, "result")) && owner.writes == 1);
   cJSON_Delete(reply);
   reply = dispatch("{\"id\":11,\"method\":\"fs/"
                    "read_text_file\",\"params\":{\"sessionId\":\"native-session\",\"path\":\"/"
                    "approved/source.txt\"}}",
                    &cfg, &ir, NULL, &status);
   assert(status == 1 && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(
                                     cJSON_GetObjectItem(reply, "result"), "content")),
                                 "exact raw content"));
   cJSON_Delete(reply);
   reply = dispatch("{\"id\":12,\"method\":\"fs/read_text_file\",\"params\":{\"path\":\"/approved/"
                    "source.txt\",\"line\":2,\"limit\":0}}",
                    &cfg, &ir, NULL, &status);
   assert(status == 1 && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(
                                     cJSON_GetObjectItem(reply, "result"), "content")),
                                 ""));
   cJSON_Delete(reply);
   reply = dispatch("{\"id\":13,\"method\":\"fs/write_text_file\",\"params\":{\"path\":\"/outside/"
                    "secret\",\"content\":\"overwrite\"}}",
                    &cfg, &ir, NULL, &status);
   assert(status == 1 && cJSON_IsObject(cJSON_GetObjectItem(reply, "error")) && owner.writes == 1);
   cJSON_Delete(reply);
   int before = owner.calls;
   reply = dispatch(write, &cfg, &ir, NULL, &status);
   assert(status < 0 && !reply && owner.calls == before && owner.writes == 1);
   assert(aimee_ir_session_capture_failed());
   aimee_harness_ir_free(&ir);
   new_execution();
   ir.acp_session_id = "native-session";
   refused_capture = 1;
   reply = dispatch(write, &cfg, &ir, NULL, &status);
   assert(status < 0 && !reply && owner.calls == before && owner.writes == 1);
   refused_capture = 0;
   aimee_harness_ir_free(&ir);
   cJSON_Delete(schema);
   puts("PASS: exact resource intents/results, range fidelity and replay refusal");
}
static void test_no_implicit_filesystem_authority(void)
{
   new_execution();
   aimee_harness_ir_t ir = {0};
   provider_cli_cfg_t cfg = {0};
   int status;
   cJSON *reply =
       dispatch("{\"id\":1,\"method\":\"fs/read_text_file\",\"params\":{\"path\":\"/etc/passwd\"}}",
                &cfg, &ir, NULL, &status);
   assert(status == 1 && cJSON_IsObject(cJSON_GetObjectItem(reply, "error")));
   cJSON_Delete(reply);
   aimee_harness_ir_free(&ir);
   new_execution();
   ir.acp_session_id = "held-session";
   reply = dispatch(
       "{\"id\":2,\"method\":\"fs/"
       "read_text_file\",\"params\":{\"sessionId\":\"foreign-session\",\"path\":\"/etc/passwd\"}}",
       &cfg, &ir, NULL, &status);
   assert(status < 0 && !reply);
   aimee_harness_ir_free(&ir);
   new_execution();
   refused_capture = 1;
   reply = dispatch("{\"id\":3,\"method\":\"fs/write_text_file\",\"params\":{\"path\":\"/approved/"
                    "source.txt\",\"content\":\"not captured\"}}",
                    &cfg, &ir, NULL, &status);
   assert(status < 0 && !reply);
   refused_capture = 0;
   aimee_harness_ir_free(&ir);
   puts("PASS: absent owner, wrong session and failed capture cannot grant I/O");
}
static void test_permission_uses_explicit_one_time_decision(void)
{
   new_execution();
   aimee_harness_ir_t ir = {0};
   provider_cli_cfg_t cfg = {0};
   cJSON *allow = cJSON_Parse("{\"decision\":\"allow\"}"),
         *cancel = cJSON_Parse("{\"decision\":\"cancel\"}");
   const char *request = "{\"id\":20,\"method\":\"session/"
                         "request_permission\",\"params\":{\"toolCall\":{\"title\":\"Write "
                         "file\"},\"options\":[{\"optionId\":\"persistent\",\"kind\":\"allow_"
                         "always\"},{\"optionId\":\"once\",\"kind\":\"allow_once\"}]}}";
   int status;
   cJSON *reply = dispatch(request, &cfg, &ir, allow, &status);
   cJSON *outcome = cJSON_GetObjectItem(cJSON_GetObjectItem(reply, "result"), "outcome");
   assert(status == 1 &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(outcome, "optionId")), "once"));
   cJSON_Delete(reply);
   reply = dispatch(request, &cfg, &ir, allow, &status);
   assert(status < 0 && !reply); /* Completed controls cannot prompt/execute again. */
   aimee_harness_ir_free(&ir);
   new_execution();
   reply = dispatch(request, &cfg, &ir, cancel, &status);
   outcome = cJSON_GetObjectItem(cJSON_GetObjectItem(reply, "result"), "outcome");
   assert(status == 1 &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(outcome, "outcome")), "cancelled"));
   cJSON_Delete(reply);
   aimee_harness_ir_free(&ir);
   cJSON_Delete(allow);
   cJSON_Delete(cancel);
   puts("PASS: permission decisions select one-time options and reject replay");
}
static void test_unknown_client_request(void)
{
   new_execution();
   aimee_harness_ir_t ir = {0};
   int status;
   cJSON *reply = dispatch("{\"id\":5,\"method\":\"terminal/create\",\"params\":{}}", NULL, &ir,
                           NULL, &status);
   assert(status == 1 && cJSON_IsObject(cJSON_GetObjectItem(reply, "error")));
   cJSON_Delete(reply);
   aimee_harness_ir_free(&ir);
}

static int live_operator, live_requests, live_responses;
static int live_capture(const aimee_session_event_t *event, void *context)
{
   int rc = capture_event(event, context);
   if (rc)
      return rc;
   if (event->kind == AIMEE_SESSION_CONTROL_REQUEST)
   {
      assert(live_operator);
      server_control_scope_t *scope = server_control_scope_current();
      assert(
          !server_control_register(scope, event->request_id, event->control_kind, event->control));
      cJSON *allow = cJSON_Parse("{\"decision\":\"allow\"}");
      char id[128];
      snprintf(id, sizeof id, "acp-turn:%s", event->request_id);
      assert(server_control_reply("acp-parent", id, "bob", allow) < 0);
      assert(!server_control_reply("acp-parent", id, "alice", allow));
      cJSON_Delete(allow);
      live_requests++;
   }
   else if (event->kind == AIMEE_SESSION_CONTROL_RESPONSE)
   {
      assert(!strcmp(
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event->control, "decision")),
          "allow"));
      live_responses++;
   }
   return 0;
}
static int refuse_launch(void *context)
{
   (*(int *)context)++;
   return -1;
}
static int fixture_launch_valid(void *context)
{
   return context ? 0 : -1;
}
static int inherited_stdio_holes;
static int run_owned_fixture(provider_cli_cfg_t *cfg, agent_result_t *result)
{
   char *argv[65] = {0}, error[128];
   int count = provider_cli_split_command(cfg->agent->cli_cmd, argv, 64, error, sizeof error);
   assert(count > 0);
   int executable = open(argv[0], O_RDONLY | O_CLOEXEC);
   int cwd = open(cfg->cwd, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   assert(executable >= 0 && cwd >= 0);
   char *environment[] = {"PATH=/usr/local/bin:/usr/bin:/bin", "LANG=C.UTF-8", NULL};
   cli_acp_host_launch_t host = {.executable_fd = executable,
                                 .cwd_fd = cwd,
                                 .argv = argv,
                                 .environment = environment,
                                 .cwd = cfg->cwd,
                                 .validate = fixture_launch_valid,
                                 .context = cfg};
   cfg->acp_host = &host;
   for (int fd = 0; fd < 3; fd++)
      if (inherited_stdio_holes & (1 << fd))
         close(fd);
   int rc = provider_cli_adapter_get("acp")->execute(cfg, result);
   cfg->acp_host = NULL;
   close(executable);
   close(cwd);
   provider_cli_free_tokens(argv, count);
   return rc;
}
static void test_live_acp_ownership_and_budgets(void)
{
   char root[256], script[512], marker[512];
   snprintf(root, sizeof root, "%s/acp-live-XXXXXX", platform_tmpdir());
   assert(mkdtemp(root));
   snprintf(script, sizeof script, "%s/acp-fixture", root);
   snprintf(marker, sizeof marker, "%s/blocked", root);
   FILE *file = fopen(script, "w");
   assert(file);
   fputs(
       "#!/usr/bin/env python3\nimport json,sys,time\n"
       "print('worker diagnostic is not protocol',file=sys.stderr,flush=True)\n"
       "mode=sys.argv[1]\n"
       "def send(value): print(json.dumps(value),flush=True)\n"
       "def receive(): return json.loads(sys.stdin.readline())\n"
       "f=receive(); assert f['method']=='initialize'\n"
       "assert "
       "f['params']['clientCapabilities']['fs']=={'readTextFile':mode=='happy','writeTextFile':"
       "mode=='happy'}\n"
       "send({'id':1,'result':{'protocolVersion':1}})\n"
       "f=receive(); assert f['method']=='session/new'\n"
       "sid='native-session' if mode=='happy' else 's'*2048\n"
       "send({'id':2,'result':{'sessionId':sid}})\n"
       "if mode=='blocked':\n open(sys.argv[2],'w').write('ready'); time.sleep(5); sys.exit(0)\n"
       "f=receive(); assert f['method']=='session/prompt' and f['params']['sessionId']==sid\n"
       "if mode=='long': assert len(f['params']['prompt'][0]['text'])==200000\n"
       "if mode=='canonical':\n"
       " assert f['params']['prompt']==[{'type':'text','text':'host context exact'},"
       "{'type':'text','text':'canonical task exact'}]\n"
       "if mode=='history':\n"
       " p=f['params']['prompt']; assert len(p)==3 and p[0]['text']=='host context exact'\n"
       " assert p[2]['text']=='canonical task exact'\n"
       " h=json.loads(p[1]['text'].split('\\n',1)[1]); assert [m['role'] for m in "
       "h]==['user','assistant','tool']\n"
       " assert h[0]['blocks'][0]['text']=='old request'\n"
       " assert len(h[1]['blocks'])==1 and h[1]['blocks'][0]['tool_id']=='prior-call'\n"
       " assert h[2]['blocks'][0]['tool_id']=='prior-call' and "
       "h[2]['blocks'][0]['content']=='prior "
       "bytes'\n"
       " assert 'private chain' not in p[1]['text'] and 'thinking_signature' not in p[1]['text']\n"
       " assert 'cache_control' not in p[1]['text']\n"
       "if mode=='happy':\n"
       " send({'id':20,'method':'session/"
       "request_permission','params':{'sessionId':sid,'toolCall':{'title':'Write "
       "file'},'options':[{'optionId':'persistent','kind':'allow_always'},{'optionId':'once','"
       "kind':'allow_once'}]}})\n"
       " f=receive(); assert f['id']==20 and "
       "f['result']['outcome']=={'outcome':'selected','optionId':'once'}\n"
       " send({'id':'write-rpc','method':'fs/write_text_file','params':{'sessionId':sid,'path':'/"
       "approved/source.txt','content':'client resource data'}})\n"
       " f=receive(); assert f['id']=='write-rpc' and f['result']=={}\n"
       " send({'id':22,'method':'fs/read_text_file','params':{'sessionId':sid,'path':'/approved/"
       "source.txt'}})\n"
       " f=receive(); assert f['id']==22 and f['result']['content']=='client resource data'\n"
       " send({'method':'session/"
       "update','params':{'update':{'sessionUpdate':'agent_thought_chunk','content':{'type':'"
       "text','text':'reason'}}}})\n"
       "send({'method':'session/"
       "update','params':{'update':{'sessionUpdate':'agent_message_chunk','content':{'type':'"
       "text','text':'ACP done'}}}})\n"
       "send({'id':3,'result':{'stopReason':mode if mode in "
       "['max_tokens','refusal','unexpected'] else 'end_turn'}})\n",
       file);
   assert(!fclose(file) && !chmod(script, 0700));
   cJSON *schema = cJSON_Parse("{\"type\":\"object\"}");
   aimee_tool_t resources[] = {
       {.kind = AIMEE_TOOL_FUNCTION, .name = "read_text_file", .schema = schema},
       {.kind = AIMEE_TOOL_FUNCTION, .name = "write_text_file", .schema = schema}};
   aimee_tool_t ns = {
       .kind = AIMEE_TOOL_NAMESPACE, .name = "acp", .children = resources, .n_children = 2};
   aimee_request_t catalog = {.tools = &ns, .n_tools = 1};
   resource_owner_t owner = {0};
   agent_t agent = {0};
   strcpy(agent.name, "ACP fixture");
   agent.timeout_ms = 5000;
   snprintf(agent.cli_cmd, sizeof agent.cli_cmd, "%s happy", script);
   provider_cli_cfg_t cfg = {.agent = &agent,
                             .cwd = root,
                             .user_prompt = "perform task",
                             .tool_catalog = &catalog,
                             .tool_call = resource_call,
                             .tool_context = &owner};
   new_execution();
   live_operator = 1;
   live_requests = live_responses = 0;
   server_control_scope_t *scope = server_control_scope_open("acp-parent", "acp-turn", "alice");
   assert(scope);
   server_control_scope_t *previous = server_control_scope_bind(scope);
   aimee_ir_session_set_sink(live_capture, NULL);
   agent_result_t result = {0};
   int status = run_owned_fixture(&cfg, &result);
   if (status)
      fprintf(stderr, "live ACP failed: %d %s\n", status, result.error);
   assert(!status && result.success && !strcmp(result.response, "ACP done"));
   assert(result.canonical_response.n_content == 2);
   assert(result.canonical_response.usage_unobserved);
   assert(result.canonical_response.content[0].type == AIMEE_BLK_THINKING);
   assert(owner.calls == 2 && owner.writes == 1 && live_requests == 1 && live_responses == 1);
   agent_result_clear_response(&result);
   server_control_scope_bind(previous);
   server_control_scope_close(scope);
   live_operator = 0;

   new_execution();
   char *large = malloc(200001);
   assert(large);
   memset(large, 'p', 200000);
   large[200000] = 0;
   cfg.user_prompt = large;
   cfg.tool_catalog = NULL;
   cfg.tool_call = NULL;
   cfg.tool_context = NULL;
   snprintf(agent.cli_cmd, sizeof agent.cli_cmd, "%s long", script);
   assert(!run_owned_fixture(&cfg, &result));
   assert(result.success && !strcmp(result.response, "ACP done"));
   agent_result_clear_response(&result);
   /* Every inherited stdio hole must preserve the exact worker protocol. */
   for (int holes = 1; holes < 8; holes++)
   {
      pid_t child = fork();
      assert(child >= 0);
      if (!child)
      {
         inherited_stdio_holes = holes;
         new_execution();
         agent_result_t child_result = {0};
         int child_rc = run_owned_fixture(&cfg, &child_result);
         int ok = !child_rc && child_result.success && child_result.response &&
                  !strcmp(child_result.response, "ACP done");
         agent_result_clear_response(&child_result);
         _exit(ok ? 0 : 1);
      }
      int status = 0;
      assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
   }

   pid_t high_fds = fork();
   assert(high_fds >= 0);
   if (!high_fds)
   {
      struct rlimit descriptors;
      if (getrlimit(RLIMIT_NOFILE, &descriptors) || descriptors.rlim_max < 1200)
         _exit(0); /* This platform cannot represent the high-descriptor case. */
      if (descriptors.rlim_cur < 1200)
      {
         descriptors.rlim_cur = 1200;
         if (setrlimit(RLIMIT_NOFILE, &descriptors))
            _exit(2);
      }
      int fd = -1;
      while (fd < 1100)
      {
         fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
         if (fd < 0)
            _exit(2);
      }
      new_execution();
      agent_result_t high_result = {0};
      int rc = run_owned_fixture(&cfg, &high_result);
      int ok = !rc && high_result.success && high_result.response &&
               !strcmp(high_result.response, "ACP done");
      agent_result_clear_response(&high_result);
      _exit(ok ? 0 : 1);
   }
   int high_status = 0;
   assert(waitpid(high_fds, &high_status, 0) == high_fds && WIFEXITED(high_status) &&
          !WEXITSTATUS(high_status));

   new_execution();
   aimee_block_t context = {.type = AIMEE_BLK_TEXT, .text = "host context exact"};
   aimee_block_t task_text = {.type = AIMEE_BLK_TEXT, .text = "canonical task exact"};
   aimee_message_t task_message = {.role = "user", .blocks = &task_text, .n_blocks = 1};
   aimee_request_t task = {
       .system = &context, .n_system = 1, .messages = &task_message, .n_messages = 1};
   cfg.canonical_request = &task;
   cfg.user_prompt = "display projection cannot replace the original task";
   snprintf(agent.cli_cmd, sizeof agent.cli_cmd, "%s canonical", script);
   assert(!run_owned_fixture(&cfg, &result));
   assert(result.success && projected_context == 1 && !projected_opaque);
   assert(result.turns == 0); /* No claim about the external provider's generations. */
   assert(result.canonical_response.usage_unobserved);
   agent_result_clear_response(&result);
   new_execution();
   aimee_block_t prior_user = {
       .type = AIMEE_BLK_TEXT, .text = "old request", .cache_control = "ephemeral"};
   aimee_block_t prior_assistant[2] = {{.type = AIMEE_BLK_THINKING,
                                        .text = "private chain",
                                        .thinking_signature = "opaque-signature"},
                                       {.type = AIMEE_BLK_TOOL_USE,
                                        .tool_id = "prior-call",
                                        .tool_name = "read_file",
                                        .tool_input = cJSON_Parse("{\"path\":\"prior.txt\"}")}};
   aimee_block_t prior_result = {.type = AIMEE_BLK_TOOL_RESULT,
                                 .tool_id = "prior-call",
                                 .tool_result = cJSON_CreateString("prior bytes")};
   aimee_message_t prior_messages[4] = {
       {.role = "user", .blocks = &prior_user, .n_blocks = 1},
       {.role = "assistant", .blocks = prior_assistant, .n_blocks = 2},
       {.role = "tool", .blocks = &prior_result, .n_blocks = 1},
       task_message};
   task.messages = prior_messages;
   task.n_messages = 4;
   snprintf(agent.cli_cmd, sizeof agent.cli_cmd, "%s history", script);
   int history_rc = run_owned_fixture(&cfg, &result);
   if (history_rc)
      fprintf(stderr, "ACP history fixture: %d %s\n", history_rc, result.error);
   assert(!history_rc && result.success && !result.tool_calls);
   assert(projected_history == 3 && projected_context == 1 && projected_reasoning == 1 &&
          projected_native_opaque == 1 && projected_cache == 1);
   agent_result_clear_response(&result);
   new_execution();
   aimee_block_t image = {.type = AIMEE_BLK_IMAGE,
                          .media_type = "image/png",
                          .media_ref = "data:image/png;base64,AA=="};
   prior_messages[0].blocks = &image;
   snprintf(agent.cli_cmd, sizeof agent.cli_cmd, "%s blocked %s", script, marker);
   assert(run_owned_fixture(&cfg, &result) == AGENT_RC_CONTEXT_REFUSED && access(marker, F_OK));
   agent_result_clear_response(&result);
   cJSON_Delete(prior_assistant[1].tool_input);
   cJSON_Delete(prior_result.tool_result);
   task.messages = &task_message;
   task.n_messages = 1;
   snprintf(agent.cli_cmd, sizeof agent.cli_cmd, "%s blocked %s", script, marker);
   agent.tool_loop_timeout_ms_cap = 500;
   for (int unsupported = 0; unsupported < 6; unsupported++)
   {
      new_execution();
      task.has_max_tokens = unsupported == 0;
      task.max_tokens = unsupported == 0 ? 1 : 0;
      task.has_temperature = unsupported == 1;
      cfg.no_tools = unsupported == 2;
      cfg.native_turn_limit = unsupported == 3 ? 1 : 0;
      agent.max_tokens = unsupported == 4 ? 1 : 0;
      agent.max_turns = unsupported == 5 ? 1 : 0;
      assert(run_owned_fixture(&cfg, &result) == AGENT_RC_CONTEXT_REFUSED);
      assert(!result.success && access(marker, F_OK));
      assert(!result.usage_unobserved && !result.canonical_response.usage_unobserved);
      agent_result_clear_response(&result);
   }
   cfg.no_tools = cfg.native_turn_limit = agent.max_tokens = agent.max_turns = 0;
   task.has_max_tokens = task.max_tokens = task.has_temperature = 0;
   cfg.canonical_request = NULL;
   cfg.user_prompt = large;
   agent.tool_loop_timeout_ms_cap = 0;

   const char *stops[] = {"max_tokens", "refusal", "unexpected"};
   const aimee_stop_reason_t reasons[] = {AIMEE_STOP_MAX_TOKENS, AIMEE_STOP_CONTENT_FILTER,
                                          AIMEE_STOP_UNKNOWN};
   for (size_t i = 0; i < sizeof stops / sizeof stops[0]; ++i)
   {
      new_execution();
      snprintf(agent.cli_cmd, sizeof agent.cli_cmd, "%s %s", script, stops[i]);
      status = run_owned_fixture(&cfg, &result);
      assert(status == AGENT_RC_CONTEXT_REFUSED && !result.success);
      assert(result.canonical_response.stop_reason == reasons[i]);
      assert(!strcmp(result.canonical_response.raw_stop_reason, stops[i]));
      assert(!strcmp(result.response, "ACP done"));
      assert(!aimee_ir_session_capture_failed());
      agent_result_clear_response(&result);
   }

   new_execution();
   snprintf(agent.cli_cmd, sizeof agent.cli_cmd, "%s blocked %s", script, marker);
   agent.tool_loop_timeout_ms_cap = 500;
   long long start = util_now_ms();
   status = run_owned_fixture(&cfg, &result);
   assert(status == AGENT_RC_CONTEXT_REFUSED && util_now_ms() - start < 2000);
   assert(!access(marker, F_OK)); /* Native peer reached the blocked-write stage. */
   assert(result.usage_unobserved && result.canonical_response.usage_unobserved);
   cJSON *failed_response = aimee_ir_response_json(&result.canonical_response);
   const cJSON *failed_usage = cJSON_GetObjectItem(failed_response, "usage");
   assert(cJSON_IsObject(failed_usage) && cJSON_GetArraySize(failed_usage) == 1 &&
          cJSON_IsTrue(cJSON_GetObjectItem(failed_usage, "unobserved")));
   cJSON_Delete(failed_response);
   unlink(marker);
   agent_result_clear_response(&result);
   agent.tool_loop_timeout_ms_cap = 0;

   new_execution();
   int attempts = 0;
   cfg.native_progress = refuse_launch;
   cfg.native_progress_context = &attempts;
   assert(run_owned_fixture(&cfg, &result) == AGENT_RC_CONTEXT_REFUSED);
   assert(attempts == 1 && access(marker, F_OK));
   assert(!result.usage_unobserved && !result.canonical_response.usage_unobserved);
   cfg.native_progress = NULL;
   aimee_ir_session_fail_capture();
   assert(run_owned_fixture(&cfg, &result) == AGENT_RC_CAPTURE_FAILED);
   assert(access(marker, F_OK));
   new_execution();
   cfg.history = &catalog;
   assert(run_owned_fixture(&cfg, &result) == AGENT_RC_CONTEXT_REFUSED);
   assert(access(marker, F_OK));
   agent_result_clear_response(&result);
   free(large);
   cJSON_Delete(schema);
   unlink(script);
   rmdir(root);
   aimee_ir_session_set_sink(NULL, NULL);
   puts("PASS: live ACP actor/control/resource exchange, full identities/prompts and bounded "
        "blocked I/O");
}

int main(void)
{
   signal(SIGPIPE, SIG_IGN);
   test_acp_adapter_registered();
   test_acp_not_found_for_unknown();
   test_acp_caps();
   test_canonical_acp_protocol();
   test_resource_owner_handoff();
   test_no_implicit_filesystem_authority();
   test_permission_uses_explicit_one_time_decision();
   test_unknown_client_request();
   test_live_acp_ownership_and_budgets();
   aimee_ir_session_set_sink(NULL, NULL);
   printf("ALL PASS\n");
   return 0;
}
