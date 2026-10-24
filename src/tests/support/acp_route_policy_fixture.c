/* Explicit in-process backend service peer for the public ACP fixture.
 * The actual C execution-policy caller, governed-action owner, source claim and
 * dispatcher remain linked. This is not a real Go/store receipt qualification.
 */
#include "aimee.h"
#include "aimee_sha256.h"
#include "module_commands.h"
#include "module_json_call.h"
#include "request_context.h"
#include "server.h"
#include "server_session_capture.h"
#include <aimee/execution-policy/module_api.h>
#include <aimee/tools/module_api.h>
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[PATH_MAX], actor_name[256], session_name[128];
static cJSON *issued, *issued_arguments;
static int admitted, sent, dispatches, outcomes, writes, fixture_fault;
void acp_route_policy_fault(int fault)
{
   fixture_fault = fault;
}
static const char *text(const cJSON *value, const char *key)
{
   const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(value, key));
   return s ? s : "";
}
static int principal_matches(void)
{
   const request_context_t *ctx = request_context_get();
   if (ctx && !strcmp(request_context_session_actor(ctx), actor_name))
      assert(ctx->model_execution && !ctx->operator_script_origin &&
             !(ctx->capabilities & ~(uint32_t)CAPS_AUTHENTICATED));
   return ctx && !strcmp(request_context_session_actor(ctx), actor_name) &&
          !strcmp(ctx->principal, actor_name) && !ctx->context_refused;
}
void acp_route_policy_reset(const char *workspace, const char *actor, const char *session)
{
   assert(workspace && workspace[0] == '/' && strlen(workspace) < sizeof root);
   assert(actor && actor[0] && strlen(actor) < sizeof actor_name);
   assert(session && session[0] && strlen(session) < sizeof session_name);
   snprintf(root, sizeof root, "%s", workspace);
   snprintf(actor_name, sizeof actor_name, "%s", actor);
   snprintf(session_name, sizeof session_name, "%s", session);
   cJSON_Delete(issued);
   issued = NULL;
   cJSON_Delete(issued_arguments);
   issued_arguments = NULL;
   admitted = sent = dispatches = outcomes = writes = fixture_fault = 0;
}
int acp_route_policy_dispatches(void)
{
   return dispatches;
}
int acp_route_policy_outcomes(void)
{
   return outcomes;
}
int acp_route_policy_writes(void)
{
   return writes;
}

/* The resource peer resolves exactly the fixture's flat file targets. Refusing
 * deeper paths here keeps this peer narrower than the real tools module. */
