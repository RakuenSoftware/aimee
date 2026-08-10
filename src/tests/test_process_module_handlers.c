#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/benchmarks/module_api.h>
#include <aimee/control-web/module_api.h>
#include <aimee/delegates/module_api.h>
#include <aimee/git/module_api.h>
#include <aimee/governance/module_api.h>
#include <aimee/kb-synthesis/module_api.h>
#include <aimee/learning/module_api.h>
#include <aimee/memory/module_api.h>
#include <aimee/providers/module_api.h>
#include <aimee/roundtable/module_api.h>
#include <aimee/runtime-web/module_api.h>
#include <aimee/skills/module_api.h>
#include <aimee/tools/module_api.h>
#include <aimee/workflows/module_api.h>
#include <aimee/workspace/module_api.h>

#define DECLARE_HANDLER(name)                                                                      \
   extern aimee_module_status_t name(const aimee_module_invocation_t *, const uint8_t *, uint32_t, \
                                     uint8_t *, uint32_t, uint32_t *, void *)

DECLARE_HANDLER(aimee_memory_module_handler);
DECLARE_HANDLER(aimee_providers_module_handler);
DECLARE_HANDLER(aimee_learning_module_handler);
DECLARE_HANDLER(aimee_delegates_module_handler);
DECLARE_HANDLER(aimee_tools_module_handler);
DECLARE_HANDLER(aimee_workspace_module_handler);
DECLARE_HANDLER(aimee_git_module_handler);
DECLARE_HANDLER(aimee_skills_module_handler);
DECLARE_HANDLER(aimee_governance_module_handler);
DECLARE_HANDLER(aimee_workflows_module_handler);
DECLARE_HANDLER(aimee_roundtable_module_handler);
DECLARE_HANDLER(aimee_kb_synthesis_module_handler);
DECLARE_HANDLER(aimee_runtime_web_module_handler);
DECLARE_HANDLER(aimee_control_web_module_handler);
DECLARE_HANDLER(aimee_benchmarks_module_handler);

int aimee_module_invocation_cancelled(const aimee_module_invocation_t *invocation)
{
   (void)invocation;
   return 0;
}

static void test_memory(void)
{
   const int64_t scores[] = {0, 329999, 330000, 659999, 660000};
   const aimee_memory_confidence_t expected[] = {
       AIMEE_MEMORY_CONFIDENCE_LOW, AIMEE_MEMORY_CONFIDENCE_LOW, AIMEE_MEMORY_CONFIDENCE_MEDIUM,
       AIMEE_MEMORY_CONFIDENCE_MEDIUM, AIMEE_MEMORY_CONFIDENCE_HIGH};
   for (size_t i = 0; i < sizeof(scores) / sizeof(scores[0]); ++i)
   {
      uint8_t request[AIMEE_MEMORY_REQUEST_LEN], response[AIMEE_MEMORY_RESPONSE_LEN];
      uint32_t response_len = 0;
      aimee_module_invocation_t invocation = {.stage_id = AIMEE_MEMORY_STAGE_RERANK};
      aimee_memory_confidence_t result;
      assert(aimee_memory_request_encode(scores[i], request, sizeof(request)) == 0);
      assert(aimee_memory_module_handler(&invocation, request, sizeof(request), response,
                                         sizeof(response), &response_len,
                                         NULL) == AIMEE_MODULE_STATUS_OK);
      assert(aimee_memory_response_decode(response, response_len, &result) == 0);
      assert(result == expected[i]);
   }
}

static uint32_t learning_mask(const char *signal)
{
   uint8_t request[AIMEE_LEARNING_REQUEST_LEN], response[AIMEE_LEARNING_RESPONSE_LEN];
   uint32_t response_len = 0, mask = 0;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_LEARNING_STAGE_OBSERVE};
   assert(aimee_learning_request_encode(signal, request, sizeof(request)) == 0);
   assert(aimee_learning_module_handler(&invocation, request, sizeof(request), response,
                                        sizeof(response), &response_len,
                                        NULL) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_learning_response_decode(response, response_len, &mask) == 0);
   return mask;
}

static void test_learning(void)
{
   assert(learning_mask("thumb_up") == AIMEE_LEARNING_SINK_RERANKER);
   assert(
       learning_mask("correction") ==
       (AIMEE_LEARNING_SINK_RERANKER | AIMEE_LEARNING_SINK_SUPERSEDE | AIMEE_LEARNING_SINK_RULE));
   assert(learning_mask("workflow_repetition") == AIMEE_LEARNING_SINK_WORKFLOW);
   assert(learning_mask("unknown") == 0);
}

/* Canonicalize `in` by going through the REAL bus module handler (encode ->
 * handler -> decode), writing the result into `out`. */
static void delegates_canonicalize_over_handler(const char *in, char *out, size_t out_cap)
{
   uint8_t request[AIMEE_DELEGATES_MESSAGE_LEN], response[AIMEE_DELEGATES_MESSAGE_LEN];
   uint32_t response_len = 0;
   char role[AIMEE_DELEGATES_ROLE_MAX + 1];
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DELEGATES_STAGE_INVOKE};
   assert(aimee_delegates_message_encode(AIMEE_DELEGATES_REQUEST_MAGIC, in, request,
                                         sizeof(request)) == 0);
   assert(aimee_delegates_module_handler(&invocation, request, sizeof(request), response,
                                         sizeof(response), &response_len,
                                         NULL) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_delegates_message_decode(response, response_len, AIMEE_DELEGATES_RESPONSE_MAGIC,
                                         role, sizeof(role)) == 0);
   assert(strlen(role) < out_cap);
   snprintf(out, out_cap, "%s", role);
}

