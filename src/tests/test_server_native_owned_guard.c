/* Exercise the CURRENT static guard itself with real IR/source preservation.
 * Only transport/session endpoints are controlled peers; this is not Native
 * receipt, grant, Model permission or historical proof qualification. */
#include "../server/server_native_runtime.c"
#include <assert.h>
static int restores, imports, failures, refuse_import, refuse_restore;
int server_native_replay_restore(aimee_request_t *request, void *context)
{
   (void)request;
   (void)context;
   restores++;
   return refuse_restore ? -1 : 0;
}
const char *server_session_capture_session(const server_session_capture_t *capture)
{
   (void)capture;
   return "owned-guard-fixture";
}
const char *server_session_capture_turn(const server_session_capture_t *capture)
{
   (void)capture;
   return "owned-guard-turn";
}
void server_session_capture_fail(server_session_capture_t *capture)
{
   (void)capture;
   failures++;
}
int db1_session_native_history_load(const char *actor, const char *session, const char *mode,
                                    const char *active, char **out)
{
   (void)actor;
   (void)session;
   (void)mode;
   (void)active;
   *out = NULL;
   return -1;
}
int aimee_module_history_tool_source_accept(const cJSON *source, const char *visible)
{
   assert(aimee_tool_source_visible_valid(source, visible));
   imports++;
   return refuse_import ? -1 : 0;
}
static aimee_request_t make_request(int count)
{
   aimee_request_t r = {
       .frontend = AIMEE_WIRE_RESPONSES, .model = strdup("guard-fixture"), .mutated = 1};
   r.n_messages = count * 2;
   r.messages = calloc((size_t)r.n_messages, sizeof *r.messages);
   assert(r.messages);
   for (int i = 0; i < count; i++)
   {
      char id[32], text[32], sha[65];
      snprintf(id, sizeof id, "call-%d", i);
      snprintf(text, sizeof text, "guard result %d", i);
      assert(!aimee_sha256_hex(text, strlen(text), sha));
      aimee_message_t *call = &r.messages[i * 2], *result = &r.messages[i * 2 + 1];
      call->role = strdup("assistant");
      call->n_blocks = 1;
      call->blocks = calloc(1, sizeof *call->blocks);
      assert(call->blocks);
      call->blocks[0] = (aimee_block_t){.type = AIMEE_BLK_TOOL_USE,
                                        .tool_id = strdup(id),
                                        .tool_name = strdup("fixture_tool"),
                                        .tool_input = cJSON_CreateObject()};
      result->role = strdup("tool");
      result->n_blocks = 1;
      result->blocks = calloc(1, sizeof *result->blocks);
      assert(result->blocks);
      cJSON *source = cJSON_CreateObject();
      assert(source);
      cJSON_AddNumberToObject(source, "schema_version", 1);
      cJSON_AddStringToObject(source, "owner", "81000000-0000-4000-8000-000000000001");
      cJSON_AddStringToObject(source, "visible_sha256", sha);
      cJSON_AddStringToObject(source, "receipt", "{\"controlled_peer\":true}");
      result->blocks[0] = (aimee_block_t){.type = AIMEE_BLK_TOOL_RESULT,
                                          .tool_id = strdup(id),
                                          .tool_result = cJSON_CreateString(text),
                                          .tool_source = source};
   }
   assert(!aimee_ir_request_admit(&r));
   return r;
}
static void reset(void)
{
   restores = imports = failures = refuse_import = refuse_restore = 0;
}
static void release_owner(native_request_owner_t *owner)
{
   aimee_request_free(&owner->request);
   aimee_request_free(&owner->admitted);
}
int main(void)
{
   server_native_runtime_t runtime = {0};
   snprintf(runtime.owner.vault_principal, sizeof runtime.owner.vault_principal, "%s",
            "fixture-owner");
   native_request_owner_t owner = {
       .runtime = &runtime, .request = make_request(2), .prepared = 1, .require_complete = 1};
   aimee_request_t request = {0};
   assert(!aimee_ir_request_clone(&owner.request, &request));
   reset();
   assert(!native_owned_guard(&request, &owner));
   assert(imports == 2 && restores == 1);
   assert(!native_owned_guard(&request, &owner));
   assert(imports == 2 && restores == 2);
   /* Tool policy/catalog normalization still runs after an already imported history. */
   free(request.model);
   request.model = strdup("post-policy-model");
   assert(!native_owned_guard(&request, &owner));
   assert(imports == 2 && restores == 3);
   assert(owner.admitted.model && !strcmp(owner.admitted.model, "post-policy-model"));
   cJSON *source = request.messages[1].blocks[0].tool_source;
   assert(cJSON_ReplaceItemInObjectCaseSensitive(source, "receipt",
                                                 cJSON_CreateString("{\"changed_peer\":true}")));
   assert(native_owned_guard(&request, &owner) == -1);
   assert(imports == 2 && failures == 1);
   aimee_request_free(&request);
   assert(!aimee_ir_request_clone(&owner.request, &request));
   cJSON_Delete(request.messages[1].blocks[0].tool_result);
   request.messages[1].blocks[0].tool_result = cJSON_CreateString("altered visible");
   assert(native_owned_guard(&request, &owner) == -1);
   assert(imports == 2);
   aimee_request_free(&request);
   request = make_request(3);
   assert(native_owned_guard(&request, &owner) == -1);
   assert(imports == 2);
   /* Dropping a complete previously sourced call/result is permitted by the
    * existing forward preservation contract, after all canonical sources imported. */
   aimee_request_free(&request);
   request = make_request(1);
   assert(!native_owned_guard(&request, &owner));
   assert(imports == 2);
   refuse_restore = 1;
   assert(native_owned_guard(&request, &owner) == -1);
   assert(imports == 2);
   refuse_restore = 0;
   aimee_request_free(&request);
   release_owner(&owner);
   /* An unexpected first partial canonical view cannot establish import-once. */
   owner = (native_request_owner_t){
       .runtime = &runtime, .request = make_request(2), .prepared = 1, .require_complete = 1};
   request = make_request(1);
   reset();
   assert(native_owned_guard(&request, &owner) == -1);
   assert(imports == 0);
   aimee_request_free(&request);
   assert(!aimee_ir_request_clone(&owner.request, &request));
   refuse_import = 1;
   assert(native_owned_guard(&request, &owner) == -1);
   assert(imports == 1);
   refuse_import = 0;
   assert(!native_owned_guard(&request, &owner));
   assert(imports == 3);
   aimee_request_free(&request);
   release_owner(&owner);
   /* A new owned request never inherits another request's import bit. */
   owner = (native_request_owner_t){
       .runtime = &runtime, .request = make_request(2), .prepared = 1, .require_complete = 1};
   assert(!aimee_ir_request_clone(&owner.request, &request));
   int before = imports;
   assert(!native_owned_guard(&request, &owner));
   assert(imports == before + 2);
   aimee_request_free(&request);
   release_owner(&owner);
   puts("native owned guard import-once controls PASS");
   return 0;
}
