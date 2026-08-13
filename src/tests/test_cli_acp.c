#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

/* ---- client-side tool serving (acp_serve_client_request) ---- */

static char *make_workdir(char *dir, size_t sz)
{
   snprintf(dir, sz, "%s/acp_serve_XXXXXX", platform_tmpdir());
   char *d = mkdtemp(dir);
   assert(d != NULL);
   return d;
}

static void test_serve_write_then_read(void)
{
   char wdbuf[64];
   char *wd = make_workdir(wdbuf, sizeof(wdbuf));
   char *resp = NULL;

   /* Agent writes a new file (in a not-yet-existing subdir). */
   int r = acp_serve_client_request(
       "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"fs/write_text_file\","
       "\"params\":{\"path\":\"sub/out.txt\",\"content\":\"hello world\"}}",
       wd, &resp);
   assert(r == 1 && resp != NULL);
   assert(strstr(resp, "\"error\"") == NULL); /* success: result, not error */
   free(resp);
   char full[512];
   snprintf(full, sizeof(full), "%s/sub/out.txt", wd);
   FILE *f = fopen(full, "rb");
   assert(f != NULL);
   char buf[64] = {0};
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[n] = '\0';
   assert(strcmp(buf, "hello world") == 0);

   /* Agent reads it back; the response carries the content. */
   resp = NULL;
   r = acp_serve_client_request("{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"fs/read_text_file\","
                                "\"params\":{\"path\":\"sub/out.txt\"}}",
                                wd, &resp);
   assert(r == 1 && resp != NULL);
   assert(strstr(resp, "hello world") != NULL);
   assert(strstr(resp, "\"error\"") == NULL);
   free(resp);

   unlink(full);
   printf("PASS: fs write then read round-trips within workdir\n");
}

static void test_serve_path_sandbox(void)
{
   char wdbuf[64];
   char *wd = make_workdir(wdbuf, sizeof(wdbuf));
   char *resp = NULL;
   /* A ".." escape is rejected with an error (but still a handled request). */
   int r =
       acp_serve_client_request("{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"fs/read_text_file\","
                                "\"params\":{\"path\":\"../../etc/passwd\"}}",
                                wd, &resp);
   assert(r == 1 && resp != NULL);
   assert(strstr(resp, "\"error\"") != NULL);
   free(resp);

   /* An absolute path outside the workdir is rejected too. */
   resp = NULL;
   r = acp_serve_client_request("{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"fs/write_text_file\","
                                "\"params\":{\"path\":\"/etc/aimee_pwn\",\"content\":\"x\"}}",
                                wd, &resp);
   assert(r == 1 && resp != NULL);
   assert(strstr(resp, "\"error\"") != NULL);
   free(resp);
   assert(access("/etc/aimee_pwn", F_OK) != 0); /* nothing written outside */
   printf("PASS: fs path traversal / absolute escape rejected\n");
}

static void test_serve_permission_autoapprove(void)
{
   char *resp = NULL;
   int r = acp_serve_client_request(
       "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"session/request_permission\",\"params\":{"
       "\"options\":[{\"optionId\":\"rej\",\"kind\":\"reject_once\"},"
       "{\"optionId\":\"yes\",\"kind\":\"allow_once\"}]}}",
       "/tmp", &resp);
   assert(r == 1 && resp != NULL);
   assert(strstr(resp, "\"selected\"") != NULL);
   assert(strstr(resp, "\"yes\"") != NULL); /* picked the allow option, not the reject */
   free(resp);
   printf("PASS: permission request auto-approves an allow option\n");
}

static void test_serve_unknown_and_nonrequests(void)
{
   char *resp = NULL;
   /* Unknown agent request (method+id) -> method-not-found so the agent proceeds. */
   int r = acp_serve_client_request(
       "{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"terminal/create\",\"params\":{}}", "/tmp",
       &resp);
   assert(r == 1 && resp != NULL && strstr(resp, "-32601") != NULL);
   free(resp);

   /* A notification (method, no id) is not a request -> left for acp_turn_consume. */
   resp = NULL;
   r = acp_serve_client_request("{\"jsonrpc\":\"2.0\",\"method\":\"session/update\","
                                "\"params\":{\"update\":{\"content\":\"hi\"}}}",
                                "/tmp", &resp);
   assert(r == 0 && resp == NULL);

   /* A response (id, no method) is not a request either. */
   r = acp_serve_client_request("{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"content\":\"x\"}}",
                                "/tmp", &resp);
   assert(r == 0 && resp == NULL);
   printf("PASS: unknown request errors; notifications/responses pass through\n");
}

int main(void)
{
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
   test_serve_write_then_read();
   test_serve_path_sandbox();
   test_serve_permission_autoapprove();
   test_serve_unknown_and_nonrequests();
   printf("ALL PASS\n");
   return 0;
}
