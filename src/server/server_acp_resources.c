/* ACP client filesystem requests are external requests, not observations of an
 * upstream model generation. Execute only their writer-committed original call
 * through the existing tool policy and final-action owners. */
#include "server_acp_resources.h"
#include "server_native_tool_source.h"
#include "server_native_tool_admission.h"
#include "db1_client/primary_sessions.h"
#include "request_context.h"
#include "agent_exec.h"
#include "agent_config.h"
#include "tool_args_coerce.h"
#include "aimee_sha256.h"
#include "util.h"
#include "modules/workspace/workspace_turn.h"
#include "modules/workspace/workspace_provider.h"
#include "headers/scoped_file_io.h"
#include <aimee/tools/agent_tools.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ACP_FILE_BYTES (16u * 1024u * 1024u)
struct server_acp_resources
{
   server_session_capture_t *capture;
   server_native_tool_source_t *source;
   aimee_ir_session_sink_fn sink;
   void *sink_context;
   aimee_request_t catalog, mcp_catalog;
   cJSON *mcp_requests;
   char session[128], actor[256], cwd[MAX_PATH_LEN], resource[65], request_id[64];
   char container_id[65], image_id[72];
   const workspace_provider_t *provider;
   server_acp_bash_open_fn bash_open;
   server_acp_bash_close_fn bash_close;
   void *bash_context;
   int root;
   uint32_t capabilities;
};
static const char *field(const cJSON *object, const char *name)
{
   const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
   return value ? value : "";
}
static int resource_live(server_acp_resources_t *owner)
{
   const request_context_t *ctx = request_context_get();
   int fd = -1;
   char digest[65];
   if (!owner || !ctx || ctx->context_refused || agent_request_cancelled() ||
       ctx->model_execution != 1 || ctx->operator_script_origin ||
       ctx->capabilities != owner->capabilities ||
       server_session_capture_current() != owner->capture ||
       strcmp(ctx->request_id, owner->request_id) ||
       strcmp(request_context_session_actor(ctx), owner->actor) || !session_id() ||
       strcmp(session_id(), owner->session) || !run_cmd_get_cwd() ||
       strcmp(run_cmd_get_cwd(), owner->cwd) || !server_session_capture_accepting(owner->capture) ||
       workspace_provider_active() != owner->provider ||
       workspace_turn_resource_identity(owner->cwd, &fd, digest))
      return 0;
   struct stat held, current;
   char id[65], image[72];
   int valid = !strcmp(digest, owner->resource) &&
               (owner->provider->kind == WS_PROVIDER_CONTAINER
                    ? fd < 0 && !workspace_turn_container_identity(id, image) &&
                          !strcmp(id, owner->container_id) && !strcmp(image, owner->image_id)
                    : fd >= 0 && !fstat(fd, &current) && !fstat(owner->root, &held) &&
                          held.st_dev == current.st_dev && held.st_ino == current.st_ino);
   if (fd >= 0)
      close(fd);
   return valid;
}
void server_acp_resources_close(server_acp_resources_t *owner)
{
   if (!owner)
      return;
   server_native_tool_source_close(owner->source);
   aimee_request_free(&owner->catalog);
   aimee_request_free(&owner->mcp_catalog);
   cJSON_Delete(owner->mcp_requests);
   if (owner->root >= 0)
      close(owner->root);
   free(owner);
}
server_acp_resources_t *server_acp_resources_open(server_session_capture_t *capture,
                                                  const char *source_session, const char *actor,
                                                  const char *cwd, int tools_enabled)
{
   const request_context_t *ctx = request_context_get();
   if (!tools_enabled || !capture || !source_session || !actor || !cwd || !ctx ||
       strlen(source_session) >= 128 || strlen(actor) >= 256 || strlen(cwd) >= MAX_PATH_LEN ||
       !agent_tools_scope_isolated() || ctx->model_execution != 1 || ctx->operator_script_origin ||
       !ctx->request_id[0])
      return NULL;
   server_acp_resources_t *owner = calloc(1, sizeof *owner);
   if (!owner)
      return NULL;
   owner->root = -1;
   owner->capture = capture;
   owner->provider = workspace_provider_active();
   owner->capabilities = ctx->capabilities;
   snprintf(owner->request_id, sizeof owner->request_id, "%s", ctx->request_id);
   snprintf(owner->session, sizeof owner->session, "%s", source_session);
   snprintf(owner->actor, sizeof owner->actor, "%s", actor);
   snprintf(owner->cwd, sizeof owner->cwd, "%s", cwd);
   owner->sink = aimee_ir_session_get_sink(&owner->sink_context);
   if (!owner->sink ||
       (owner->provider->kind != WS_PROVIDER_SHARED &&
        owner->provider->kind != WS_PROVIDER_CONTAINER) ||
       workspace_turn_resource_identity(cwd, &owner->root, owner->resource) ||
       (owner->provider->kind == WS_PROVIDER_CONTAINER
            ? owner->root >= 0 ||
                  workspace_turn_container_identity(owner->container_id, owner->image_id)
            : owner->root < 0) ||
       !(owner->source = server_native_tool_source_open(capture, source_session, actor)))
      goto refused;
   if (agent_tools_catalog(AIMEE_WIRE_OPENAI_CHAT, &owner->mcp_catalog) ||
       agent_tools_catalog_filter(&owner->mcp_catalog, agent_tools_dispatch_role()) ||
       !(owner->mcp_requests = cJSON_CreateObject()))
      goto refused;
   for (int i = 0; i < owner->mcp_catalog.n_tools; i++)
      if (owner->mcp_catalog.tools[i].kind != AIMEE_TOOL_FUNCTION)
         goto refused; /* MCP tools/list is a flat function boundary. */
   aimee_tool_t children[2] = {0};
   children[0] = (aimee_tool_t){
       .kind = AIMEE_TOOL_FUNCTION,
       .name = "read_text_file",
       .schema = cJSON_Parse(
           "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"sessionId\":{"
           "\"type\":\"string\"},\"line\":{\"type\":\"integer\",\"minimum\":1},\"limit\":{\"type\":"
           "\"integer\",\"minimum\":1}},\"required\":[\"path\"],\"additionalProperties\":false}")};
   children[1] =
       (aimee_tool_t){.kind = AIMEE_TOOL_FUNCTION,
                      .name = "write_text_file",
                      .schema = cJSON_Parse(
                          "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
                          "\"sessionId\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},"
                          "\"required\":[\"path\",\"content\"],\"additionalProperties\":false}")};
   aimee_tool_t available[2];
   int count = 0;
   if (agent_tools_permission_check("read_file", NULL) == AGENT_TOOL_PERMISSION_ALLOWED)
      available[count++] = children[0];
   if (agent_tools_permission_check("write_file", NULL) == AGENT_TOOL_PERMISSION_ALLOWED)
      available[count++] = children[1];
   aimee_tool_t space = {
       .kind = AIMEE_TOOL_NAMESPACE, .name = "acp", .children = available, .n_children = count};
   aimee_request_t view = {.tools = count ? &space : NULL, .n_tools = count ? 1 : 0};
   int invalid =
       !children[0].schema || !children[1].schema || aimee_ir_request_clone(&view, &owner->catalog);
   cJSON_Delete(children[0].schema);
   cJSON_Delete(children[1].schema);
   if (invalid || !resource_live(owner))
      goto refused;
   return owner;
refused:
   server_acp_resources_close(owner);
   return NULL;
}
const aimee_request_t *server_acp_resources_catalog(const server_acp_resources_t *owner)
{
   return owner ? &owner->catalog : NULL;
}
static int resource_record_committed(server_acp_resources_t *owner, const aimee_request_t *catalog,
                                     const aimee_block_t *call, const char *message_id,
                                     const char *before, const char *after, int index)
{
   char *wire =
       db1_session_ir_page(server_session_capture_session(owner->capture), before, 1, owner->actor);
   cJSON *page = wire ? aimee_ir_json_parse_frame(wire, strlen(wire)) : NULL;
   free(wire);
   const cJSON *row = cJSON_GetArrayItem(page, 0);
   const cJSON *payload = cJSON_GetObjectItemCaseSensitive(row, "event");
   const char *turn = server_session_capture_turn(owner->capture);
   size_t size = turn ? strlen(turn) + strlen(message_id) + 2 : 0;
   char *qualified = size ? malloc(size) : NULL;
   if (qualified)
      snprintf(qualified, size, "%s:%s", turn, message_id);
   const char *committed_message = field(payload, "message_id");
   char digest[65];
   int refused =
       !qualified || !cJSON_IsArray(page) || cJSON_GetArraySize(page) != 1 ||
       strcmp(field(row, "sequence"), after) ||
       strcmp(field(row, "session_id"), server_session_capture_session(owner->capture)) ||
       strcmp(field(row, "actor"), owner->actor) || strcmp(field(row, "turn_id"), turn) ||
       strcmp(committed_message, qualified) || server_native_tool_source_digest("", row, digest) ||
       ((!call->tool_namespace && !strcmp(call->tool_name, "bash"))
            ? server_native_tool_source_record_with_catalog(
                  owner->source, catalog, call, committed_message, after, after, digest, index)
            : server_native_tool_source_record(owner->source, catalog, call, committed_message,
                                               after, after, digest, index));
   free(qualified);
   cJSON_Delete(page);
   return refused ? -1 : 0;
}
int server_acp_resources_sink(const aimee_session_event_t *event, void *context)
{
   server_acp_resources_t *owner = context;
   if (!owner || !event)
      return -1;
   int client_call = 0;
   if (event->kind == AIMEE_SESSION_MESSAGE && event->message && event->message->role &&
       !strcmp(event->message->role, "assistant"))
      for (int i = 0; i < event->message->n_blocks; i++)
      {
         const aimee_block_t *block = &event->message->blocks[i];
         if (block->type == AIMEE_BLK_TOOL_USE && block->tool_namespace &&
             !strcmp(block->tool_namespace, "acp"))
            client_call = 1;
      }
   /* Cancellation and sticky capture failure must still reach the existing
    * failed/cancelled terminal owner. Resource authority gates effects only. */
   if (!client_call)
      return owner->sink(event, owner->sink_context);
   if (!resource_live(owner))
      return -1;
   char before[32], after[32];
   if (server_session_capture_cursor(owner->capture, before) ||
       owner->sink(event, owner->sink_context))
      return -1;
   if (event->kind != AIMEE_SESSION_MESSAGE || !event->message || !event->message->role ||
       strcmp(event->message->role, "assistant"))
      return 0;
   for (int i = 0; i < event->message->n_blocks; i++)
   {
      const aimee_block_t *call = &event->message->blocks[i];
      if (call->type != AIMEE_BLK_TOOL_USE || !call->tool_namespace ||
          strcmp(call->tool_namespace, "acp"))
         continue; /* Opaque agent tool observations are not host client calls. */
      if (event->message->n_blocks != 1 || !call->tool_id ||
          strncmp(call->tool_id, "acp-rpc:", 8) || !event->message_id ||
          strcmp(event->message_id, call->tool_id) ||
          !aimee_ir_find_function(&owner->catalog, "acp", call->tool_name) ||
          server_session_capture_cursor(owner->capture, after))
         goto failed;
      if (resource_record_committed(owner, &owner->catalog, call, event->message_id, before, after,
                                    i))
         goto failed;
   }
   return 0;
failed:
   server_session_capture_fail(owner->capture);
   return -1;
}