static void test_delegates(void)
{
   char role[AIMEE_DELEGATES_ROLE_MAX + 1];

   delegates_canonicalize_over_handler("implement", role, sizeof(role));
   assert(strcmp(role, "code") == 0);

   /* The role alias table exists TWICE, byte-for-byte identical: here in the bus
    * module handler (modules/delegates/module_adapter.c) and in delegate_role.c's
    * local path, used by binaries that host no bus — the thin client. Nothing in
    * the build keeps them in sync, so a one-sided edit would make the same role
    * canonicalize differently depending on which binary ran it.
    *
    * Deduplicating them means editing delegates module source, which is a
    * vendored mirror pinned by dependencies/aimee-repositories.lock.json — that
    * needs a coordinated module release. Until then this table is the guard: it
    * pins the handler's full mapping so a drifting edit fails here. The mirror of
    * this expectation for the local path is test_delegate_role.c; the two must
    * state the same pairs, and that is the invariant a reviewer should check when
    * touching either table.
    *
    * Expectations are written out rather than computed FROM the table under test,
    * so this cannot pass vacuously by reading the same array it is checking. */
   static const struct
   {
      const char *alias;
      const char *canonical;
   } expected[] = {
       {"implement", "code"},        {"build", "code"},
       {"reviewer", "review"},       {"verifier", "validate"},
       {"test", "validate"},         {"check", "validate"},
       {"evaluate", "validate"},     {"evaluate-optimize", "validate"},
       {"inspect", "diagnose"},      {"research", "execute"},
       {"enforce", "execute"},       {"recall", "search"},
       {"synthesize", "summarize"},  {"rank-fuse", "reason"},
       {"classify-score", "reason"}, {"planner", "plan"},
       {"planning", "plan"},
   };
   for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i)
   {
      delegates_canonicalize_over_handler(expected[i].alias, role, sizeof(role));
      assert(strcmp(role, expected[i].canonical) == 0);
      assert(strcmp(role, expected[i].alias) != 0); /* every entry really is an alias */
   }

   /* An already-canonical or unknown role passes through untouched. */
   delegates_canonicalize_over_handler("code", role, sizeof(role));
   assert(strcmp(role, "code") == 0);
   delegates_canonicalize_over_handler("no-such-role", role, sizeof(role));
   assert(strcmp(role, "no-such-role") == 0);
}

static aimee_tool_class_t tool_class(const char *name)
{
   uint8_t request[AIMEE_TOOLS_REQUEST_LEN], response[AIMEE_TOOLS_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_tool_class_t result;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_TOOLS_STAGE_DISPATCH};
   assert(aimee_tools_request_encode(name, request, sizeof(request)) == 0);
   assert(aimee_tools_module_handler(&invocation, request, sizeof(request), response,
                                     sizeof(response), &response_len,
                                     NULL) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_tools_response_decode(response, response_len, &result) == 0);
   return result;
}

static void test_tools(void)
{
   assert(tool_class("bash") == AIMEE_TOOL_CLASS_EXEC);
   assert(tool_class("execute_script") == AIMEE_TOOL_CLASS_EXEC);
   assert(tool_class("test") == AIMEE_TOOL_CLASS_EXEC);
   assert(tool_class("run_tests") == AIMEE_TOOL_CLASS_EXEC);
   assert(tool_class("read_file") == AIMEE_TOOL_CLASS_READ);
   assert(tool_class("mcp:remote") == AIMEE_TOOL_CLASS_REMOTE);
   assert(tool_class("not_registered") == AIMEE_TOOL_CLASS_UNKNOWN);
}

static void test_workspace(void)
{
   char max_ref[AIMEE_WORKSPACE_REF_MAX + 1];
   memset(max_ref, 'a', 64);
   max_ref[64] = '/';
   memset(max_ref + 65, 'b', 64);
   max_ref[AIMEE_WORKSPACE_REF_MAX] = '\0';
   uint8_t request[AIMEE_WORKSPACE_REQUEST_LEN], response[AIMEE_WORKSPACE_RESPONSE_LEN];
   uint32_t response_len = 0;
   int allowed = 0;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_WORKSPACE_STAGE_ACCESS};
   assert(aimee_workspace_request_encode(max_ref, strlen(max_ref), request, sizeof(request)) == 0);
   assert(aimee_workspace_get_u16(request + 6) == AIMEE_WORKSPACE_REF_MAX);
   assert(aimee_workspace_module_handler(&invocation, request, sizeof(request), response,
                                         sizeof(response), &response_len,
                                         NULL) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_workspace_response_decode(response, response_len, &allowed) == 0 && allowed);
}

/* The runner frames are written here and read in Go, so the layout is pinned on
 * both sides. A field that moves silently would not fail a build; it would fail
 * as a workspace that resolves to the wrong client. */
static void test_workspace_runner_frames(void)
{
   uint8_t request[AIMEE_WS_RUNNER_REQUEST_LEN];
   assert(aimee_ws_runner_request_encode(AIMEE_WS_RUNNER_OP_RESOLVE, "/srv/repo/src", 13, request,
                                         sizeof(request)) == 0);
   assert(aimee_workspace_get_u32(request) == AIMEE_WS_RUNNER_REQUEST_MAGIC);
   assert(request[4] == AIMEE_WORKSPACE_WIRE_VERSION);
   assert(request[5] == AIMEE_WS_RUNNER_OP_RESOLVE);
   assert(aimee_workspace_get_u16(request + 6) == 13);
   assert(memcmp(request + 8, "/srv/repo/src", 13) == 0);

   /* A registered id is the key the handoff is looked up by, so it is bounded to
    * that key even though a path being asked about is not. */
   char long_id[AIMEE_WS_RUNNER_ID_MAX + 2];
   memset(long_id, 'a', sizeof(long_id) - 1);
   long_id[sizeof(long_id) - 1] = '\0';
   assert(aimee_ws_runner_request_encode(AIMEE_WS_RUNNER_OP_REGISTER, long_id,
                                         AIMEE_WS_RUNNER_ID_MAX + 1, request, sizeof(request)) < 0);
   assert(aimee_ws_runner_request_encode(AIMEE_WS_RUNNER_OP_RESOLVE, long_id,
                                         AIMEE_WS_RUNNER_ID_MAX + 1, request,
                                         sizeof(request)) == 0);

   /* Nobody serving the tree decodes as an empty id, not as a failure. */
   uint8_t reply[AIMEE_WS_RUNNER_RESPONSE_LEN];
   memset(reply, 0, sizeof(reply));
   aimee_workspace_put_u32(reply, AIMEE_WS_RUNNER_RESPONSE_MAGIC);
   char id[AIMEE_WS_RUNNER_ID_MAX + 1] = "stale";
   assert(aimee_ws_runner_response_decode(reply, sizeof(reply), id, sizeof(id)) == 0);
   assert(id[0] == '\0');

   aimee_workspace_put_u32(reply + 4, 9);
   memcpy(reply + 8, "/srv/repo", 9);
   assert(aimee_ws_runner_response_decode(reply, sizeof(reply), id, sizeof(id)) == 0);
   assert(strcmp(id, "/srv/repo") == 0);

   /* An id longer than the frame allows is refused rather than truncated. */
   aimee_workspace_put_u32(reply + 4, AIMEE_WS_RUNNER_ID_MAX + 1);
   assert(aimee_ws_runner_response_decode(reply, sizeof(reply), id, sizeof(id)) < 0);
}

