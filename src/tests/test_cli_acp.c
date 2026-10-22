#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include "agent_exec.h"
#include "server_session_control.h"
#include "util.h"

#include "cli_acp.h"
#include "provider_cli_adapter.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* Minimal stubs needed for provider_cli_adapter_get without the full agent stack */
int agent_execute_with_tools(const agent_t *a, const agent_network_t *n, const char *sys,
                             const char *usr, int max, double temp, agent_result_t *out)
{
   (void)a;
   (void)n;
   (void)sys;
   (void)usr;
   (void)max;
   (void)temp;
   if (out)
   {
      memset(out, 0, sizeof(*out));
      out->success = 1;
   }
   return 0;
}

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
   assert(acp->spawn == NULL);
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

/* ---- outbound turn parser (acp_turn_consume) ---- */

static void test_turn_text_delta(void)
{
   /* Legacy aimee dialect: text/delta notifications accumulate. */
   acp_turn_state_t st = {0};
   assert(acp_turn_consume("{\"jsonrpc\":\"2.0\",\"method\":\"text/delta\","
                           "\"params\":{\"content\":\"po\"}}",
                           &st) == 0);
   assert(acp_turn_consume("{\"jsonrpc\":\"2.0\",\"method\":\"text/delta\","
                           "\"params\":{\"content\":\"ng\"}}",
                           &st) == 0);
   assert(st.text && strcmp(st.text, "pong") == 0);
   assert(!st.done);
   free(st.text);
   printf("PASS: text/delta deltas accumulate\n");
}

static void test_turn_session_update_string(void)
{
   /* aimee's own inbound server streams session/update with a string content
    * and a type of agent_message_chunk. These must accumulate too — the bug
    * this fix addresses (previously session/update was ignored). */
   acp_turn_state_t st = {0};
   assert(acp_turn_consume("{\"jsonrpc\":\"2.0\",\"method\":\"session/update\","
                           "\"params\":{\"sessionId\":\"s1\",\"update\":"
                           "{\"type\":\"agent_message_chunk\",\"content\":\"he\"}}}",
                           &st) == 0);
   assert(acp_turn_consume("{\"jsonrpc\":\"2.0\",\"method\":\"session/update\","
                           "\"params\":{\"sessionId\":\"s1\",\"update\":"
                           "{\"type\":\"agent_message_chunk\",\"content\":\"llo\"}}}",
                           &st) == 0);
   assert(st.text && strcmp(st.text, "hello") == 0);
   free(st.text);
   printf("PASS: session/update string content accumulates\n");
}

static void test_turn_session_update_typed_object(void)
{
   /* Real ACP (Zed) carries content as a typed {type:"text",text:...} object. */
   acp_turn_state_t st = {0};
   assert(acp_turn_consume("{\"jsonrpc\":\"2.0\",\"method\":\"session/update\","
                           "\"params\":{\"update\":{\"sessionUpdate\":\"agent_message_chunk\","
                           "\"content\":{\"type\":\"text\",\"text\":\"hi there\"}}}}",
                           &st) == 0);
   assert(st.text && strcmp(st.text, "hi there") == 0);
   free(st.text);
   printf("PASS: session/update typed object content accumulates\n");
}

static void test_turn_tool_call_counts(void)
{
   acp_turn_state_t st = {0};
   /* legacy tool/call */
   assert(acp_turn_consume("{\"jsonrpc\":\"2.0\",\"method\":\"tool/call\","
                           "\"params\":{\"name\":\"grep\"}}",
                           &st) == 0);
   /* ACP session/update tool_call */
   assert(acp_turn_consume("{\"jsonrpc\":\"2.0\",\"method\":\"session/update\","
                           "\"params\":{\"update\":{\"sessionUpdate\":\"tool_call\","
                           "\"title\":\"read_file\"}}}",
                           &st) == 0);
   assert(st.tool_calls == 2);
   assert(st.text == NULL); /* tool calls contribute no reply text */
   printf("PASS: tool calls counted from both dialects\n");
}

static void test_turn_result_fallback(void)
{
   /* With no streamed text, the terminal result.content becomes the reply. */
   acp_turn_state_t st = {0};
   assert(acp_turn_consume("{\"jsonrpc\":\"2.0\",\"id\":2,"
                           "\"result\":{\"content\":\"final answer\"}}",
                           &st) == 0);
   assert(st.done);
   assert(st.text && strcmp(st.text, "final answer") == 0);
   free(st.text);
   printf("PASS: result.content used as fallback reply\n");
}

static void test_turn_stream_beats_result(void)
{
   /* Streamed text takes precedence; result.content is ignored when text exists. */
   acp_turn_state_t st = {0};
   assert(acp_turn_consume("{\"jsonrpc\":\"2.0\",\"method\":\"session/update\","
                           "\"params\":{\"update\":{\"type\":\"agent_message_chunk\","
                           "\"content\":\"streamed\"}}}",
                           &st) == 0);
   assert(acp_turn_consume("{\"jsonrpc\":\"2.0\",\"id\":2,"
                           "\"result\":{\"content\":\"DUPLICATE\"}}",
                           &st) == 0);
   assert(st.done);
   assert(st.text && strcmp(st.text, "streamed") == 0);
   free(st.text);
   printf("PASS: streamed text wins over result.content\n");
}