typedef struct
{
   server_acp_resources_t *owner;
   char *text;
   int completed;
   int timeout_ms;
} resource_io_t;
static int resource_io_live(void *context)
{
   return resource_live(context);
}
static char *resource_io(const char *name, const cJSON *args, void *context, int *is_error)
{
   resource_io_t *io = context;
   server_acp_resources_t *owner = io->owner;
   int writing = !strcmp(name, "write_file");
   const char *path = field(args, "path"), *content = field(args, "content");
   const char *error = agent_tools_validate_file_resource(path, writing);
   scoped_file_io_result_t raw = {0};
   *is_error = 1;
   if (error || !resource_live(owner) ||
       (writing && (strlen(content) > ACP_FILE_BYTES || !text_is_valid_utf8(content))))
      goto done;
   int failed;
   if (owner->provider->kind == WS_PROVIDER_CONTAINER)
   {
      int unknown = 0;
      failed = workspace_turn_container_file(path, writing, content, writing ? strlen(content) : 0,
                                             io->timeout_ms, &raw.bytes, &raw.length, &unknown);
      raw.effect_started = writing && (!failed || unknown);
   }
   else
      failed = scoped_file_io(owner->root, owner->cwd, path, writing, content, strlen(content),
                              resource_io_live, owner, &raw);
   if (failed)
      goto done;
   if (!writing)
   {
      if (strlen(raw.bytes) != raw.length || !text_is_valid_utf8(raw.bytes))
         goto done;
      size_t begin = 0, end = raw.length;
      const cJSON *offset = cJSON_GetObjectItemCaseSensitive(args, "offset");
      const cJSON *limit = cJSON_GetObjectItemCaseSensitive(args, "limit");
      int skip = cJSON_IsNumber(offset) ? offset->valueint : 0;
      int take = cJSON_IsNumber(limit) ? limit->valueint : 0;
      while (begin < raw.length && skip > 0)
         if (raw.bytes[begin++] == '\n')
            skip--;
      if (take > 0)
      {
         end = begin;
         while (end < raw.length && take > 0)
            if (raw.bytes[end++] == '\n')
               take--;
      }
      io->text = strndup(raw.bytes + begin, end - begin);
      if (!io->text)
         goto done;
   }
   if (!resource_live(owner))
      goto done;
   *is_error = 0;
   io->completed = 1;
done:
   if (*is_error && raw.effect_started)
      *is_error = SCOPED_WRITE_OUTCOME_UNKNOWN;
   free(raw.bytes);
   return strdup(
       *is_error ? (raw.effect_started
                        ? "error: ACP file write outcome unknown; inspect target before retrying"
                    : error ? error
                            : "error: ACP file resource refused or unavailable")
                 : "ok");
}