static void test_workspace_runner_io_frames(void)
{
   uint8_t request[AIMEE_WS_IO_HEADER_LEN + AIMEE_WS_RUNNER_ID_MAX + 32];
   const char *body = "{\"op\":\"read\"}";
   size_t n = aimee_ws_io_request_encode(AIMEE_WS_IO_OP_SUBMIT, "/srv/repo", body, strlen(body),
                                         request, sizeof(request));
   assert(n == AIMEE_WS_IO_HEADER_LEN + 9 + strlen(body));
   assert(aimee_workspace_get_u32(request) == AIMEE_WS_IO_REQUEST_MAGIC);
   assert(request[5] == AIMEE_WS_IO_OP_SUBMIT);
   assert(aimee_workspace_get_u16(request + 6) == 9);
   assert(aimee_workspace_get_u32(request + 8) == strlen(body));
   assert(memcmp(request + AIMEE_WS_IO_HEADER_LEN, "/srv/repo", 9) == 0);
   assert(memcmp(request + AIMEE_WS_IO_HEADER_LEN + 9, body, strlen(body)) == 0);

   /* Too small a buffer reports 0 rather than writing past it. */
   assert(aimee_ws_io_request_encode(AIMEE_WS_IO_OP_SUBMIT, "/srv/repo", body, strlen(body),
                                     request, AIMEE_WS_IO_HEADER_LEN) == 0);

   /* "another chunk follows" is what tells a streaming caller to drain again;
    * reading it off the wrong word would end a stream early. */
   uint8_t reply[AIMEE_WS_IO_RESP_HEADER_LEN + 4];
   aimee_workspace_put_u32(reply, AIMEE_WS_IO_RESPONSE_MAGIC);
   aimee_workspace_put_u32(reply + 4, AIMEE_WS_IO_MORE);
   aimee_workspace_put_u32(reply + 8, 4);
   memcpy(reply + AIMEE_WS_IO_RESP_HEADER_LEN, "tok1", 4);
   const uint8_t *chunk = NULL;
   size_t chunk_len = 0;
   int more = 0;
   assert(aimee_ws_io_response_decode(reply, sizeof(reply), &chunk, &chunk_len, &more) == 0);
   assert(more && chunk_len == 4 && memcmp(chunk, "tok1", 4) == 0);

   aimee_workspace_put_u32(reply + 4, 0);
   assert(aimee_ws_io_response_decode(reply, sizeof(reply), &chunk, &chunk_len, &more) == 0);
   assert(!more);

   /* A length that disagrees with the frame is refused, not trusted. */
   aimee_workspace_put_u32(reply + 8, 64);
   assert(aimee_ws_io_response_decode(reply, sizeof(reply), &chunk, &chunk_len, &more) < 0);
}

/* Capability inference is stated twice, here in the module's C mirror and in
 * server-go/modules/delegates/capabilities.go, and nothing in the build keeps
 * them in step. These pin the answers a drifting edit would change. */
static void test_delegates_capability_inference(void)
{
   uint8_t response[AIMEE_DELEGATES_CAP_RESPONSE_LEN];
   uint8_t request[AIMEE_DELEGATES_CAP_HEADER_LEN + 256];
   uint32_t response_len = 0;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DELEGATES_STAGE_CAPABILITIES};

   struct
   {
      const char *prompt;
      int tools;
      unsigned expect;
   } cases[] = {
       /* A file reference needs the modality; describing the work in code does not. */
       {"Transcribe recording.mp3 into text.", 0, AIMEE_DELEGATES_CAP_AUDIO},
       {"Implement an STT dispatcher and audio routing module.", 0, 0},
       {"Analyze screenshot.png", 0, AIMEE_DELEGATES_CAP_VISION},
       {"review this diff: ![alt](docs/diagram)", 0, 0},
       {"read the spec.pdf", 0, AIMEE_DELEGATES_CAP_PDF},
       /* Tools is asserted by the caller, never read out of the prompt. */
       {"", 1, AIMEE_DELEGATES_CAP_TOOLS},
       {"", 0, 0},
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
   {
      size_t n = aimee_delegates_cap_request_encode(cases[i].prompt, strlen(cases[i].prompt),
                                                    cases[i].tools, request, sizeof(request));
      assert(n > 0);
      assert(aimee_delegates_module_handler(&invocation, request, (uint32_t)n, response,
                                            sizeof(response), &response_len,
                                            NULL) == AIMEE_MODULE_STATUS_OK);
      unsigned caps = 0;
      int min_ctx = 0;
      assert(aimee_delegates_cap_response_decode(response, response_len, &caps, &min_ctx) == 0);
      assert(caps == cases[i].expect);
      assert(min_ctx == 0); /* every prompt here is short */
   }

   /* Encode refuses a prompt the buffer cannot hold rather than overrunning it. */
   char big[600];
   memset(big, 'a', sizeof(big) - 1);
   big[sizeof(big) - 1] = '\0';
   assert(aimee_delegates_cap_request_encode(big, strlen(big), 0, request, sizeof(request)) == 0);
}

/* Chain depth is stated twice, here in the module's C mirror and in
 * server-go/modules/delegates/chain.go. These pin both answers. */
