/* Real resource/source owners and real file effects; transport, durable store
 * and policy/dispatch peers expose ordering and refusal at their boundaries.
 * Stored IDs follow the capture owner's turn qualification; the public-route
 * composition tests additionally use actual capture_append and action owners. */
#include "../server/server_acp_resources.c"
#include "db1_client/server_sessions.h"
#include "server_session_control.h"
#include <assert.h>

struct server_session_capture
{
   int live, failed;
   unsigned long count;
   unsigned char claims[128];
};
static struct server_session_capture capture = {.live = 1};
static int other_capture, cancel_on_truncate;
static request_context_t ctx;
static cJSON *rows;
static char root_path[MAX_PATH_LEN], current_session[128] = "source";
static int root_fd = -1, cancelled, stale, foreign, permission_denied, directive_denied,
           policy_denied;
static int reply_cap = 1, dispatch_calls, policy_calls, sink_calls;
static workspace_provider_t workspace = {.kind = WS_PROVIDER_SHARED};

const request_context_t *request_context_get(void)
{
   return &ctx;
}
const char *session_id(void)
{
   return current_session;
}
int agent_request_cancelled(void)
{
   return cancelled;
}
int agent_tools_scope_isolated(void)
{
   return 1;
}
const workspace_provider_t *workspace_provider_active(void)
{
   return &workspace;
}
/* This lower fixture exercises the shared FD owner. Container ownership is
 * exercised by the real backend/full-worker fixture, never affirmed here. */