static void test_turn_standard_acp_terminal(void)
{
   /* Standard ACP: text streams via session/update; the turn ends with the
    * session/prompt response (id == prompt_id) carrying {stopReason}, no
    * content. The id-2 default must NOT end the turn in this mode. */
   acp_turn_state_t st = {.prompt_id = 3};
   assert(acp_turn_consume("{\"jsonrpc\":\"2.0\",\"method\":\"session/update\","
                           "\"params\":{\"update\":{\"sessionUpdate\":\"agent_message_chunk\","
                           "\"content\":{\"type\":\"text\",\"text\":\"hi\"}}}}",
                           &st) == 0);
   /* an id-2 response (e.g. a stray) does not terminate a prompt_id=3 turn */
   assert(acp_turn_consume("{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"sessionId\":\"s\"}}",
                           &st) == 0);
   assert(!st.done);
   /* the session/prompt response (id 3, stopReason) ends the turn */
   assert(acp_turn_consume(
              "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"stopReason\":\"end_turn\"}}", &st) == 0);
   assert(st.done);
   assert(st.text && strcmp(st.text, "hi") == 0); /* reply came from the stream */
   free(st.text);
   printf("PASS: standard ACP terminal is the session/prompt response\n");
}

static void test_turn_error(void)
{
   acp_turn_state_t st = {0};
   int rc = acp_turn_consume("{\"jsonrpc\":\"2.0\",\"id\":2,"
                             "\"error\":{\"code\":-32000,\"message\":\"boom\"}}",
                             &st);
   assert(rc == -1);
   assert(st.had_error);
   assert(strstr(st.error, "boom") != NULL);
   free(st.text);
   printf("PASS: error response returns -1 with message\n");
}

static void test_turn_ignores_noise(void)
{
   acp_turn_state_t st = {0};
   assert(acp_turn_consume("not json", &st) == 0);
   assert(acp_turn_consume("", &st) == 0);
   assert(acp_turn_consume(NULL, &st) == 0);
   /* initialize ack (id=1) is neither a delta nor the terminal result. */
   assert(acp_turn_consume("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}", &st) == 0);
   assert(st.text == NULL && !st.done && !st.had_error);
   printf("PASS: noise / non-terminal responses ignored\n");
}

/* These fixtures exercise the real native/canonical handoff. The explicit
 * resource owner is deterministic and has no pathname-based filesystem grant. */
static int captured, refused_capture;
static int capture_event(const aimee_session_event_t *event, void *context)
{
   (void)context;
   if (refused_capture)
      return -1;
   cJSON *json = aimee_ir_session_event_json(event);
   assert(json);
   cJSON_Delete(json);
   captured++;
   return 0;
}
static void new_execution(void)
{
   aimee_ir_session_set_sink(NULL, NULL);
   aimee_ir_session_set_sink(capture_event, NULL);
   captured = refused_capture = 0;
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
static void test_live_acp_ownership_and_budgets(void)
{
   char root[256], script[512], marker[512];
   snprintf(root, sizeof root, "%s/acp-live-XXXXXX", platform_tmpdir());
   assert(mkdtemp(root));
   snprintf(script, sizeof script, "%s/acp-fixture", root);
   snprintf(marker, sizeof marker, "%s/blocked", root);
   FILE *file = fopen(script, "w");
   assert(file);
   fputs("#!/usr/bin/env python3\nimport json,sys,time\n"
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
         "send({'id':3,'result':{'stopReason':'end_turn'}})\n",
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
   int status = provider_cli_adapter_execute_cfg(provider_cli_adapter_get("acp"), &cfg, &result);
   if (status)
      fprintf(stderr, "live ACP failed: %d %s\n", status, result.error);
   assert(!status && result.success && !strcmp(result.response, "ACP done"));
   assert(result.canonical_response.n_content == 2);
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
   assert(!provider_cli_adapter_execute_cfg(provider_cli_adapter_get("acp"), &cfg, &result));
   assert(result.success && !strcmp(result.response, "ACP done"));
   agent_result_clear_response(&result);

   new_execution();
   snprintf(agent.cli_cmd, sizeof agent.cli_cmd, "%s blocked %s", script, marker);
   agent.tool_loop_timeout_ms_cap = 500;
   long long start = util_now_ms();
   status = provider_cli_adapter_execute_cfg(provider_cli_adapter_get("acp"), &cfg, &result);
   assert(status == AGENT_RC_CONTEXT_REFUSED && util_now_ms() - start < 2000);
   assert(!access(marker, F_OK)); /* Native peer reached the blocked-write stage. */
   unlink(marker);
   agent_result_clear_response(&result);
   agent.tool_loop_timeout_ms_cap = 0;

   new_execution();
   int attempts = 0;
   cfg.native_progress = refuse_launch;
   cfg.native_progress_context = &attempts;
   assert(provider_cli_adapter_execute_cfg(provider_cli_adapter_get("acp"), &cfg, &result) ==
          AGENT_RC_CONTEXT_REFUSED);
   assert(attempts == 1 && access(marker, F_OK));
   cfg.native_progress = NULL;
   aimee_ir_session_fail_capture();
   assert(provider_cli_adapter_execute_cfg(provider_cli_adapter_get("acp"), &cfg, &result) ==
          AGENT_RC_CAPTURE_FAILED);
   assert(access(marker, F_OK));
   new_execution();
   cfg.history = &catalog;
   assert(provider_cli_adapter_execute_cfg(provider_cli_adapter_get("acp"), &cfg, &result) ==
          AGENT_RC_CONTEXT_REFUSED);
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
   test_turn_text_delta();
   test_turn_session_update_string();
   test_turn_session_update_typed_object();
   test_turn_tool_call_counts();
   test_turn_result_fallback();
   test_turn_stream_beats_result();
   test_turn_standard_acp_terminal();
   test_turn_error();
   test_turn_ignores_noise();
   test_resource_owner_handoff();
   test_no_implicit_filesystem_authority();
   test_permission_uses_explicit_one_time_decision();
   test_unknown_client_request();
   test_live_acp_ownership_and_budgets();
   aimee_ir_session_set_sink(NULL, NULL);
   printf("ALL PASS\n");
   return 0;
}