static void test_delegates_chain_depth(void)
{
   uint8_t request[AIMEE_DELEGATES_CHAIN_REQUEST_LEN];
   uint8_t response[AIMEE_DELEGATES_CHAIN_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_DELEGATES_STAGE_CHAIN};

   /* An inherited depth is trustworthy only while its chain still exists. The
    * UNKNOWN case must NOT clear: discarding the depth on a failed liveness
    * check would quietly raise the ceiling for everything underneath it. */
   struct
   {
      int has_depth, has_parent, known, active, want;
   } clears[] = {
       {1, 0, 0, 0, 1}, /* depth with no parent marker is a leftover */
       {1, 1, 1, 0, 1}, /* parent known to have exited */
       {1, 1, 1, 1, 0}, /* parent still running */
       {1, 1, 0, 0, 0}, /* liveness unknown: leave it alone */
       {0, 0, 0, 0, 0}, /* nothing inherited */
   };
   for (size_t i = 0; i < sizeof(clears) / sizeof(clears[0]); ++i)
   {
      int flag = -1;
      assert(aimee_delegates_chain_request_encode(
                 AIMEE_DELEGATES_CHAIN_OP_SHOULD_CLEAR, clears[i].has_depth, clears[i].has_parent,
                 clears[i].known, clears[i].active, 0, 0, request, sizeof(request)) == 0);
      assert(aimee_delegates_module_handler(&invocation, request, sizeof(request), response,
                                            sizeof(response), &response_len,
                                            NULL) == AIMEE_MODULE_STATUS_OK);
      assert(aimee_delegates_chain_response_decode(response, response_len, &flag, NULL) == 0);
      assert(flag == clears[i].want);
   }

   /* The depth reported is the CHILD's, so a delegation at the limit is refused
    * before it runs rather than after. */
   struct
   {
      int parent, max, want_depth, want_allowed;
   } depths[] = {
       {0, 3, 1, 1},
       {2, 3, 3, 1},
       {3, 3, 4, 0},
       {0, 0, 1, 0},
   };
   for (size_t i = 0; i < sizeof(depths) / sizeof(depths[0]); ++i)
   {
      int flag = -1;
      int32_t current = -1;
      assert(aimee_delegates_chain_request_encode(AIMEE_DELEGATES_CHAIN_OP_CHECK_DEPTH, 0, 0, 0, 0,
                                                  depths[i].parent, depths[i].max, request,
                                                  sizeof(request)) == 0);
      assert(aimee_delegates_module_handler(&invocation, request, sizeof(request), response,
                                            sizeof(response), &response_len,
                                            NULL) == AIMEE_MODULE_STATUS_OK);
      assert(aimee_delegates_chain_response_decode(response, response_len, &flag, &current) == 0);
      assert(flag == depths[i].want_allowed);
      assert(current == depths[i].want_depth);
   }

   /* A flag byte that is neither 0 nor 1 is not a boolean: refused, not coerced. */
   assert(aimee_delegates_chain_request_encode(AIMEE_DELEGATES_CHAIN_OP_SHOULD_CLEAR, 0, 0, 0, 0, 0,
                                               0, request, sizeof(request)) == 0);
   request[7] = 2;
   assert(aimee_delegates_module_handler(&invocation, request, sizeof(request), response,
                                         sizeof(response), &response_len,
                                         NULL) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
}

static void test_git(void)
{
   uint8_t request[AIMEE_GIT_REQUEST_LEN], response[AIMEE_GIT_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_git_classification_t result;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_GIT_STAGE_OPERATION};
   assert(aimee_git_request_encode("push", request, sizeof(request)) == 0);
   assert(aimee_git_module_handler(&invocation, request, sizeof(request), response,
                                   sizeof(response), &response_len,
                                   NULL) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_git_response_decode(response, response_len, &result) == 0);
   assert(result.operation == AIMEE_GIT_OP_PUSH && result.needs_credentials);

   uint8_t ref_request[AIMEE_GIT_REF_REQUEST_LEN];
   int allowed = 0;
   invocation.stage_id = AIMEE_GIT_STAGE_REF_VALIDATE;
   assert(aimee_git_ref_request_encode("feature/topic-1", ref_request, sizeof(ref_request)) == 0);
   assert(aimee_git_module_handler(&invocation, ref_request, sizeof(ref_request), response,
                                   sizeof(response), &response_len,
                                   NULL) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_git_ref_response_decode(response, response_len, &allowed) == 0 && allowed);
   assert(
       aimee_git_ref_request_encode("aimee/wi/wi_57186250728b511961573e5afb37cc93.s4263a4834d.g0.0",
                                    ref_request, sizeof(ref_request)) == 0);
   assert(aimee_git_module_handler(&invocation, ref_request, sizeof(ref_request), response,
                                   sizeof(response), &response_len,
                                   NULL) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_git_ref_response_decode(response, response_len, &allowed) == 0 && allowed);
   assert(aimee_git_ref_request_encode("-evil", ref_request, sizeof(ref_request)) == 0);
   assert(aimee_git_module_handler(&invocation, ref_request, sizeof(ref_request), response,
                                   sizeof(response), &response_len,
                                   NULL) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_git_ref_response_decode(response, response_len, &allowed) == 0 && !allowed);
}

static void test_skills(void)
{
   uint8_t request[512], response[AIMEE_SKILLS_TRIGGER_RESPONSE_LEN];
   uint32_t response_len = 0;
   int fire = 0;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_SKILLS_STAGE_CONTEXT};
   assert(aimee_skills_request_encode(12, 6, request, sizeof(request)) == 0);
   assert(aimee_skills_module_handler(&invocation, request, AIMEE_SKILLS_REQUEST_LEN, response,
                                      sizeof(response), &response_len,
                                      NULL) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_skills_response_decode(response, response_len, &fire) == 0 && fire);

   const char *content = "---\nname: wait\ntriggers:\n  tool: [Bash]\n"
                         "  arg_pattern: [\"sleep \", \"curl \"]\n---\nWait safely.\n";
   size_t request_len = aimee_skills_trigger_request_size(content, "Bash", "sleep 5");
   assert(request_len > 0 && request_len <= sizeof(request));
   assert(aimee_skills_trigger_request_encode(content, "Bash", "sleep 5", request,
                                              sizeof(request)) == 0);
   invocation.stage_id = AIMEE_SKILLS_STAGE_TRIGGER;
   int match = 0;
   assert(aimee_skills_module_handler(&invocation, request, (uint32_t)request_len, response,
                                      sizeof(response), &response_len,
                                      NULL) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_skills_trigger_response_decode(response, response_len, &match) == 0 && match);
}