static int resource_path(const cJSON *args, char path[PATH_MAX])
{
   const char *p = text(args, "path");
   int n =
       p[0] == '/' ? snprintf(path, PATH_MAX, "%s", p) : snprintf(path, PATH_MAX, "%s/%s", root, p);
   size_t size = strlen(root);
   if (!p[0] || n < 0 || n >= PATH_MAX || strncmp(path, root, size) || path[size] != '/' ||
       !path[size + 1] || strchr(path + size + 1, '/') || !strcmp(path + size + 1, ".") ||
       !strcmp(path + size + 1, ".."))
      return -1;
   struct stat st;
   return !lstat(path, &st) && !S_ISREG(st.st_mode) ? -1 : 0;
}
static int file_tool(const char *tool)
{
   return !strcmp(tool, "read_file") || !strcmp(tool, "write_file");
}
cJSON *aimee_module_json_call(uint32_t event, uint32_t stage, cJSON *request, size_t max_body,
                              int timeout, aimee_module_call_result_t *result)
{
   (void)max_body;
   (void)timeout;
   const cJSON *args = cJSON_GetObjectItemCaseSensitive(request, "arguments");
   char path[PATH_MAX];
   int allowed = event == AIMEE_EXECUTION_POLICY_EVENT_TOOL &&
                 stage == AIMEE_EXECUTION_POLICY_STAGE_TOOL && principal_matches() &&
                 file_tool(text(request, "tool")) && !resource_path(args, path);
   if (fixture_fault == 3 && event == AIMEE_EXECUTION_POLICY_EVENT_TOOL &&
       stage == AIMEE_EXECUTION_POLICY_STAGE_TOOL && principal_matches() &&
       !strcmp(text(request, "tool"), "bash"))
      allowed = !strncmp(text(args, "command"), "aimee --json tool call write_file ",
                         strlen("aimee --json tool call write_file "));
   if (fixture_fault == 4 && event == AIMEE_EXECUTION_POLICY_EVENT_TOOL &&
       stage == AIMEE_EXECUTION_POLICY_STAGE_TOOL && principal_matches() &&
       !strcmp(text(request, "tool"), "bash"))
      allowed = !strcmp(text(args, "command"),
                        "aimee --json tool call read_file path=owned.txt raw=true") ||
                !strcmp(text(args, "command"),
                        "aimee --json tool call bash --arguments '{\"command\":\"aimee --json tool "
                        "call read_file path=owned.txt raw=true\"}'");
   if (fixture_fault == 2 && !strcmp(text(request, "tool"), "write_file"))
      allowed = 0;
   cJSON *reply = cJSON_CreateObject();
   assert(reply && cJSON_AddBoolToObject(reply, "allowed", allowed) &&
          cJSON_AddStringToObject(
              reply, "reason", allowed ? "fixture bounded file policy" : "fixture policy refused"));
   cJSON_Delete(request); /* Same ownership contract as the actual bus caller. */
   if (result)
      *result = AIMEE_MODULE_CALL_OK;
   return reply;
}
cJSON *acp_route_resource_call(uint32_t event, uint32_t stage, const char *verb,
                               const cJSON *request, const cJSON *caller)
{
   if (event != AIMEE_TOOLS_EVENT_ACTION_RESOURCE || stage != AIMEE_TOOLS_STAGE_ACTION_RESOURCE ||
       !principal_matches() ||
       !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(caller, "authenticated")) ||
       strcmp(text(caller, "principal"), actor_name) || strcmp(text(request, "directory"), root) ||
       !file_tool(text(request, "tool")))
      return NULL;
   const cJSON *args = cJSON_GetObjectItemCaseSensitive(request, "arguments");
   char path[PATH_MAX], destination[PATH_MAX + 8], digest[65];
   if (resource_path(args, path))
      return NULL;
   const char *content = text(args, "content");
   if (aimee_sha256_hex(content, strlen(content), digest))
      return NULL;
   snprintf(destination, sizeof destination, "file:%s", path);
   cJSON *reply = cJSON_CreateObject();
   assert(reply);
   if (!strcmp(verb, "describe"))
   {
      cJSON *effective = cJSON_Duplicate(args, 1);
      assert(effective);
      cJSON_DeleteItemFromObjectCaseSensitive(effective, "path");
      assert(cJSON_AddStringToObject(effective, "path", path));
      assert(cJSON_AddStringToObject(reply, "class",
                                     !strcmp(text(request, "tool"), "write_file") ? "file_write"
                                                                                  : "read_only"));
      assert(cJSON_AddStringToObject(reply, "destination", destination));
      assert(cJSON_AddStringToObject(reply, "payload_sha256", digest));
      char size[32];
      snprintf(size, sizeof size, "%zu", strlen(content));
      assert(cJSON_AddStringToObject(reply, "request_bytes", size));
      assert(cJSON_AddItemToObject(reply, "effective_arguments", effective));
      return reply;
   }
   if (strcmp(verb, "verify") || strcmp(text(request, "destination"), destination) ||
       strcmp(text(request, "payload_sha256"), digest))
   {
      cJSON_Delete(reply);
      return NULL;
   }
   int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
   struct stat st;
   size_t size = strlen(content), offset = 0;
   int matches = fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size >= 0 &&
                 (unsigned long long)st.st_size == size;
   char buffer[4096];
   while (matches && offset < size)
   {
      ssize_t count =
          read(fd, buffer, size - offset < sizeof buffer ? size - offset : sizeof buffer);
      if (count <= 0 || memcmp(buffer, content + offset, (size_t)count))
         matches = 0;
      else
         offset += (size_t)count;
   }
   if (fd >= 0)
      close(fd);
   if (!matches)
   {
      cJSON_Delete(reply);
      return NULL;
   }
   writes++;
   assert(cJSON_AddStringToObject(reply, "state", "effect_confirmed") &&
          cJSON_AddStringToObject(reply, "destination", destination) &&
          cJSON_AddStringToObject(reply, "payload_sha256", digest) &&
          cJSON_AddStringToObject(reply, "object_version", digest));
   return reply;
}
static int same_json(const cJSON *a, const cJSON *b)
{
   return a && b && cJSON_Compare(a, b, 1);
}
int db1_governed_action_apply(const char *principal, const char *sid, const char *wire,
                              char *output, size_t cap)
{
   assert(principal_matches() && !strcmp(principal, actor_name) && !strcmp(sid, session_name));
   cJSON *request = cJSON_Parse(wire), *reply = cJSON_CreateObject();
   assert(request && reply);
   server_session_capture_t *capture = server_session_capture_current();
   assert(capture);
   assert(!strcmp(text(request, "operation"), "outcome") ||
          server_session_capture_accepting(capture));
   assert(!strcmp(text(request, "turn_session_id"), server_session_capture_session(capture)) &&
          !strcmp(text(request, "turn_id"), server_session_capture_turn(capture)));
   const char *operation = text(request, "operation");
   if (!strcmp(operation, "issue"))
   {
      assert(!issued || (sent && outcomes == dispatches));
      cJSON_Delete(issued);
      issued = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(request, "intent"), 1);
      assert(issued);
      assert(file_tool(text(issued, "tool")) && text(issued, "attempt_id")[0] &&
             !strcmp(text(issued, "request_id"), request_context_get()->request_id));
      assert(cJSON_AddStringToObject(issued, "action_id", "fixture-action") &&
             cJSON_AddStringToObject(issued, "policy_generation", "fixture-policy"));
      assert(cJSON_AddStringToObject(issued, "turn_session_id", text(request, "turn_session_id")) &&
             cJSON_AddStringToObject(issued, "turn_id", text(request, "turn_id")));
      cJSON *receipt = cJSON_AddObjectToObject(reply, "receipt");
      assert(receipt && cJSON_AddItemToObject(receipt, "intent", cJSON_Duplicate(issued, 1)));
      cJSON_Delete(issued_arguments);
      issued_arguments = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(request, "arguments"), 1);
      assert(issued_arguments);
      admitted = sent = 0;
   }
   else
   {
      assert(same_json(issued, cJSON_GetObjectItemCaseSensitive(request, "intent")));
      if (!strcmp(operation, "admit") || !strcmp(operation, "dispatch"))
         assert(
             same_json(issued_arguments, cJSON_GetObjectItemCaseSensitive(request, "arguments")));
      if (!strcmp(operation, "admit"))
      {
         assert(!admitted && !sent);
         admitted = 1;
      }
      else if (!strcmp(operation, "dispatch"))
      {
         assert(admitted && !sent);
         sent = 1;
         dispatches++;
         assert(cJSON_AddBoolToObject(reply, "dispatch_allowed", 1));
      }
      else
      {
         assert(!strcmp(operation, "outcome") && admitted && sent);
         assert(outcomes < dispatches);
         if (fixture_fault == 1)
         {
            cJSON_Delete(request);
            cJSON_Delete(reply);
            return -1;
         }
         outcomes++;
         const cJSON *outcome = cJSON_GetObjectItemCaseSensitive(request, "outcome");
         assert(!strcmp(text(outcome, "state"), "effect_confirmed") ||
                !strcmp(text(outcome, "state"), "acknowledged") ||
                !strcmp(text(outcome, "state"), "outcome_unknown"));
      }
   }
   assert(cJSON_AddBoolToObject(reply, "allowed", 1));
   char *encoded = cJSON_PrintUnformatted(reply);
   assert(encoded && strlen(encoded) < cap);
   snprintf(output, cap, "%s", encoded);
   free(encoded);
   cJSON_Delete(request);
   cJSON_Delete(reply);
   return 0;
}
