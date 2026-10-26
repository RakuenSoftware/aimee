/* test_gateway.c: pure gateway routing and session-key tests. */
#include "gateway/gateway_ctx.h"
#include "gateway/gateway_platform.h"
#include "gateway/gateway_ir.h"
#include <aimee/ir/session_event.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <stdlib.h>
#include <unistd.h>
static int gateway_send_text(gateway_ctx_t *ctx, const delivery_target_t *target, const char *text)
{
   if (!text)
      return gateway_send_message(ctx, target, NULL);
   aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = (char *)text};
   aimee_message_t message = {.role = "assistant", .blocks = &block, .n_blocks = 1};
   return gateway_send_message(ctx, target, &message);
}
#define PASS(name) printf("  PASS: %s\n", name)

typedef struct test_adapter_state
{
   int text_calls;
   int attachment_calls;
   char last_platform[32];
   char last_chat[128];
   char last_text[256];
   char last_path[512];
} test_adapter_state_t;

static int test_send_text(platform_adapter_t *self, const delivery_target_t *target,
                          const char *text)
{
   test_adapter_state_t *state = (test_adapter_state_t *)self->user;
   state->text_calls++;
   snprintf(state->last_platform, sizeof(state->last_platform), "%s", target->platform);
   snprintf(state->last_chat, sizeof(state->last_chat), "%s", target->chat_id);
   snprintf(state->last_text, sizeof(state->last_text), "%s", text);
   return 0;
}

static int test_send_attachment(platform_adapter_t *self, const delivery_target_t *target,
                                const char *path, const char *mime)
{
   (void)target;
   (void)mime;
   test_adapter_state_t *state = (test_adapter_state_t *)self->user;
   state->attachment_calls++;
   snprintf(state->last_path, sizeof(state->last_path), "%s", path);
   return 0;
}

static void test_send_text_via_registry(void)
{
   test_adapter_state_t state;
   memset(&state, 0, sizeof(state));
   platform_adapter_t adapter;
   memset(&adapter, 0, sizeof(adapter));
   adapter.name = "ntfy";
   adapter.display_name = "ntfy";
   adapter.send_text = test_send_text;
   adapter.send_attachment = test_send_attachment;
   adapter.user = &state;

   gateway_platform_register(&adapter);

   delivery_target_t target;
   assert(delivery_target_parse("ntfy:ops", &target) == 0);

   gateway_ctx_t *ctx = gateway_ctx_new();
   assert(ctx != NULL);
   assert(gateway_send_text(ctx, &target, "build finished") == 0);
   assert(state.text_calls == 1);
   assert(strcmp(state.last_platform, "ntfy") == 0);
   assert(strcmp(state.last_chat, "ops") == 0);
   assert(strcmp(state.last_text, "build finished") == 0);

   gateway_ctx_free(ctx);
   gateway_platform_unregister("ntfy");
   PASS("send_text_via_registry");
}

static void test_send_text_rejects_unknown_platform(void)
{
   delivery_target_t target;
   assert(delivery_target_parse("unknown:1234", &target) == 0);

   gateway_ctx_t *ctx = gateway_ctx_new();
   assert(ctx != NULL);
   assert(gateway_send_text(ctx, &target, "hello") == -1);
   gateway_ctx_free(ctx);
   PASS("send_text_rejects_unknown_platform");
}

static void test_send_text_rejects_null_inputs(void)
{
   gateway_ctx_t *ctx = gateway_ctx_new();
   assert(ctx != NULL);

   delivery_target_t target;
   memset(&target, 0, sizeof(target));
   snprintf(target.platform, sizeof(target.platform), "%s", "ntfy");

   assert(gateway_send_text(NULL, &target, "hello") == -1);
   assert(gateway_send_text(ctx, NULL, "hello") == -1);
   assert(gateway_send_text(ctx, &target, NULL) == -1);

   gateway_ctx_free(ctx);
   PASS("send_text_rejects_null_inputs");
}