int workspace_turn_container_identity(char container_id[65], char image_id[72])
{
   (void)container_id;
   (void)image_id;
   abort();
}
int workspace_turn_container_file(const char *path, int writing, const char *content, size_t length,
                                  int timeout_ms, char **out, size_t *out_length, int *unknown)
{
   (void)path;
   (void)writing;
   (void)content;
   (void)length;
   (void)timeout_ms;
   (void)out;
   (void)out_length;
   (void)unknown;
   abort();
}
int workspace_turn_resource_identity(const char *cwd, int *fd, char digest[65])
{
   if (stale || strcmp(cwd, root_path))
      return -1;
   struct stat partial;
   if (cancel_on_truncate && !fstatat(root_fd, "partial", &partial, AT_SYMLINK_NOFOLLOW) &&
       !partial.st_size)
   {
      cancelled = 1;
      return -1;
   }

   *fd = dup(root_fd);
   memset(digest, 'a', 64);
   digest[64] = 0;
   return *fd < 0 ? -1 : 0;
}
server_session_capture_t *server_session_capture_current(void)
{
   return other_capture ? NULL : &capture;
}
const char *server_session_capture_turn(const server_session_capture_t *c)
{
   return c ? "turn" : NULL;
}
const char *server_session_capture_session(const server_session_capture_t *c)
{
   return c ? "destination" : NULL;
}
int server_session_capture_accepting(const server_session_capture_t *c)
{
   return c && c->live && !c->failed;
}
void server_session_capture_fail(server_session_capture_t *c)
{
   if (c)
      c->failed = 1;
}
int server_session_capture_cursor(server_session_capture_t *c, char sequence[32])
{
   snprintf(sequence, 32, "%lu", c->count);
   return 0;
}
int server_session_capture_claim_call(server_session_capture_t *c, const char *sequence, int index)
{
   unsigned n = (unsigned)strtoul(sequence, NULL, 10);
   if (index || n >= 128 || c->claims[n])
      return -1;
   c->claims[n] = 1;
   return 0;
}
int db1_server_session_get(const char *id, db1_server_session_t *out)
{
   if (strcmp(id, "source") && strcmp(id, "destination"))
      return -1;
   memset(out, 0, sizeof *out);
   snprintf(out->principal, sizeof out->principal, "%s", foreign ? "other" : "actor");
   snprintf(out->created_at, sizeof out->created_at, "incarnation");
   return 0;
}
char *db1_session_ir_page(const char *session, const char *after, int limit, const char *actor)
{
   if (strcmp(session, "destination") || strcmp(actor, "actor"))
      return NULL;
   unsigned start = (unsigned)strtoul(after, NULL, 10);
   cJSON *page = cJSON_CreateArray();
   for (int i = (int)start; i < cJSON_GetArraySize(rows) && limit-- > 0; i++)
      assert(cJSON_AddItemToArray(page, cJSON_Duplicate(cJSON_GetArrayItem(rows, i), 1)));
   char *wire = cJSON_PrintUnformatted(page);
   cJSON_Delete(page);
   return wire;
}
static int original_sink(const aimee_session_event_t *event, void *unused)
{
   (void)unused;
   sink_calls++;
   cJSON *row = cJSON_CreateObject();
   char sequence[32];
   snprintf(sequence, sizeof sequence, "%lu", ++capture.count);
   assert(cJSON_AddStringToObject(row, "sequence", sequence));
   assert(cJSON_AddStringToObject(row, "session_id", "destination"));
   assert(cJSON_AddStringToObject(row, "turn_id", "turn"));
   assert(cJSON_AddStringToObject(row, "actor", "actor"));
   cJSON *payload = aimee_ir_session_event_json(event);
   if (event->message_id)
   {
      size_t size = strlen(event->message_id) + sizeof "turn:";
      char *qualified = malloc(size);
      assert(qualified);
      snprintf(qualified, size, "turn:%s", event->message_id);
      assert(
          cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(payload, "message_id"), qualified));
      free(qualified);
   }
   assert(cJSON_AddItemToObject(row, "event", payload));
   assert(cJSON_AddItemToArray(rows, row));
   return 0;
}
server_control_scope_t *server_control_scope_current(void)
{
   return (server_control_scope_t *)1;
}
int server_control_scope_can_reply(const server_control_scope_t *s)
{
   return s && reply_cap;
}
const char *agent_tools_validate_file_resource(const char *path, int writing)
{
   (void)writing;
   return strstr(path, "secret") ? "error: sensitive path" : NULL;
}
agent_tool_permission_t agent_tools_permission_check(const char *tool, const cJSON *args)
{
   assert(!strcmp(tool, "read_file") || !strcmp(tool, "write_file") || !strcmp(tool, "bash"));
   assert(!args || cJSON_IsObject(args));
   return permission_denied ? AGENT_TOOL_PERMISSION_ROLE : AGENT_TOOL_PERMISSION_ALLOWED;
}
char *agent_tools_permission_error(agent_tool_permission_t permission)
{
   (void)permission;
   return strdup("role refused");
}
int directive_check_tool(const char *tool, const char *wire, char *error, size_t capacity)
{
   (void)tool;
   (void)wire;
   snprintf(error, capacity, "directive refused");
   return directive_denied ? -1 : 0;
}
int tool_policy_metadata(const char *tool, aimee_tool_metadata_t *metadata, char *effect,
                         size_t cap, char *error, size_t error_cap)
{
   (void)error;
   (void)error_cap;
   assert(!strcmp(tool, "read_file") || !strcmp(tool, "write_file") || !strcmp(tool, "bash"));
   memset(metadata, 0, sizeof *metadata);
   snprintf(effect, cap, "filesystem");
   return 0;
}
int policy_check_tool_metadata_attempt(const char *tool, const aimee_tool_metadata_t *metadata,
                                       const char *effect, const char *wire, const char *attempt,
                                       char *error, size_t capacity)
{
   (void)metadata;
   (void)effect;
   (void)wire;
   assert(!strcmp(tool, "read_file") || !strcmp(tool, "write_file") || !strcmp(tool, "bash"));
   assert(!strncmp(attempt, "acp-client:", 11));
   policy_calls++;
   snprintf(error, capacity, "policy refused");
   return policy_denied ? -1 : 0;
}
int agent_tools_execution_definition(const char *name, aimee_tool_t *out)
{
   out->kind = AIMEE_TOOL_FUNCTION;
   out->name = strdup(name);
   if (!strcmp(name, "bash"))
   {
      out->schema =
          cJSON_Parse("{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},"
                      "\"required\":[\"command\"],\"additionalProperties\":false}");
      return 0;
   }
   out->schema = cJSON_Parse(
       "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":"
       "\"string\"},\"offset\":{\"type\":\"integer\"},\"limit\":{\"type\":\"integer\"},\"raw\":{"
       "\"type\":\"boolean\"}},\"required\":[\"path\"],\"additionalProperties\":false}");
   return 0;
}
const char *agent_tools_dispatch_role(void)
{
   return "fixture";
}
int agent_tools_catalog(aimee_wire_t wire, aimee_request_t *out)
{
   assert(wire == AIMEE_WIRE_OPENAI_CHAT);
   out->tools = calloc(3, sizeof *out->tools);
   assert(out->tools);
   out->n_tools = 3;
   assert(!agent_tools_execution_definition("read_file", out->tools));
   assert(!agent_tools_execution_definition("write_file", out->tools + 1));
   assert(!agent_tools_execution_definition("bash", out->tools + 2));
   return 0;
}
int agent_tools_catalog_filter(aimee_request_t *catalog, const char *role)
{
   assert(!strcmp(role, "fixture"));
   if (permission_denied)
      aimee_request_free(catalog);
   return 0;
}
static agent_tools_bash_scope_t active_bash_scope;
static int bash_opens, bash_closes, bash_open_fail;
static char *bash_environment[] = {"PATH=/usr/bin:/bin", NULL};
agent_tools_bash_scope_t agent_tools_bind_bash_scope(const agent_tools_bash_scope_t *scope)
{
   agent_tools_bash_scope_t previous = active_bash_scope;
   active_bash_scope = scope ? *scope : (agent_tools_bash_scope_t){0};
   return previous;
}
static int bash_progress(void *context)
{
   assert(context == &bash_opens);
   return 0;
}
static int bash_open(const char *digest, const aimee_request_t *catalog,
                     agent_tools_bash_scope_t *scope, void **lease, void *context)
{
   assert(context == &bash_opens && strlen(digest) == 64 && catalog->n_tools == 3);
   assert(aimee_ir_find_function(catalog, NULL, "bash") &&
          aimee_ir_find_function(catalog, NULL, "write_file"));
   bash_opens++;
   *lease = &bash_opens;
   *scope = (agent_tools_bash_scope_t){.environment = bash_environment,
                                       .replace_environment = 1,
                                       .progress = bash_progress,
                                       .context = &bash_opens};
   return bash_open_fail ? -1 : 0;
}
static void bash_close(void *lease, void *context)
{
   assert(lease == &bash_opens && context == &bash_opens);
   assert(!active_bash_scope.environment && !active_bash_scope.progress);
   bash_closes++;
}
static int mcp_dispatches;
char *dispatch_tool_call_ir_ctx_status(const aimee_tool_t *definition, const char *wire,
                                       int timeout, int *failed)
{
   assert(!strcmp(definition->name, "read_file") || !strcmp(definition->name, "write_file") ||
          !strcmp(definition->name, "bash"));
   assert(timeout > 0 && timeout <= 120000);
   cJSON *args = cJSON_Parse(wire);
   assert(cJSON_IsObject(args));
   if (!strcmp(definition->name, "bash"))
   {
      assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(args, "command")));
      assert(active_bash_scope.environment == bash_environment &&
             active_bash_scope.replace_environment && active_bash_scope.progress);
      assert(!active_bash_scope.progress(active_bash_scope.context));
   }
   else
      assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(args, "path")));
   assert(!cJSON_GetObjectItemCaseSensitive(args, "file_path"));
   const cJSON *cwd = cJSON_GetObjectItemCaseSensitive(args, "cwd");
   assert(!cwd || (cJSON_IsString(cwd) && !strcmp(cwd->valuestring, root_path)));
   cJSON_Delete(args);
   mcp_dispatches++;
   *failed = 0;
   return strdup("actual MCP tool peer result");
}
void agent_tools_set_effect_authorized(int value)
{
   (void)value;
}
char *dispatch_tool_file_resource_ctx_status(const aimee_tool_t *definition, const char *wire,
                                             int timeout, agent_tool_file_resource_fn execute,
                                             void *context, int *failed)
{
   (void)timeout;
   dispatch_calls++;
   cJSON *args = cJSON_Parse(wire);
   assert(args);
   const char *path = field(args, "path");
   char absolute[MAX_PATH_LEN];
   if (path[0] != '/')
   {
      assert(snprintf(absolute, sizeof absolute, "%s/%s", root_path, path) < (int)sizeof absolute);
      assert(cJSON_SetValuestring(cJSON_GetObjectItem(args, "path"), absolute));
   }
   char *result = execute(definition->name, args, context, failed);
   cJSON_Delete(args);
   return result;
}
/* The real validator is linked; the optional operator registry is absent. */
int kb_client_tool_registry_lookup(const char *name, char *description, size_t description_len,
                                   char *schema, size_t schema_len, int *enabled, int *found)
{
   (void)name;
   (void)description;
   (void)description_len;
   (void)schema;
   (void)schema_len;
   *enabled = 1;
   *found = 0;
   return -1;
}

