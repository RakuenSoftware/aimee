#include "server_native_history.h"
#include <aimee/ir/session_event.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char *durable;
static int unavailable;
int db1_session_native_history_load(const char *actor, const char *session, const char *mode,
                                    const char *active, char **out)
{
   assert(!strcmp(actor, "owner") && !strcmp(session, "s") && !strcmp(mode, "running") &&
          !strcmp(active, "next"));
   *out = unavailable ? NULL : strdup(durable);
   return unavailable ? -1 : 0;
}
static const char message[] =
    "The host lost this tool's completion result. Its effects are unknown; interruption does not "
    "prove that it failed or was cancelled. Do not repeat an effect without explicit user "
    "authorization.";
int main(void)
{
   const char *raw =
       "{\"messages\":[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"tool_use\",\"tool_id\":"
       "\"call\",\"name\":\"bash\",\"input\":{\"exact\":123.123456789012345678901234567890}}]},{"
       "\"role\":\"tool\",\"blocks\":[{\"type\":\"tool_result\",\"tool_id\":\"call\",\"content\":"
       "\"REPLACE\",\"is_error\":true,\"context\":{\"origin\":1,\"authority\":0,\"trust\":4,"
       "\"sensitivity\":1,"
       "\"model_visible\":true,\"revision_domain\":\"native-interruption\",\"revision_scope\":"
       "\"receipt\"}}]}],\"interruptions\":[{\"event_id\":\"receipt\",\"receipt\":{\"version\":1,"
       "\"source\":{\"session_id\":\"s\",\"turn_id\":\"old\",\"sequence\":\"2\",\"event_id\":"
       "\"original\",\"actor\":\"owner\",\"message_id\":\"a\",\"block_index\":0,\"tool_id\":"
       "\"call\",\"name\":\"bash\"},\"owner\":{\"owner_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
       "\"binding_sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
       "\"epoch\":\"0\",\"containment_sha256\":"
       "\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"},\"terminal_"
       "sequence\":\"3\",\"outcome\":\"unknown\",\"retry\":\"explicit_permission_required\"}}]}";
   cJSON *root = aimee_ir_json_parse_frame(raw, strlen(raw));
   assert(root);
   cJSON *messages = cJSON_GetObjectItemCaseSensitive(root, "messages"),
         *receipts = cJSON_GetObjectItemCaseSensitive(root, "interruptions");
   cJSON *block = cJSON_GetArrayItem(
       cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(messages, 1), "blocks"), 0);
   assert(cJSON_ReplaceItemInObjectCaseSensitive(block, "content", cJSON_CreateString(message)));
   assert(cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(block, "context"),
                                  "revision_epoch", "0"));
   assert(cJSON_AddItemToObject(
       cJSON_GetArrayItem(receipts, 0), "call",
       cJSON_Duplicate(
           cJSON_GetArrayItem(
               cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(messages, 0), "blocks"), 0),
           1)));
   durable = cJSON_PrintUnformatted(root);
   assert(durable);
   char *before = cJSON_PrintUnformatted(messages);
   assert(aimee_ir_history_admit(messages) == -1);
   assert(!server_native_history_admit(messages, receipts));
   char *after = cJSON_PrintUnformatted(messages);
   assert(!strcmp(before, after));
   free(before);
   free(after);
   cJSON *originalCall = cJSON_GetArrayItem(
       cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(messages, 0), "blocks"), 0);
   cJSON *inputBefore = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(originalCall, "input"), 1);
   const char *changedRaw = "{\"exact\":123.123456789012345678901234567891}";
   cJSON *changedInput = aimee_ir_json_parse_frame(changedRaw, strlen(changedRaw));
   assert(changedInput &&
          cJSON_ReplaceItemInObjectCaseSensitive(originalCall, "input", changedInput));
   assert(server_native_history_admit(messages, receipts) == -1);
   assert(cJSON_ReplaceItemInObjectCaseSensitive(originalCall, "input", inputBefore));
   assert(!server_native_history_admit(messages, receipts));
   cJSON *none = cJSON_CreateArray();
   assert(server_native_history_admit(messages, none) == -1);
   cJSON_Delete(none);
   cJSON *loaded = NULL, *authority = NULL;
   assert(!server_native_history_load("s", "owner", "next", 1, &loaded, &authority));
   assert(server_native_history_requires_permission(authority) == 1);
   assert(cJSON_Compare(messages, loaded, 1) && cJSON_Compare(receipts, authority, 1));
   cJSON_Delete(loaded);
   cJSON_Delete(authority);
   cJSON *receipt = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(receipts, 0), "receipt");
   assert(
       cJSON_ReplaceItemInObjectCaseSensitive(receipt, "outcome", cJSON_CreateString("succeeded")));
   assert(server_native_history_admit(messages, receipts) == -1);
   assert(
       cJSON_ReplaceItemInObjectCaseSensitive(receipt, "outcome", cJSON_CreateString("unknown")));
   cJSON *entry = cJSON_GetArrayItem(receipts, 0);
   cJSON *source = cJSON_GetObjectItemCaseSensitive(receipt, "source");
   cJSON *native = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(receipt, "owner"), 1);
   cJSON *operator= cJSON_CreateObject();
   assert(cJSON_AddStringToObject(operator, "operator_cancel_event_id", "operator-cancel:old"));
   assert(cJSON_ReplaceItemInObjectCaseSensitive(receipt, "owner", operator));
   assert(cJSON_ReplaceItemInObjectCaseSensitive(
       entry, "event_id", cJSON_CreateString("operator-interruption:old:2:0")));
   cJSON *ctx = cJSON_GetObjectItemCaseSensitive(block, "context");
   assert(cJSON_ReplaceItemInObjectCaseSensitive(
       ctx, "revision_scope", cJSON_CreateString("operator-interruption:old:2:0")));
   assert(!server_native_history_admit(messages, receipts));
   assert(cJSON_AddStringToObject(operator, "owner_id", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
   assert(server_native_history_admit(messages, receipts) == -1);
   cJSON_DeleteItemFromObjectCaseSensitive(operator, "owner_id");
   assert(cJSON_ReplaceItemInObjectCaseSensitive(operator, "operator_cancel_event_id",
                                                 cJSON_CreateString("operator-cancel:foreign")));
   assert(server_native_history_admit(messages, receipts) == -1);
   assert(cJSON_ReplaceItemInObjectCaseSensitive(operator, "operator_cancel_event_id",
                                                 cJSON_CreateString("operator-cancel:old")));
   assert(cJSON_ReplaceItemInObjectCaseSensitive(entry, "event_id", cJSON_CreateString("forged")));
   assert(server_native_history_admit(messages, receipts) == -1);
   assert(cJSON_ReplaceItemInObjectCaseSensitive(
       entry, "event_id", cJSON_CreateString("operator-interruption:old:2:0")));
   char long_actor[161];
   memset(long_actor, 'a', 160);
   long_actor[160] = 0;
   assert(cJSON_ReplaceItemInObjectCaseSensitive(source, "actor", cJSON_CreateString(long_actor)));
   assert(server_native_history_admit(messages, receipts) == -1);
   long_actor[159] = 0;
   assert(cJSON_ReplaceItemInObjectCaseSensitive(source, "actor", cJSON_CreateString(long_actor)));
   assert(!server_native_history_admit(messages, receipts));
   assert(cJSON_ReplaceItemInObjectCaseSensitive(source, "actor", cJSON_CreateString("owner")));
   cJSON *saved_input = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(originalCall, "input"), 1);
   assert(cJSON_ReplaceItemInObjectCaseSensitive(originalCall, "input",
                                                 cJSON_Parse("{\"changed\":true}")));
   assert(server_native_history_admit(messages, receipts) == -1);
   assert(cJSON_ReplaceItemInObjectCaseSensitive(originalCall, "input", saved_input));
   cJSON *calls = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(messages, 0), "blocks");
   assert(cJSON_AddItemToArray(calls, cJSON_Duplicate(originalCall, 1)));
   assert(server_native_history_admit(messages, receipts) == -1);
   cJSON_DeleteItemFromArray(calls, 1);
   char *native_durable = durable;
   durable = cJSON_PrintUnformatted(root);
   assert(!server_native_history_load("s", "owner", "next", 1, &loaded, &authority));
   cJSON_Delete(loaded);
   cJSON_Delete(authority);
   assert(cJSON_ReplaceItemInObjectCaseSensitive(source, "actor", cJSON_CreateString("foreign")));
   free(durable);
   durable = cJSON_PrintUnformatted(root);
   assert(server_native_history_load("s", "owner", "next", 1, &loaded, &authority) == -1);
   free(durable);
   durable = native_durable;
   assert(cJSON_ReplaceItemInObjectCaseSensitive(source, "actor", cJSON_CreateString("owner")));
   assert(cJSON_ReplaceItemInObjectCaseSensitive(receipt, "owner", native));
   assert(cJSON_ReplaceItemInObjectCaseSensitive(entry, "event_id", cJSON_CreateString("receipt")));
   assert(cJSON_ReplaceItemInObjectCaseSensitive(ctx, "revision_scope",
                                                 cJSON_CreateString("receipt")));
   assert(!server_native_history_admit(messages, receipts));
   cJSON *context = cJSON_GetObjectItemCaseSensitive(block, "context");
   assert(cJSON_ReplaceItemInObjectCaseSensitive(context, "origin", cJSON_CreateNumber(7)));
   assert(aimee_ir_history_admit(messages) == -1 &&
          server_native_history_admit(messages, receipts) == -1);
   unavailable = 1;
   loaded = (void *)1;
   authority = (void *)1;
   assert(server_native_history_load("s", "owner", "next", 1, &loaded, &authority) == -1 &&
          !loaded && !authority);
   cJSON_Delete(root);
   free(durable);
   puts("native source-owned interruption sidecar admission, spoof refusal and immutable replay "
        "passed");
   return 0;
}