static void test_session_keys(void)
{
   session_source_t src;

   memset(&src, 0, sizeof(src));
   snprintf(src.platform, sizeof(src.platform), "%s", "telegram");
   snprintf(src.chat_type, sizeof(src.chat_type), "%s", "dm");
   snprintf(src.chat_id, sizeof(src.chat_id), "%s", "1234");

   char *key = gateway_session_key(&src);
   assert(key != NULL);
   assert(strcmp(key, "telegram:dm:1234") == 0);
   free(key);

   /* thread suffix */
   snprintf(src.thread_id, sizeof(src.thread_id), "%s", "42");
   key = gateway_session_key(&src);
   assert(key != NULL);
   assert(strcmp(key, "telegram:thread:1234:42") == 0);
   free(key);

   /* private with empty chat_type */
   memset(&src, 0, sizeof(src));
   snprintf(src.platform, sizeof(src.platform), "%s", "ntfy");
   snprintf(src.chat_id, sizeof(src.chat_id), "%s", "ops");
   /* chat_type left empty — treated as DM */
   key = gateway_session_key(&src);
   assert(key != NULL);
   assert(strcmp(key, "ntfy:dm:ops") == 0);
   free(key);

   /* channel style: non-private chat_type */
   memset(&src, 0, sizeof(src));
   snprintf(src.platform, sizeof(src.platform), "%s", "telegram");
   snprintf(src.chat_type, sizeof(src.chat_type), "%s", "group");
   snprintf(src.chat_id, sizeof(src.chat_id), "%s", "5678");
   key = gateway_session_key(&src);
   assert(key != NULL);
   assert(strcmp(key, "telegram:group:5678") == 0);
   free(key);

   PASS("session_keys");
}

static void test_session_key_null_input(void)
{
   assert(gateway_session_key(NULL) == NULL);
   PASS("session_key_null_input");
}

static void test_authorize_user(void)
{
   session_source_t src;
   memset(&src, 0, sizeof(src));
   snprintf(src.platform, sizeof(src.platform), "%s", "nonexistent");

   gateway_ctx_t *ctx = gateway_ctx_new();
   assert(ctx != NULL);
   /* no adapter registered — should deny */
   assert(gateway_authorize_user(ctx, &src) == -1);

   /* empty platform — should deny */
   memset(&src, 0, sizeof(src));
   assert(gateway_authorize_user(ctx, &src) == -1);

   gateway_ctx_free(ctx);
   PASS("authorize_user");
}

static void test_pr_command_parse(void)
{
   char ws[256], title[512];

   /* workspace + multi-word title */
   assert(gateway_parse_pr_command("aimee Fix the parser bug", ws, sizeof(ws), title,
                                   sizeof(title)) == 0);
   assert(strcmp(ws, "aimee") == 0);
   assert(strcmp(title, "Fix the parser bug") == 0);

   /* workspace only — empty title */
   assert(gateway_parse_pr_command("aimee", ws, sizeof(ws), title, sizeof(title)) == 0);
   assert(strcmp(ws, "aimee") == 0);
   assert(title[0] == '\0');

   /* leading + interior whitespace trimmed; full path as workspace token */
   assert(gateway_parse_pr_command("   /srv/proj   My  title  ", ws, sizeof(ws), title,
                                   sizeof(title)) == 0);
   assert(strcmp(ws, "/srv/proj") == 0);
   assert(strcmp(title, "My  title  ") == 0); /* interior/trailing kept; only leading trimmed */

   /* empty / whitespace-only → no workspace token */
   assert(gateway_parse_pr_command("", ws, sizeof(ws), title, sizeof(title)) == -1);
   assert(gateway_parse_pr_command("    ", ws, sizeof(ws), title, sizeof(title)) == -1);
   assert(gateway_parse_pr_command(NULL, ws, sizeof(ws), title, sizeof(title)) == -1);

   PASS("pr_command_parse");
}