static int resource_execute_claimed(server_acp_resources_t *owner, const aimee_block_t *proposed,
                                    aimee_block_t *out, int mcp, int timeout_ms)
{
   server_native_tool_claim_t claim = {0};
   aimee_tool_t definition = {0};
   resource_io_t io = {.owner = owner, .timeout_ms = timeout_ms};
   cJSON *args = NULL;
   char *wire = NULL, *receipt = NULL;
   int failed = 1, rc = -1, bash_bound = 0;
   void *bash_lease = NULL;
   agent_tools_bash_scope_t previous_bash = {0}, bash_scope = {0};
   if (!out || !resource_live(owner) ||
       server_native_tool_source_claim(owner->source, proposed, &claim))
      goto done;
   const aimee_block_t *call = &claim.original.messages[0].blocks[0];
   const aimee_tool_t *advertised =
       aimee_ir_find_function(&claim.original, mcp ? NULL : "acp", call->tool_name);
   char detail[512] = "";
   wire = cJSON_PrintUnformatted(call->tool_input);
   if (!advertised || !wire || tool_validate_ir(advertised, wire, detail, sizeof detail))
      goto done;
   free(wire);
   wire = NULL;
   const char *name = mcp                                          ? call->tool_name
                      : !strcmp(call->tool_name, "read_text_file") ? "read_file"
                                                                   : "write_file";
   args = mcp ? cJSON_Duplicate(call->tool_input, 1) : cJSON_CreateObject();
   if (!args || (!mcp && !cJSON_AddStringToObject(args, "path", field(call->tool_input, "path"))))
      goto done;
   if (!mcp && !strcmp(name, "read_file"))
   {
      const cJSON *line = cJSON_GetObjectItemCaseSensitive(call->tool_input, "line");
      const cJSON *limit = cJSON_GetObjectItemCaseSensitive(call->tool_input, "limit");
      if ((line && (!cJSON_IsNumber(line) || line->valuedouble < 1 || line->valuedouble > INT_MAX ||
                    line->valuedouble != line->valueint)) ||
          (limit && (!cJSON_IsNumber(limit) || limit->valuedouble < 1 ||
                     limit->valuedouble > INT_MAX || limit->valuedouble != limit->valueint)) ||
          (line && !cJSON_AddNumberToObject(args, "offset", line->valueint - 1)) ||
          (limit && !cJSON_AddNumberToObject(args, "limit", limit->valueint)) ||
          !cJSON_AddBoolToObject(args, "raw", 1))
         goto done;
   }
   else if (!mcp && !cJSON_AddStringToObject(args, "content", field(call->tool_input, "content")))
      goto done;
   if (agent_tools_execution_definition(name, &definition) ||
       !(wire = cJSON_PrintUnformatted(args)) ||
       tool_validate_ir(&definition, wire, detail, sizeof detail))
      goto done;
   char attempt[96];
   snprintf(attempt, sizeof attempt, "acp-client:%s:%d", claim.sequence, claim.block_index);
   const char *refusal =
       server_native_tool_admit_attempt(name, args, owner->session, attempt, detail, sizeof detail);
   if (!resource_live(owner))
      goto done;
   if (refusal)
      receipt = strdup(detail[0] ? detail : "ACP tool policy refused execution");
   else
   {
      if (mcp && !strcmp(name, "bash"))
      {
         if (!owner->bash_open || !owner->bash_close ||
             owner->bash_open(claim.digest, &claim.catalog, &bash_scope, &bash_lease,
                              owner->bash_context) ||
             !bash_lease || !bash_scope.environment || !bash_scope.replace_environment ||
             !bash_scope.progress || !resource_live(owner))
            goto done;
         previous_bash = agent_tools_bind_bash_scope(&bash_scope);
         bash_bound = 1;
      }
      agent_tools_set_effect_authorized(1);
      receipt = mcp ? dispatch_tool_call_ir_ctx_status(&definition, wire, timeout_ms, &failed)
                    : dispatch_tool_file_resource_ctx_status(&definition, wire, timeout_ms,
                                                             resource_io, &io, &failed);
      agent_tools_set_effect_authorized(0);
      if (bash_bound)
      {
         agent_tools_bind_bash_scope(&previous_bash);
         bash_bound = 0;
      }
      if (bash_lease)
      {
         owner->bash_close(bash_lease, owner->bash_context);
         bash_lease = NULL;
      }
   }
   if (!receipt)
      goto done;
   if (!mcp && !failed && !io.completed)
   {
      free(receipt);
      receipt = strdup("ACP file resource has no current host I/O receipt");
      failed = 1;
      if (!receipt)
         goto done;
   }
   aimee_block_t result = {
       .type = AIMEE_BLK_TOOL_RESULT,
       .tool_id = strdup(call->tool_id),
       .tool_is_error = failed,
       .tool_result = cJSON_CreateString(mcp || failed ? receipt : (io.text ? io.text : ""))};
   if (!result.tool_id || !result.tool_result || !aimee_ir_block_valid(&result))
   {
      aimee_block_free_contents(&result);
      goto done;
   }
   aimee_block_free_contents(out);
   *out = result;
   rc = 0;
done:
   agent_tools_set_effect_authorized(0);
   if (bash_bound)
      agent_tools_bind_bash_scope(&previous_bash);
   if (bash_lease)
      owner->bash_close(bash_lease, owner->bash_context);
   if (rc && owner)
      server_session_capture_fail(owner->capture);
   free(io.text);
   free(wire);
   free(receipt);
   cJSON_Delete(args);
   aimee_tool_free(&definition);
   server_native_tool_claim_free(&claim);
   return rc;
}