static aimee_block_t call(unsigned id, const char *name, const char *json)
{
   char key[64];
   snprintf(key, sizeof key, "acp-rpc:%u", id);
   return (aimee_block_t){.type = AIMEE_BLK_TOOL_USE,
                          .tool_namespace = strdup("acp"),
                          .tool_name = strdup(name),
                          .tool_id = strdup(key),
                          .tool_input = cJSON_Parse(json)};
}
static void record(server_acp_resources_t *owner, const aimee_block_t *call)
{
   aimee_message_t message = {.role = "assistant", .blocks = (aimee_block_t *)call, .n_blocks = 1};
   aimee_session_event_t event = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = call->tool_id, .message = &message};
   assert(!server_acp_resources_sink(&event, owner));
}
static void write_fixture(const char *name, const char *text)
{
   int fd = openat(root_fd, name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
   assert(fd >= 0);
   assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
   assert(!close(fd));
}
static void expect(server_acp_resources_t *owner, unsigned id, const char *name, const char *json,
                   int failed, const char *text)
{
   aimee_block_t input = call(id, name, json), result = {0};
   record(owner, &input);
   assert(!server_acp_resources_execute(&input, &result, owner));
   assert(result.tool_is_error == failed);
   if (text)
      assert(!strcmp(cJSON_GetStringValue(result.tool_result), text));
   aimee_block_free_contents(&input);
   aimee_block_free_contents(&result);
}
int main(void)
{
   const char *tmp = getenv("TMPDIR");
   assert(tmp && tmp[0]);
   assert(snprintf(root_path, sizeof root_path, "%s/acp-resource-XXXXXX", tmp) <
          (int)sizeof root_path);
   assert(mkdtemp(root_path));
   root_fd = open(root_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   assert(root_fd >= 0);
   run_cmd_set_cwd(root_path);
   snprintf(ctx.principal, sizeof ctx.principal, "actor");
   snprintf(ctx.request_id, sizeof ctx.request_id, "request");
   ctx.model_execution = 1;
   rows = cJSON_CreateArray();
   aimee_ir_session_set_sink(original_sink, NULL);
   server_acp_resources_t *owner =
       server_acp_resources_open(&capture, "source", "actor", root_path, 1);
   assert(owner);
   reply_cap = 0;
   server_acp_resources_t *headless =
       server_acp_resources_open(&capture, "source", "actor", root_path, 1);
   assert(headless &&
          aimee_ir_find_function(server_acp_resources_catalog(headless), "acp", "read_text_file"));
   assert(aimee_ir_find_function(server_acp_resources_catalog(headless), "acp", "write_text_file"));
   server_acp_resources_close(headless);
   reply_cap = 1;
   permission_denied = 1;
   server_acp_resources_t *text_only =
       server_acp_resources_open(&capture, "source", "actor", root_path, 1);
   assert(text_only && !server_acp_resources_catalog(text_only)->n_tools);
   aimee_block_t text_block = {.type = AIMEE_BLK_TEXT, .text = "Text-only role remains available"};
   aimee_message_t text_message = {.role = "assistant", .blocks = &text_block, .n_blocks = 1};
   aimee_session_event_t text_event = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = "text-only-role", .message = &text_message};
   int prior_sinks = sink_calls, prior_dispatch = dispatch_calls;
   assert(!server_acp_resources_sink(&text_event, text_only) && sink_calls == prior_sinks + 1);
   aimee_block_t forbidden_fs =
       call(25, "write_text_file", "{\"path\":\"no-fs-role\",\"content\":\"no\"}");
   aimee_message_t forbidden_message = {
       .role = "assistant", .blocks = &forbidden_fs, .n_blocks = 1};
   aimee_session_event_t forbidden_event = {.kind = AIMEE_SESSION_MESSAGE,
                                            .message_id = forbidden_fs.tool_id,
                                            .message = &forbidden_message};
   assert(server_acp_resources_sink(&forbidden_event, text_only));
   assert(dispatch_calls == prior_dispatch && faccessat(root_fd, "no-fs-role", F_OK, 0));
   capture.failed = 0;
   aimee_block_free_contents(&forbidden_fs);
   server_acp_resources_close(text_only);
   permission_denied = 0;
   write_fixture("utf8", "error: ordinary file bytes\nαβ\nlast\n");
   expect(owner, 1, "read_text_file", "{\"path\":\"utf8\"}", 0,
          "error: ordinary file bytes\nαβ\nlast\n");
   expect(owner, 2, "read_text_file", "{\"path\":\"utf8\",\"line\":2,\"limit\":1}", 0, "αβ\n");
   expect(owner, 3, "write_text_file", "{\"path\":\"out\",\"content\":\"one exact write α\"}", 0,
          "");
   char buffer[64] = {0};
   int fd = openat(root_fd, "out", O_RDONLY | O_CLOEXEC);
   assert(fd >= 0);
   assert(read(fd, buffer, sizeof buffer) > 0);
   close(fd);
   assert(!strcmp(buffer, "one exact write α"));
   aimee_block_t duplicate = call(3, "write_text_file",
                                  "{\"path\":\"out\",\"content\":\"one exact write α\"}"),
                 result = {0};
   assert(server_acp_resources_execute(&duplicate, &result, owner));
   assert(!result.tool_result);
   aimee_block_free_contents(&duplicate);
   capture.failed = 0;
   assert(!symlinkat("utf8", root_fd, "link"));
   expect(owner, 4, "read_text_file", "{\"path\":\"link\"}", 1, NULL);
   expect(owner, 5, "write_text_file", "{\"path\":\"../outside\",\"content\":\"no\"}", 1, NULL);
   expect(owner, 6, "read_text_file", "{\"path\":\"/etc/hosts\"}", 1, NULL);
   assert(!mkdirat(root_fd, "dir", 0700));
   assert(!symlinkat("dir", root_fd, "dirlink"));
   expect(owner, 7, "write_text_file", "{\"path\":\"dirlink/no\",\"content\":\"no\"}", 1, NULL);
   reply_cap = 0;
   expect(owner, 8, "write_text_file", "{\"path\":\"headless\",\"content\":\"authorized\"}", 0, "");
   assert(!faccessat(root_fd, "headless", F_OK, 0));
   policy_denied = 1;
   expect(owner, 23, "write_text_file", "{\"path\":\"headless-denied\",\"content\":\"no\"}", 1,
          NULL);
   assert(faccessat(root_fd, "headless-denied", F_OK, 0));
   policy_denied = 0;
   reply_cap = 1;
   int count = dispatch_calls;
   permission_denied = 1;
   expect(owner, 9, "read_text_file", "{\"path\":\"utf8\"}", 1, NULL);
   permission_denied = 0;
   assert(dispatch_calls == count);
   directive_denied = 1;
   expect(owner, 10, "read_text_file", "{\"path\":\"utf8\"}", 1, NULL);
   directive_denied = 0;
   assert(dispatch_calls == count);
   policy_denied = 1;
   expect(owner, 11, "read_text_file", "{\"path\":\"utf8\"}", 1, NULL);
   policy_denied = 0;
   assert(dispatch_calls == count);
   aimee_block_t pending =
       call(12, "write_text_file", "{\"path\":\"cancelled\",\"content\":\"no\"}");
   record(owner, &pending);
   cancelled = 1;
   assert(server_acp_resources_execute(&pending, &result, owner));
   cancelled = 0;
   capture.failed = 0;
   assert(faccessat(root_fd, "cancelled", F_OK, 0));
   aimee_block_free_contents(&pending);
   pending = call(13, "write_text_file", "{\"path\":\"foreign\",\"content\":\"no\"}");
   record(owner, &pending);
   foreign = 1;
   assert(server_acp_resources_execute(&pending, &result, owner));
   foreign = 0;
   capture.failed = 0;
   aimee_block_free_contents(&pending);
   pending = call(14, "write_text_file", "{\"path\":\"stale\",\"content\":\"no\"}");
   record(owner, &pending);
   stale = 1;
   assert(server_acp_resources_execute(&pending, &result, owner));
   stale = 0;
   capture.failed = 0;
   aimee_block_free_contents(&pending);
   /* Opaque agent updates carry no ACP client namespace and establish no source. */
   pending = call(15, "write_text_file", "{\"path\":\"opaque\",\"content\":\"no\"}");
   free(pending.tool_namespace);
   pending.tool_namespace = NULL;
   aimee_message_t opaque = {.role = "assistant", .blocks = &pending, .n_blocks = 1};
   aimee_session_event_t event = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = "acp-use:opaque", .message = &opaque};
   assert(!server_acp_resources_sink(&event, owner));
   assert(server_acp_resources_execute(&pending, &result, owner));
   capture.failed = 0;
   aimee_block_free_contents(&pending);
   pending = call(16, "read_text_file", "{\"path\":\"utf8\",\"line\":1.5}");
   record(owner, &pending);
   count = dispatch_calls;
   assert(server_acp_resources_execute(&pending, &result, owner));
   assert(dispatch_calls == count);
   capture.failed = 0;
   aimee_block_free_contents(&pending);
   char *long_text = malloc(10002);
   assert(long_text);
   memset(long_text, 'x', 10000);
   long_text[10000] = '\n';
   long_text[10001] = 0;
   write_fixture("long", long_text);
   expect(owner, 17, "read_text_file", "{\"path\":\"long\",\"line\":1,\"limit\":1}", 0, long_text);
   free(long_text);
   write_fixture("invalid", "\xff");
   expect(owner, 18, "read_text_file", "{\"path\":\"invalid\"}", 1, NULL);
   pending = call(19, "write_text_file", "{\"path\":\"forged\",\"content\":\"original\"}");
   record(owner, &pending);
   assert(cJSON_SetValuestring(cJSON_GetObjectItem(pending.tool_input, "content"), "changed"));
   count = dispatch_calls;
   assert(server_acp_resources_execute(&pending, &result, owner));
   assert(dispatch_calls == count && faccessat(root_fd, "forged", F_OK, 0));
   capture.failed = 0;
   aimee_block_free_contents(&pending);
   pending = call(20, "write_text_file", "{\"path\":\"corrupt\",\"content\":\"original\"}");
   record(owner, &pending);
   cJSON *row = cJSON_GetArrayItem(rows, cJSON_GetArraySize(rows) - 1);
   assert(cJSON_SetValuestring(cJSON_GetObjectItem(row, "turn_id"), "tampered"));
   assert(server_acp_resources_execute(&pending, &result, owner));
   assert(dispatch_calls == count && faccessat(root_fd, "corrupt", F_OK, 0));
   capture.failed = 0;
   aimee_block_free_contents(&pending);
   pending = call(21, "write_text_file", "{\"path\":\"wrong-capture\",\"content\":\"no\"}");
   record(owner, &pending);
   other_capture = 1;
   assert(server_acp_resources_execute(&pending, &result, owner));
   other_capture = 0;
   capture.failed = 0;
   aimee_block_free_contents(&pending);
   assert(faccessat(root_fd, "wrong-capture", F_OK, 0));
   pending = call(22, "write_text_file", "{\"path\":\"wrong-request\",\"content\":\"no\"}");
   record(owner, &pending);
   snprintf(ctx.request_id, sizeof ctx.request_id, "other-request");
   assert(server_acp_resources_execute(&pending, &result, owner));
   snprintf(ctx.request_id, sizeof ctx.request_id, "request");
   capture.failed = 0;
   aimee_block_free_contents(&pending);
   assert(faccessat(root_fd, "wrong-request", F_OK, 0));
   write_fixture("partial", "existing content");
   pending = call(24, "write_text_file", "{\"path\":\"partial\",\"content\":\"replacement\"}");
   record(owner, &pending);
   cancel_on_truncate = 1;
   assert(!server_acp_resources_execute(&pending, &result, owner));
   assert(result.tool_is_error &&
          strstr(cJSON_GetStringValue(result.tool_result), "outcome unknown"));
   struct stat partial;
   assert(!fstatat(root_fd, "partial", &partial, 0) && !partial.st_size);
   assert(cancelled);
   cancelled = 0;
   cancel_on_truncate = 0;
   aimee_block_free_contents(&pending);
   aimee_block_free_contents(&result);
   aimee_ir_session_set_sink(server_acp_resources_sink, owner);
   cJSON *mcp_args = cJSON_Parse("{\"path\":\"utf8\"}");
   assert(mcp_args);
   int previous_rows = cJSON_GetArraySize(rows), previous_mcp = mcp_dispatches;
   assert(aimee_ir_find_function(server_acp_resources_mcp_catalog(owner), NULL, "read_file"));
   assert(!server_acp_resources_mcp_execute(owner, "host-request-1", "read_file", mcp_args, 1000,
                                            &result));
   assert(!result.tool_is_error &&
          !strcmp(cJSON_GetStringValue(result.tool_result), "actual MCP tool peer result"));
   assert(!strcmp(result.tool_id, "acp-mcp:host-request-1") && mcp_dispatches == previous_mcp + 1);
   assert(cJSON_GetArraySize(rows) == previous_rows + 2);
   aimee_block_free_contents(&result);
   assert(server_acp_resources_mcp_execute(owner, "host-request-1", "read_file", mcp_args, 1000,
                                           &result));
   assert(mcp_dispatches == previous_mcp + 1 && cJSON_GetArraySize(rows) == previous_rows + 2);
   permission_denied = 1;
   assert(!server_acp_resources_mcp_execute(owner, "host-request-role", "read_file", mcp_args, 1000,
                                            &result));
   assert(result.tool_is_error && mcp_dispatches == previous_mcp + 1);
   permission_denied = 0;
   aimee_block_free_contents(&result);
   policy_denied = 1;
   reply_cap = 0;
   assert(!server_acp_resources_mcp_execute(owner, "host-headless-refused", "write_file", mcp_args,
                                            1000, &result));
   assert(result.tool_is_error && mcp_dispatches == previous_mcp + 1);
   policy_denied = 0;
   aimee_block_free_contents(&result);
   assert(!server_acp_resources_mcp_execute(owner, "host-headless-authorized", "write_file",
                                            mcp_args, 1000, &result));
   assert(!result.tool_is_error && mcp_dispatches == previous_mcp + 2);
   reply_cap = 1;
   aimee_block_free_contents(&result);
   cancelled = 1;
   assert(server_acp_resources_mcp_execute(owner, "host-cancelled", "read_file", mcp_args, 1000,
                                           &result));
   cancelled = 0;
   stale = 1;
   assert(
       server_acp_resources_mcp_execute(owner, "host-stale", "read_file", mcp_args, 1000, &result));
   stale = 0;
   snprintf(ctx.principal, sizeof ctx.principal, "other");
   assert(server_acp_resources_mcp_execute(owner, "host-foreign", "read_file", mcp_args, 1000,
                                           &result));
   snprintf(ctx.principal, sizeof ctx.principal, "actor");
   assert(cJSON_AddStringToObject(mcp_args, "cwd", "/foreign/project"));
   assert(server_acp_resources_mcp_execute(owner, "host-foreign-cwd", "read_file", mcp_args, 1000,
                                           &result));
   cJSON_DeleteItemFromObjectCaseSensitive(mcp_args, "cwd");
   assert(server_acp_resources_mcp_execute(owner, "host-unadvertised", "unadvertised", mcp_args,
                                           1000, &result));
   assert(
       server_acp_resources_mcp_execute(owner, "host-timeout", "read_file", mcp_args, 0, &result));
   ctx.operator_script_origin = 1;
   assert(server_acp_resources_mcp_execute(owner, "host-operator-upgrade", "read_file", mcp_args,
                                           1000, &result));
   ctx.operator_script_origin = 0;
   ctx.capabilities ^= 1;
   assert(server_acp_resources_mcp_execute(owner, "host-capability-upgrade", "read_file", mcp_args,
                                           1000, &result));
   ctx.capabilities ^= 1;
   ctx.model_execution = 0;
   assert(server_acp_resources_mcp_execute(owner, "host-model-origin-missing", "read_file",
                                           mcp_args, 1000, &result));
   ctx.model_execution = 1;
   cJSON *alias_args = cJSON_Parse("{\"file_path\":\"utf8\",\"offset\":\"2\"}");
   assert(alias_args);
   assert(!server_acp_resources_mcp_execute(owner, "host-normalized-alias", "read_file", alias_args,
                                            1000, &result));
   assert(!result.tool_is_error && mcp_dispatches == previous_mcp + 3);
   assert(cJSON_GetObjectItemCaseSensitive(alias_args, "file_path")); /* Boundary owns its copy. */
   aimee_block_free_contents(&result);
   cJSON_Delete(alias_args);
   assert(mcp_dispatches == previous_mcp + 3);
   cJSON *bash_args = cJSON_Parse("{\"command\":\"fixture ordinary Bash dispatcher\"}");
   assert(bash_args);
   assert(!server_acp_resources_set_bash_owner(owner, bash_open, bash_close, &bash_opens));
   assert(server_acp_resources_set_bash_owner(owner, bash_open, bash_close, &bash_opens));
   policy_denied = 1;
   assert(!server_acp_resources_mcp_execute(owner, "bash-policy-denied", "bash", bash_args, 1000,
                                            &result));
   assert(result.tool_is_error && !bash_opens && !bash_closes);
   aimee_block_free_contents(&result);
   policy_denied = 0;
   assert(!server_acp_resources_mcp_execute(owner, "bash-actual-source", "bash", bash_args, 1000,
                                            &result));
   assert(!result.tool_is_error && bash_opens == 1 && bash_closes == 1 &&
          mcp_dispatches == previous_mcp + 4);
   aimee_block_free_contents(&result);
   assert(server_acp_resources_mcp_execute(owner, "bash-actual-source", "bash", bash_args, 1000,
                                           &result));
   assert(bash_opens == 1 && bash_closes == 1);
   bash_open_fail = 1;
   assert(server_acp_resources_mcp_execute(owner, "bash-owner-refused", "bash", bash_args, 1000,
                                           &result));
   assert(bash_opens == 2 && bash_closes == 2 && mcp_dispatches == previous_mcp + 4);
   cJSON_Delete(bash_args);
   cJSON_Delete(mcp_args);
   aimee_ir_session_set_sink(original_sink, NULL);
   int sink_before = sink_calls;
   capture.failed = 1;
   cancelled = 1;
   aimee_session_event_t terminal = {.kind = AIMEE_SESSION_EXECUTION,
                                     .execution_status = "failed",
                                     .execution_error = "cancelled or failed capture"};
   assert(!server_acp_resources_sink(&terminal, owner));
   assert(sink_calls ==
          sink_before + 1); /* No resource gate may suppress owner failure terminal. */
   capture.failed = 0;
   cancelled = 0;
   server_acp_resources_close(owner);
   aimee_ir_session_set_sink(NULL, NULL);
   cJSON_Delete(rows);
   unlinkat(root_fd, "utf8", 0);
   unlinkat(root_fd, "out", 0);
   unlinkat(root_fd, "partial", 0);
   unlinkat(root_fd, "headless", 0);
   unlinkat(root_fd, "long", 0);
   unlinkat(root_fd, "invalid", 0);
   unlinkat(root_fd, "link", 0);
   unlinkat(root_fd, "dirlink", 0);
   unlinkat(root_fd, "dir", AT_REMOVEDIR);
   close(root_fd);
   assert(!rmdir(root_path));
   assert(policy_calls > 0 && sink_calls > 0);
   puts("ACP resource source/once/range/UTF8/no-follow/policy refusal: PASS");
   return 0;
}