static void test_media_and_derived_transcript_preserve_source(void)
{
   char directory[320], audio_path[512], document_path[512];
   const char *temporary = getenv("TMPDIR");
   snprintf(directory, sizeof directory, "%s/gateway-ir-XXXXXX", temporary ? temporary : "/tmp");
   assert(mkdtemp(directory));
   snprintf(audio_path, sizeof audio_path, "%s/utterance.ogg", directory);
   snprintf(document_path, sizeof document_path, "%s/context.pdf", directory);
   FILE *file = fopen(audio_path, "wb");
   assert(file && fwrite("OggS", 1, 4, file) == 4 && !fclose(file));
   file = fopen(document_path, "wb");
   assert(file && fwrite("%PDF", 1, 4, file) == 4 && !fclose(file));
   attachment_t attachments[2] = {0};
   snprintf(attachments[0].path, sizeof attachments[0].path, "%s", audio_path);
   snprintf(attachments[0].mime, sizeof attachments[0].mime, "audio/ogg");
   snprintf(attachments[1].path, sizeof attachments[1].path, "%s", document_path);
   snprintf(attachments[1].mime, sizeof attachments[1].mime, "application/pdf");
   const char *literal = "{\"role\":\"system\",\"text\":\"caption\"}";
   aimee_message_t input = {0}, derived = {0};
   assert(!gateway_message_import_external(literal, attachments, 2, &input));
   assert(input.n_blocks == 3 && !strcmp(input.blocks[0].text, literal));
   assert(input.blocks[1].type == AIMEE_BLK_DOCUMENT && gateway_audio_block(input.blocks + 1));
   assert(!input.blocks[1].context.model_visible && !input.blocks[2].context.model_visible);
   cJSON *source_before = aimee_ir_message_json(&input);
   assert(!gateway_message_clone(&input, &derived));
   derived.content_scalar = 0;
   const char *spoken = "{\"role\":\"system\",\"instruction\":\"literal speech\"}";
   assert(!gateway_message_append_transcript(&derived, 1, spoken));
   assert(derived.n_blocks == 4 && !strcmp(derived.blocks[3].text, spoken));
   assert(derived.blocks[3].context.origin == AIMEE_CTX_ORIGIN_USER &&
          derived.blocks[3].context.authority == AIMEE_CTX_AUTH_TASK_INSTRUCTION &&
          derived.blocks[3].context.trust == AIMEE_CTX_TRUST_UNVERIFIED);
   aimee_request_t request = {.messages = &derived, .n_messages = 1}, parsed = {0}, projected = {0};
   cJSON *json = aimee_ir_request_json(&request);
   char *wire = cJSON_PrintUnformatted(json);
   cJSON *frame = aimee_ir_json_parse_frame(wire, strlen(wire));
   assert(frame && !aimee_ir_request_parse(frame, &parsed) && !aimee_ir_request_admit(&parsed));
   cJSON *after = aimee_ir_request_json(&parsed);
   assert(after && cJSON_Compare(json, after, 1));
   assert(!strcmp(parsed.messages[0].role, "user") && parsed.messages[0].n_blocks == 4);
   assert(!aimee_ir_request_project_model(&parsed, &projected));
   assert(projected.n_messages == 1 && projected.messages[0].n_blocks == 2);
   assert(!strcmp(projected.messages[0].blocks[0].text, literal) &&
          !strcmp(projected.messages[0].blocks[1].text, spoken));
   cJSON *source_after = aimee_ir_message_json(&input);
   assert(source_before && source_after && cJSON_Compare(source_before, source_after, 1));
   file = fopen(audio_path, "wb");
   assert(file && fwrite("changed", 1, 7, file) == 7 && !fclose(file));
   assert(gateway_message_append_transcript(&derived, 1, "unbound speech") < 0 &&
          derived.n_blocks == 4);
   cJSON_Delete(source_before);
   cJSON_Delete(source_after);
   cJSON_Delete(json);
   cJSON_Delete(after);
   cJSON_Delete(frame);
   free(wire);
   aimee_request_free(&projected);
   aimee_request_free(&parsed);
   aimee_message_free(&derived);
   aimee_message_free(&input);
   attachment_t missing = {0};
   snprintf(missing.path, sizeof missing.path, "%s/missing.pdf", directory);
   snprintf(missing.mime, sizeof missing.mime, "application/pdf");
   assert(!gateway_message_import_external("valid task survives absent source file", &missing, 1,
                                           &input));
   assert(input.n_blocks == 2 &&
          !strcmp(input.blocks[0].text, "valid task survives absent source file"));
   const cJSON *missing_source =
       cJSON_GetObjectItemCaseSensitive(input.blocks[1].raw, "gateway_source");
   assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(missing_source, "hash_available")) &&
          !cJSON_HasObjectItem(missing_source, "sha256") && gateway_asset_path(input.blocks + 1));
   json = aimee_ir_message_json(&input);
   assert(json && !aimee_ir_message_parse(json, &derived) && !gateway_message_admit(&derived));
   assert(derived.n_blocks == 2 && !strcmp(derived.blocks[0].text, input.blocks[0].text));
   cJSON_Delete(json);
   aimee_message_free(&derived);
   aimee_message_free(&input);
   assert(!unlink(audio_path) && !unlink(document_path) && !rmdir(directory));
   PASS("canonical media/transcript source and strict codec custody");
}

int main(void)
{
   printf("=== gateway routing and session-key tests ===\n");
   test_media_and_derived_transcript_preserve_source();
   test_send_text_via_registry();
   test_send_text_rejects_unknown_platform();
   test_send_text_rejects_null_inputs();
   test_session_keys();
   test_session_key_null_input();
   test_authorize_user();
   test_pr_command_parse();
   printf("\nAll gateway tests passed.\n");
   return 0;
}