int server_acp_resources_execute(const aimee_block_t *call, aimee_block_t *result, void *owner)
{
   return resource_execute_claimed(owner, call, result, 0, 120000);
}
const aimee_request_t *server_acp_resources_mcp_catalog(const server_acp_resources_t *owner)
{
   return owner ? &owner->mcp_catalog : NULL;
}
int server_acp_resources_mcp_execute(server_acp_resources_t *owner, const char *request_key,
                                     const char *tool, const cJSON *arguments, int timeout_ms,
                                     aimee_block_t *out)
{
   if (!out || !resource_live(owner) || !request_key || !request_key[0] ||
       strlen(request_key) > 127 || !tool || !tool[0] || !cJSON_IsObject(arguments) ||
       !aimee_ir_json_unique(arguments) || timeout_ms <= 0 || timeout_ms > 120000 ||
       !aimee_ir_find_function(&owner->mcp_catalog, NULL, tool) ||
       cJSON_GetArraySize(owner->mcp_requests) >= 4096 ||
       cJSON_GetObjectItemCaseSensitive(owner->mcp_requests, request_key))
      return -1;
   for (const unsigned char *p = (const unsigned char *)request_key; *p; p++)
      if (*p < 0x21 || *p > 0x7e)
         return -1;
   const cJSON *cwd = cJSON_GetObjectItemCaseSensitive(arguments, "cwd");
   if (cwd && (!cJSON_IsString(cwd) || strcmp(cwd->valuestring, owner->cwd)))
      return -1;
   if (!cJSON_AddTrueToObject(owner->mcp_requests, request_key))
      return -1;
   const aimee_tool_t *advertised = aimee_ir_find_function(&owner->mcp_catalog, NULL, tool);
   cJSON *canonical = tool_args_canonicalize(advertised->schema, arguments);
   const cJSON *canonical_cwd = cJSON_GetObjectItemCaseSensitive(canonical, "cwd");
   if (!cJSON_IsObject(canonical) ||
       (canonical_cwd &&
        (!cJSON_IsString(canonical_cwd) || strcmp(canonical_cwd->valuestring, owner->cwd))))
   {
      cJSON_Delete(canonical);
      return -1;
   }
   char id[160];
   snprintf(id, sizeof id, "acp-mcp:%s", request_key);
   /* The source is this actual received client request, not an observed
    * provider generation or an opaque agent tool status update. */
   aimee_block_t call = {.type = AIMEE_BLK_TOOL_USE,
                         .tool_id = id,
                         .tool_name = (char *)tool,
                         .tool_input = canonical};
   aimee_message_t message = {.role = "assistant", .blocks = &call, .n_blocks = 1};
   aimee_session_event_t event = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = id, .message = &message};
   char before[32], after[32];
   void *sink_context = NULL;
   if (!aimee_ir_block_valid(&call) ||
       aimee_ir_session_get_sink(&sink_context) != server_acp_resources_sink ||
       sink_context != owner || server_session_capture_cursor(owner->capture, before) ||
       aimee_ir_session_emit(&event) || server_session_capture_cursor(owner->capture, after) ||
       resource_record_committed(owner, &owner->mcp_catalog, &call, id, before, after, 0) ||
       resource_execute_claimed(owner, &call, out, 1, timeout_ms))
      goto failed;
   char result_id[176];
   snprintf(result_id, sizeof result_id, "%s:result", id);
   aimee_message_t result = {.role = "tool", .blocks = out, .n_blocks = 1};
   event = (aimee_session_event_t){
       .kind = AIMEE_SESSION_MESSAGE, .message_id = result_id, .message = &result};
   if (aimee_ir_session_emit(&event))
      goto failed;
   cJSON_Delete(canonical);
   return 0;
failed:
   cJSON_Delete(canonical);
   server_session_capture_fail(owner->capture);
   return -1;
}

int server_acp_resources_set_bash_owner(server_acp_resources_t *owner, server_acp_bash_open_fn open,
                                        server_acp_bash_close_fn close, void *context)
{
   if (!resource_live(owner) || !open || !close || owner->bash_open)
      return -1;
   owner->bash_open = open;
   owner->bash_close = close;
   owner->bash_context = context;
   return 0;
}