static void test_governance(void)
{
   static const char *inactive_tools[] = {"Agent", "read_file"};
   static const char *denied_tools[] = {"spawn_agent", "Task"};
   static const char *partial_tools[] = {"read_file", "RemoteTrigger", "write_file"};
   static const char *derived_tools[] = {"Agent", "bash"};
   static const char *allowed_tools[] = {"agent", "delegate"};
   static const struct
   {
      int active;
      const char *const *tools;
      uint32_t tool_count;
      const char *stop_reason;
      uint32_t keep_mask;
      uint32_t drop_count;
      const char *final_reason;
   } cases[] = {
       {0, inactive_tools, 2, "", 3, 0, ""},
       {1, NULL, 0, "", 0, 0, ""},
       {1, denied_tools, 2, "tool_use", 0, 2, "end_turn"},
       {1, partial_tools, 3, "max_tokens", 5, 1, "max_tokens"},
       {1, derived_tools, 2, "", 2, 1, "tool_use"},
       {1, allowed_tools, 2, "refusal", 3, 0, "refusal"},
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
   {
      uint8_t request[AIMEE_GOVERNANCE_REQUEST_LEN], response[AIMEE_GOVERNANCE_RESPONSE_LEN];
      uint32_t response_len = 0;
      aimee_governance_decision_t decision;
      aimee_module_invocation_t invocation = {.stage_id = AIMEE_GOVERNANCE_STAGE_EVALUATE};
      assert(aimee_governance_request_encode(cases[i].active, cases[i].tools, cases[i].tool_count,
                                             cases[i].stop_reason, request, sizeof(request)) == 0);
      assert(aimee_governance_module_handler(&invocation, request, sizeof(request), response,
                                             sizeof(response), &response_len,
                                             NULL) == AIMEE_MODULE_STATUS_OK);
      assert(aimee_governance_response_decode(response, response_len, cases[i].tool_count,
                                              &decision) == 0);
      assert(decision.keep_mask == cases[i].keep_mask);
      assert(decision.drop_count == cases[i].drop_count);
      assert(strcmp(decision.stop_reason, cases[i].final_reason) == 0);
   }
}

static void test_workflows(void)
{
   static const struct
   {
      const char *bound;
      const char *work_item;
      const char *observed;
      const char *actual_stage;
      const char *actual_state;
      int have_nonce;
      const char *nonce;
      const char *last_nonce;
      aimee_workflows_advance_outcome_t outcome;
   } cases[] = {
       {"wi_1", "wi_1", "understand", "understand", "active", 0, "", "",
        AIMEE_WORKFLOWS_ADVANCE_OK},
       {"", "wi_1", "understand", "understand", "active", 0, "", "",
        AIMEE_WORKFLOWS_ADVANCE_UNBOUND},
       {"wi_2", "wi_1", "understand", "understand", "active", 0, "", "",
        AIMEE_WORKFLOWS_ADVANCE_UNBOUND},
       {"wi_1", "wi_1", "understand", "split", "active", 0, "", "", AIMEE_WORKFLOWS_ADVANCE_STALE},
       {"wi_1", "wi_1", "understand", "understand", "accepted", 0, "", "",
        AIMEE_WORKFLOWS_ADVANCE_TERMINAL},
       {"wi_1", "", "understand", "understand", "active", 0, "", "",
        AIMEE_WORKFLOWS_ADVANCE_BADARGS},
       {"wi_1", "wi_1", "understand", "split", "accepted", 1, "nonce_1", "nonce_1",
        AIMEE_WORKFLOWS_ADVANCE_REPLAY},
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
   {
      uint8_t request[AIMEE_WORKFLOWS_REQUEST_LEN], response[AIMEE_WORKFLOWS_RESPONSE_LEN];
      uint32_t response_len = 0;
      aimee_workflows_advance_outcome_t outcome;
      aimee_module_invocation_t invocation = {.stage_id = AIMEE_WORKFLOWS_STAGE_ADVANCE};
      assert(aimee_workflows_request_encode(cases[i].bound, cases[i].work_item, cases[i].observed,
                                            cases[i].actual_stage, cases[i].actual_state,
                                            cases[i].have_nonce, cases[i].nonce,
                                            cases[i].last_nonce, request, sizeof(request)) == 0);
      assert(aimee_workflows_module_handler(&invocation, request, sizeof(request), response,
                                            sizeof(response), &response_len,
                                            NULL) == AIMEE_MODULE_STATUS_OK);
      assert(aimee_workflows_response_decode(response, response_len, &outcome) == 0);
      assert(outcome == cases[i].outcome);
   }
}

static void test_roundtable(void)
{
   static const struct
   {
      aimee_roundtable_replay_status_t status;
      int factual;
      const char *claimed;
      aimee_roundtable_verify_action_t action;
      const char *severity;
   } cases[] = {
       {AIMEE_ROUNDTABLE_REPLAY_CONTRADICTED, 1, "blocking", AIMEE_ROUNDTABLE_VERIFY_REJECT, ""},
       {AIMEE_ROUNDTABLE_REPLAY_VACUOUS, 1, "blocking", AIMEE_ROUNDTABLE_VERIFY_REJECT, ""},
       {AIMEE_ROUNDTABLE_REPLAY_INDEX_UNAVAILABLE, 1, "blocking", AIMEE_ROUNDTABLE_VERIFY_DEGRADE,
        "blocking"},
       {AIMEE_ROUNDTABLE_REPLAY_NO_EVIDENCE, 0, "blocking", AIMEE_ROUNDTABLE_VERIFY_CAP,
        "suggestion"},
       {AIMEE_ROUNDTABLE_REPLAY_MATCH, 1, "blocking", AIMEE_ROUNDTABLE_VERIFY_KEEP, "blocking"},
       {AIMEE_ROUNDTABLE_REPLAY_MATCH, 0, "blocking", AIMEE_ROUNDTABLE_VERIFY_CAP, "suggestion"},
       {AIMEE_ROUNDTABLE_REPLAY_CORRECTED, 1, "nit", AIMEE_ROUNDTABLE_VERIFY_KEEP, "nit"},
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
   {
      uint8_t request[AIMEE_ROUNDTABLE_REQUEST_LEN], response[AIMEE_ROUNDTABLE_RESPONSE_LEN];
      uint32_t response_len = 0;
      char severity[AIMEE_ROUNDTABLE_SEVERITY_MAX + 1u];
      aimee_roundtable_verify_action_t action;
      aimee_module_invocation_t invocation = {.stage_id = AIMEE_ROUNDTABLE_STAGE_DELIBERATE};
      assert(aimee_roundtable_request_encode(cases[i].status, cases[i].factual, cases[i].claimed,
                                             request, sizeof(request)) == 0);
      assert(aimee_roundtable_module_handler(&invocation, request, sizeof(request), response,
                                             sizeof(response), &response_len,
                                             NULL) == AIMEE_MODULE_STATUS_OK);
      assert(aimee_roundtable_response_decode(response, response_len, &action, severity,
                                              sizeof(severity)) == 0);
      assert(action == cases[i].action && strcmp(severity, cases[i].severity) == 0);
   }
}

static void test_kb_synthesis(void)
{
   static const char *none_string[] = {"No Side Effects"};
   static const char *none_array[] = {"none", "n/a"};
   static const char *honest_string[] = {"writes to disk"};
   static const char *mixed_array[] = {"none", "network"};
   static const char *write_callees[] = {"strlen", "write"};
   static const char *socket_callees[] = {"socket"};
   static const char *ordered_callees[] = {"strlen", "PQexec", "write"};
   static const char *clean_callees[] = {"strlen", "memcpy"};
   static const char *case_callees[] = {"Write", "pqexec"};
   static const struct
   {
      aimee_kb_synthesis_claim_kind_t kind;
      const char *const *claims;
      uint32_t claim_count;
      const char *const *callees;
      uint32_t callee_count;
      int contradicts;
      const char *reason;
   } cases[] = {
       {AIMEE_KB_SYNTHESIS_CLAIM_NONE, NULL, 0, write_callees, 2, 1, "write"},
       {AIMEE_KB_SYNTHESIS_CLAIM_STRING, none_string, 1, socket_callees, 1, 1, "socket"},
       {AIMEE_KB_SYNTHESIS_CLAIM_STRING_ARRAY, none_array, 2, ordered_callees, 3, 1, "PQexec"},
       {AIMEE_KB_SYNTHESIS_CLAIM_STRING_ARRAY, NULL, 0, clean_callees, 2, 0, ""},
       {AIMEE_KB_SYNTHESIS_CLAIM_STRING, honest_string, 1, write_callees, 2, 0, ""},
       {AIMEE_KB_SYNTHESIS_CLAIM_STRING_ARRAY, mixed_array, 2, socket_callees, 1, 0, ""},
       {AIMEE_KB_SYNTHESIS_CLAIM_NONSTRING, NULL, 0, write_callees, 2, 0, ""},
       {AIMEE_KB_SYNTHESIS_CLAIM_NONE, NULL, 0, case_callees, 2, 0, ""},
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
   {
      uint8_t request[AIMEE_KB_SYNTHESIS_REQUEST_LEN];
      uint8_t response[AIMEE_KB_SYNTHESIS_RESPONSE_LEN];
      uint32_t response_len = 0;
      aimee_kb_synthesis_grounding_decision_t decision;
      aimee_module_invocation_t invocation = {.stage_id = AIMEE_KB_SYNTHESIS_STAGE_GROUNDING};
      assert(aimee_kb_synthesis_request_encode(cases[i].kind, cases[i].claims, cases[i].claim_count,
                                               cases[i].callees, cases[i].callee_count, request,
                                               sizeof(request)) == 0);
      assert(aimee_kb_synthesis_module_handler(&invocation, request, sizeof(request), response,
                                               sizeof(response), &response_len,
                                               NULL) == AIMEE_MODULE_STATUS_OK);
      assert(aimee_kb_synthesis_response_decode(response, response_len, &decision) == 0);
      assert(decision.contradicts == cases[i].contradicts);
      assert(strcmp(decision.reason, cases[i].reason) == 0);
   }
}

static void test_runtime_web(void)
{
   static const struct
   {
      const char *kind;
      uint32_t status;
   } cases[] = {
       {"invalid_argument", 400u}, {"not_found", 404u}, {"permission_denied", 403u},
       {"unavailable", 503u},      {"", 502u},          {"unknown", 502u},
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
   {
      uint8_t request[AIMEE_RUNTIME_WEB_REQUEST_LEN];
      uint8_t response[AIMEE_RUNTIME_WEB_RESPONSE_LEN];
      uint32_t response_len = 0, status = 0;
      aimee_module_invocation_t invocation = {.stage_id = AIMEE_RUNTIME_WEB_STAGE_CLASSIFY};
      assert(aimee_runtime_web_request_encode(cases[i].kind, request, sizeof(request)) == 0);
      assert(aimee_runtime_web_module_handler(&invocation, request, sizeof(request), response,
                                              sizeof(response), &response_len,
                                              NULL) == AIMEE_MODULE_STATUS_OK);
      assert(aimee_runtime_web_response_decode(response, response_len, &status) == 0);
      assert(status == cases[i].status);
   }
}

static void test_control_web(void)
{
   static const struct
   {
      aimee_control_web_target_t target;
      const char *method;
      const char *path;
      int allowed;
   } cases[] = {
       {AIMEE_CONTROL_WEB_TARGET_CONSOLE_ADMIN, "GET", "/v1/console/overview", 1},
       {AIMEE_CONTROL_WEB_TARGET_CONSOLE_ADMIN, "POST", "/v1/enrollments/abc/revoke", 1},
       {AIMEE_CONTROL_WEB_TARGET_CONSOLE_ADMIN, "GET", "/v1/enrollments/", 1},
       {AIMEE_CONTROL_WEB_TARGET_CONSOLE_ADMIN, "GET", "/v1/servers/s1/health", 0},
       {AIMEE_CONTROL_WEB_TARGET_CONSOLE_ADMIN, "GET", "/v1/%65nrollments", 0},
       {AIMEE_CONTROL_WEB_TARGET_FLEET, "GET", "/v1/servers/s1/health", 1},
       {AIMEE_CONTROL_WEB_TARGET_FLEET, "POST", "/v1/servers/s1/actions", 1},
       {AIMEE_CONTROL_WEB_TARGET_FLEET, "GET", "/v1/servers/", 0},
       {AIMEE_CONTROL_WEB_TARGET_FLEET, "POST", "/v1/servers/s1/config", 0},
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
   {
      uint8_t request[AIMEE_CONTROL_WEB_REQUEST_LEN];
      uint8_t response[AIMEE_CONTROL_WEB_RESPONSE_LEN];
      uint32_t response_len = 0;
      int allowed = 0;
      aimee_module_invocation_t invocation = {.stage_id = AIMEE_CONTROL_WEB_STAGE_AUTHORIZE};
      assert(aimee_control_web_request_encode(cases[i].target, cases[i].method, cases[i].path,
                                              request, sizeof(request)) == 0);
      assert(aimee_control_web_module_handler(&invocation, request, sizeof(request), response,
                                              sizeof(response), &response_len,
                                              NULL) == AIMEE_MODULE_STATUS_OK);
      assert(aimee_control_web_response_decode(response, response_len, &allowed) == 0);
      assert(allowed == cases[i].allowed);
   }
}

static void test_benchmarks(void)
{
   static const int64_t perfect_retrieved[] = {11, 22, 33};
   static const int64_t perfect_relevant[] = {11, 22, 33};
   static const int64_t rank_two_retrieved[] = {5, 9, 7};
   static const int64_t rank_two_relevant[] = {9};
   static const int64_t duplicate_retrieved[] = {7, 7};
   static const int64_t duplicate_relevant[] = {7};
   static const struct
   {
      const int64_t *retrieved;
      uint32_t retrieved_count;
      const int64_t *relevant;
      uint32_t relevant_count;
      uint32_t k;
      double mrr;
      double ndcg;
      double recall;
   } cases[] = {
       {perfect_retrieved, 3, perfect_relevant, 3, 3, 1.0, 1.0, 1.0},
       {rank_two_retrieved, 3, rank_two_relevant, 1, 3, 0.5, 0.6309297535714574, 1.0},
       {duplicate_retrieved, 2, duplicate_relevant, 1, 2, 1.0, 1.6309297535714575, 2.0},
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
   {
      uint8_t request[AIMEE_BENCHMARKS_REQUEST_LEN], response[AIMEE_BENCHMARKS_RESPONSE_LEN];
      uint32_t response_len = 0;
      aimee_benchmarks_ir_scores_t scores;
      aimee_module_invocation_t invocation = {.stage_id = AIMEE_BENCHMARKS_STAGE_RUN};
      assert(aimee_benchmarks_request_encode(cases[i].retrieved, cases[i].retrieved_count,
                                             cases[i].relevant, cases[i].relevant_count, cases[i].k,
                                             request, sizeof(request)) == 0);
      assert(aimee_benchmarks_module_handler(&invocation, request, sizeof(request), response,
                                             sizeof(response), &response_len,
                                             NULL) == AIMEE_MODULE_STATUS_OK);
      assert(aimee_benchmarks_response_decode(response, response_len, &scores) == 0);
      assert(fabs(scores.mrr - cases[i].mrr) < 1e-12);
      assert(fabs(scores.ndcg - cases[i].ndcg) < 1e-12);
      assert(fabs(scores.recall - cases[i].recall) < 1e-12);
   }

   static const double latencies[] = {10.0, 1.0, 5.0, 3.0, 8.0};
   uint8_t latency_request[AIMEE_BENCHMARKS_LATENCY_REQUEST_LEN];
   uint8_t latency_response[AIMEE_BENCHMARKS_LATENCY_RESPONSE_LEN];
   uint32_t latency_response_len = 0;
   aimee_benchmarks_latency_summary_t summary;
   aimee_module_invocation_t latency_invocation = {.stage_id = AIMEE_BENCHMARKS_STAGE_LATENCY};
   assert(aimee_benchmarks_latency_request_encode(latencies, 5, latency_request,
                                                  sizeof(latency_request)) == 0);
   assert(aimee_benchmarks_module_handler(
              &latency_invocation, latency_request, sizeof(latency_request), latency_response,
              sizeof(latency_response), &latency_response_len, NULL) == AIMEE_MODULE_STATUS_OK);
   assert(aimee_benchmarks_latency_response_decode(latency_response, latency_response_len,
                                                   &summary) == 0);
   assert(summary.queries == 5 && summary.p50_ms == 5.0 && summary.p95_ms == 10.0 &&
          summary.p99_ms == 10.0 && summary.min_ms == 1.0 && summary.max_ms == 10.0);
}

/* Providers: the precedence rules that used to be four hand-written copies.
 *
 * Each case pins a rule that a "value > 0" check gets wrong, which is how the
 * copies drifted apart in the first place. */
static void providers_call(uint32_t stage, const uint8_t *req, uint32_t req_len, uint8_t *resp,
                           uint32_t *resp_len)
{
   aimee_module_invocation_t invocation = {.stage_id = stage};
   assert(aimee_providers_module_handler(&invocation, req, req_len, resp,
                                         AIMEE_PROVIDERS_RESPONSE_LEN, resp_len,
                                         NULL) == AIMEE_MODULE_STATUS_OK);
   assert(*resp_len == AIMEE_PROVIDERS_RESPONSE_LEN);
   assert(aimee_providers_get_u32(resp) == AIMEE_PROVIDERS_RESPONSE_MAGIC);
}

static void providers_init(uint8_t *req, uint32_t len)
{
   memset(req, 0, len);
   aimee_providers_put_u32(req, AIMEE_PROVIDERS_REQUEST_MAGIC);
   aimee_providers_put_u32(req + 4, AIMEE_PROVIDERS_WIRE_VERSION);
}

static void test_providers(void)
{
   uint8_t req[AIMEE_PROVIDERS_RESOLVE_REQUEST_LEN];
   uint8_t resp[AIMEE_PROVIDERS_RESPONSE_LEN];
   uint32_t resp_len = 0;
   const uint8_t *rec = resp + AIMEE_PROVIDERS_RESPONSE_HEADER_LEN;

   /* 1. A DECLARED ZERO PRICE WINS. The seat is free; a "> 0" test would have
    *    discarded the statement and reported the price as unknown. */
   providers_init(req, sizeof req);
   uint8_t *decl = req + AIMEE_PROVIDERS_OFF_DECLARED_RECORD;
   uint8_t *fetch = req + AIMEE_PROVIDERS_OFF_FETCHED_RECORD;
   assert(aimee_providers_put_str(decl + AIMEE_PROVIDERS_OFF_PROVIDER, AIMEE_PROVIDERS_NAME_MAX,
                                  "anthropic") == 0);
   assert(aimee_providers_put_str(decl + AIMEE_PROVIDERS_OFF_MODEL, AIMEE_PROVIDERS_MODEL_MAX,
                                  "claude-sonnet-5") == 0);
   aimee_providers_put_u64(decl + AIMEE_PROVIDERS_OFF_PRICE_IN, 0ull);
   aimee_providers_put_u32(decl + AIMEE_PROVIDERS_OFF_DECLARED, AIMEE_PROVIDERS_DECL_PRICE_IN);
   providers_call(AIMEE_PROVIDERS_STAGE_RESOLVE, req, sizeof req, resp, &resp_len);
   assert(aimee_providers_get_u32(resp + 8) == AIMEE_PROVIDERS_OK);
   assert(aimee_providers_get_u64(rec + AIMEE_PROVIDERS_OFF_PRICE_IN) == 0ull);
   assert(rec[AIMEE_PROVIDERS_OFF_PRICE_SRC] == AIMEE_PROVIDERS_SRC_DECLARED);

   /* 2. A DECLARED ZERO CAPACITY DOES NOT WIN -- the opposite rule, on purpose.
    *    There is no zero-token window, so the provider's real number stands. */
   providers_init(req, sizeof req);
   assert(aimee_providers_put_str(decl + AIMEE_PROVIDERS_OFF_MODEL, AIMEE_PROVIDERS_MODEL_MAX,
                                  "m") == 0);
   aimee_providers_put_u32(decl + AIMEE_PROVIDERS_OFF_CONTEXT, 0u);
   aimee_providers_put_u32(decl + AIMEE_PROVIDERS_OFF_DECLARED,
                           AIMEE_PROVIDERS_DECL_CONTEXT_WINDOW);
   assert(aimee_providers_put_str(fetch + AIMEE_PROVIDERS_OFF_MODEL, AIMEE_PROVIDERS_MODEL_MAX,
                                  "m") == 0);
   aimee_providers_put_u32(fetch + AIMEE_PROVIDERS_OFF_CONTEXT, 1000000u);
   providers_call(AIMEE_PROVIDERS_STAGE_RESOLVE, req, sizeof req, resp, &resp_len);
   assert(aimee_providers_get_u32(rec + AIMEE_PROVIDERS_OFF_CONTEXT) == 1000000u);
   assert(rec[AIMEE_PROVIDERS_OFF_CONTEXT_SRC] == AIMEE_PROVIDERS_SRC_FETCHED);

   /* 3. A real declared capacity outranks the provider, and says so. */
   aimee_providers_put_u32(decl + AIMEE_PROVIDERS_OFF_CONTEXT, 200000u);
   providers_call(AIMEE_PROVIDERS_STAGE_RESOLVE, req, sizeof req, resp, &resp_len);
   assert(aimee_providers_get_u32(rec + AIMEE_PROVIDERS_OFF_CONTEXT) == 200000u);
   assert(rec[AIMEE_PROVIDERS_OFF_CONTEXT_SRC] == AIMEE_PROVIDERS_SRC_DECLARED);

   /* 4. Nobody knows: unknown, not a confident zero. */
   providers_init(req, sizeof req);
   assert(aimee_providers_put_str(decl + AIMEE_PROVIDERS_OFF_MODEL, AIMEE_PROVIDERS_MODEL_MAX,
                                  "m") == 0);
   providers_call(AIMEE_PROVIDERS_STAGE_RESOLVE, req, sizeof req, resp, &resp_len);
   assert(aimee_providers_get_u32(rec + AIMEE_PROVIDERS_OFF_CONTEXT) == 0u);
   assert(rec[AIMEE_PROVIDERS_OFF_CONTEXT_SRC] == AIMEE_PROVIDERS_SRC_UNKNOWN);

   /* 5. Two records naming DIFFERENT models are refused rather than merged --
    *    merging attributes one model's limits to another. */
   providers_init(req, sizeof req);
   assert(aimee_providers_put_str(decl + AIMEE_PROVIDERS_OFF_MODEL, AIMEE_PROVIDERS_MODEL_MAX,
                                  "sonnet") == 0);
   assert(aimee_providers_put_str(fetch + AIMEE_PROVIDERS_OFF_MODEL, AIMEE_PROVIDERS_MODEL_MAX,
                                  "opus") == 0);
   providers_call(AIMEE_PROVIDERS_STAGE_RESOLVE, req, sizeof req, resp, &resp_len);
   assert(aimee_providers_get_u32(resp + 8) == AIMEE_PROVIDERS_ERR_IDENTITY_MISMATCH);
   assert(aimee_providers_get_u32(resp + 12) == 0u); /* no record on a refusal */

   /* 6. Deprecation is the union: either side is enough to retire a model. */
   providers_init(req, sizeof req);
   assert(aimee_providers_put_str(decl + AIMEE_PROVIDERS_OFF_MODEL, AIMEE_PROVIDERS_MODEL_MAX,
                                  "m") == 0);
   assert(aimee_providers_put_str(fetch + AIMEE_PROVIDERS_OFF_MODEL, AIMEE_PROVIDERS_MODEL_MAX,
                                  "m") == 0);
   fetch[AIMEE_PROVIDERS_OFF_DEPRECATED] = 1;
   providers_call(AIMEE_PROVIDERS_STAGE_RESOLVE, req, sizeof req, resp, &resp_len);
   assert(rec[AIMEE_PROVIDERS_OFF_DEPRECATED] == 1);

   /* 7. VALIDATE normalizes a cleared capacity to "not declared" rather than
    *    rejecting it -- clearing a form field must remain expressible. */
   uint8_t vreq[AIMEE_PROVIDERS_VALIDATE_REQUEST_LEN];
   providers_init(vreq, sizeof vreq);
   uint8_t *prop = vreq + 8;
   assert(aimee_providers_put_str(prop + AIMEE_PROVIDERS_OFF_PROVIDER, AIMEE_PROVIDERS_NAME_MAX,
                                  "anthropic") == 0);
   assert(aimee_providers_put_str(prop + AIMEE_PROVIDERS_OFF_MODEL, AIMEE_PROVIDERS_MODEL_MAX,
                                  "m") == 0);
   aimee_providers_put_u32(prop + AIMEE_PROVIDERS_OFF_CONTEXT, 0u);
   aimee_providers_put_u32(prop + AIMEE_PROVIDERS_OFF_DECLARED,
                           AIMEE_PROVIDERS_DECL_CONTEXT_WINDOW);
   providers_call(AIMEE_PROVIDERS_STAGE_VALIDATE, vreq, sizeof vreq, resp, &resp_len);
   assert(aimee_providers_get_u32(resp + 8) == AIMEE_PROVIDERS_OK);
   assert(!(aimee_providers_get_u32(rec + AIMEE_PROVIDERS_OFF_DECLARED) &
            AIMEE_PROVIDERS_DECL_CONTEXT_WINDOW));

   /* 8. ...but an output ceiling above the window is REFUSED, not shrunk: the
    *    window bounds prompt and completion together, so it cannot be true, and
    *    silently fixing it would hide the operator's mistake. */
   aimee_providers_put_u32(prop + AIMEE_PROVIDERS_OFF_CONTEXT, 1000u);
   aimee_providers_put_u32(prop + AIMEE_PROVIDERS_OFF_MAX_OUTPUT, 2000u);
   aimee_providers_put_u32(prop + AIMEE_PROVIDERS_OFF_DECLARED,
                           AIMEE_PROVIDERS_DECL_CONTEXT_WINDOW | AIMEE_PROVIDERS_DECL_MAX_OUTPUT);
   providers_call(AIMEE_PROVIDERS_STAGE_VALIDATE, vreq, sizeof vreq, resp, &resp_len);
   assert(aimee_providers_get_u32(resp + 8) == AIMEE_PROVIDERS_ERR_INVALID_DECLARATION);

   /* 9. A malformed envelope is a TRANSPORT error, not a rule result. */
   {
      aimee_module_invocation_t bad = {.stage_id = AIMEE_PROVIDERS_STAGE_RESOLVE};
      providers_init(req, sizeof req);
      aimee_providers_put_u32(req, 0xdeadbeefu);
      assert(aimee_providers_module_handler(&bad, req, sizeof req, resp, sizeof resp, &resp_len,
                                            NULL) == AIMEE_MODULE_STATUS_INVALID_REQUEST);
   }
}

int main(void)
{
   test_memory();
   test_learning();
   test_delegates();
   test_tools();
   test_workspace();
   test_workspace_runner_frames();
   test_workspace_runner_io_frames();
   test_delegates_capability_inference();
   test_delegates_chain_depth();
   test_git();
   test_skills();
   test_governance();
   test_workflows();
   test_roundtable();
   test_kb_synthesis();
   test_runtime_web();
   test_control_web();
   test_benchmarks();
   test_providers();
   puts("process module handlers: PASS");
   return 0;
}
