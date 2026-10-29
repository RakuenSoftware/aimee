/* Real UDS/HTTP reader plus the production canonical subscriber. No model process. */
#include "cli_client.h"
#include "cli_chat_stream.h"
#include "cli_ir.h"
#include "cJSON.h"
#include <aimee/ir/session_event.h>
#include "aimee_sha256.h"
#include <aimee/protocols/acp/acp_server.h>
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "../cli_chat_stream.c"
static aimee_context_meta_t block_context(aimee_context_origin_t origin,
                                          aimee_context_authority_t authority, int visible)
{
   return (aimee_context_meta_t){.origin = origin,
                                 .authority = authority,
                                 .trust = AIMEE_CTX_TRUST_UNVERIFIED,
                                 .sensitivity = AIMEE_CTX_SENS_INTERNAL,
                                 .model_visible = visible};
}

static int message_admit(const aimee_message_t *message)
{
   aimee_request_t view = {.messages = (aimee_message_t *)message, .n_messages = 1};
   return message ? aimee_ir_request_admit(&view) : -1;
}

/* One user text block, admitted the way an external client's turn is. */
static int user_text_message(const char *text, aimee_message_t *out)
{
   memset(out, 0, sizeof *out);
   out->role = strdup("user");
   out->blocks = calloc(1, sizeof *out->blocks);
   if (!out->role || !out->blocks)
      goto failed;
   out->blocks[0].type = AIMEE_BLK_TEXT;
   out->blocks[0].context =
       block_context(AIMEE_CTX_ORIGIN_USER, AIMEE_CTX_AUTH_TASK_INSTRUCTION, 1);
   if (!(out->blocks[0].text = strdup(text)))
      goto failed;
   out->n_blocks = 1;
   if (message_admit(out))
      goto failed;
   return 0;
failed:
   aimee_message_free(out);
   return -1;
}

/* The model-visible text of the authored messages, concatenated. */
static char *visible_text(const aimee_message_t *messages, int count)
{
   size_t size = 1, used = 0;
   for (int m = 0; m < count; m++)
      for (int i = 0; i < messages[m].n_blocks; i++)
         if (messages[m].blocks[i].type == AIMEE_BLK_TEXT &&
             aimee_ir_block_model_visible(messages[m].blocks + i) && messages[m].blocks[i].text)
            size += strlen(messages[m].blocks[i].text);
   char *text = malloc(size);
   if (!text)
      return NULL;
   for (int m = 0; m < count; m++)
      for (int i = 0; i < messages[m].n_blocks; i++)
      {
         const aimee_block_t *b = messages[m].blocks + i;
         if (b->type == AIMEE_BLK_TEXT && aimee_ir_block_model_visible(b) && b->text)
         {
            memcpy(text + used, b->text, strlen(b->text));
            used += strlen(b->text);
         }
      }
   text[used] = 0;
   return text;
}

/* A canonical turn as an in-process client sends it: message, session, cwd. */
static int chat_send_stream_ir(const char *endpoint, const char *session,
                               const aimee_message_t *message, cli_chat_result_t *reply)
{
   cJSON *req = cJSON_CreateObject();
   cJSON *canonical = aimee_ir_message_json(message);
   if (!req || !canonical || !cJSON_AddItemToObject(req, "message", canonical))
   {
      cJSON_Delete(canonical);
      cJSON_Delete(req);
      return -1;
   }
   cJSON_AddStringToObject(req, "aimee_session_id", session);
   cJSON_AddStringToObject(req, "cwd", "/");
   int rc = cli_chat_request_stream_ir(endpoint, NULL, req, 120000, reply);
   cJSON_Delete(req);
   return rc;
}

static cli_chat_result_t canonical_result;
static const aimee_message_t *expected_request_message;
static char home[96], wire[1 << 20], observed[1024];
static int listener, status_code = 200, check_negotiation = 1, text_calls, tool_calls;
static uint64_t cursor;
static const char *sid = "owned-session", *turn = "0123456789abcdef0123456789abcdef";
const char *aimee_home(void)
{
   return home;
}
int cli_v1_has_remote_endpoint(void)
{
   return 0;
}
char *cli_v1_client_endpoint(void)
{
   return NULL;
}
char *cli_v1_client_bearer(void)
{
   return NULL;
}
static void frame(cJSON *event, const char *override_turn)
{
   cJSON *row = cJSON_CreateObject();
   char sequence[32], id[64];
   snprintf(sequence, sizeof sequence, "%" PRIu64, ++cursor);
   snprintf(id, sizeof id, "event-%" PRIu64, cursor);
   cJSON_AddNumberToObject(row, "version", 1);
   cJSON_AddStringToObject(row, "session_id", sid);
   cJSON_AddStringToObject(row, "turn_id", override_turn ? override_turn : turn);
   cJSON_AddStringToObject(row, "actor", "alice");
   cJSON_AddStringToObject(row, "event_id", id);
   cJSON_AddStringToObject(row, "sequence", sequence);
   cJSON_AddItemToObject(row, "event", event);
   char *line = cJSON_PrintUnformatted(row);
   assert(line && strlen(wire) + strlen(line) + 2 < sizeof wire);
   strcat(wire, line);
   strcat(wire, "\n");
   free(line);
   cJSON_Delete(row);
}
static void lifecycle(const char *status, const char *other)
{
   cJSON *e = cJSON_CreateObject();
   cJSON_AddStringToObject(e, "kind", "execution");
   cJSON_AddStringToObject(cJSON_AddObjectToObject(e, "payload"), "status", status);
   frame(e, other);
}
static void snapshot(const char *id, aimee_block_t *blocks, int count, const char *role,
                     int partial)
{
   aimee_message_t m = {.role = (char *)role, .blocks = blocks, .n_blocks = count};
   cJSON *e = cJSON_CreateObject();
   cJSON_AddStringToObject(e, "kind", "message");
   cJSON_AddStringToObject(e, "message_id", id);
   cJSON_AddItemToObject(e, "payload", aimee_ir_message_json(&m));
   if (partial)
      cJSON_AddBoolToObject(e, "partial", 1);
   frame(e, NULL);
}
static void text_snapshot(const char *value, int partial)
{
   aimee_block_t b = {.type = AIMEE_BLK_TEXT, .text = (char *)value};
   snapshot("model-message", &b, 1, "assistant", partial);
}
static void delta(const char *type, const char *chunk)
{
   cJSON *e = cJSON_CreateObject();
   cJSON_AddStringToObject(e, "kind", "delta");
   cJSON_AddStringToObject(e, "message_id", "model-message");
   cJSON_AddStringToObject(e, "role", "assistant");
   cJSON *p = cJSON_AddObjectToObject(e, "payload");
   cJSON_AddStringToObject(p, "type", type);
   if (!strncmp(type, "BLOCK_", 6))
   {
      cJSON_AddStringToObject(p, "kind", "text");
      cJSON_AddNumberToObject(p, "block_id", 0);
   }
   if (chunk)
      cJSON_AddStringToObject(p, "text_delta", chunk);
   frame(e, NULL);
}
static void *serve(void *unused)
{
   (void)unused;
   int fd = accept(listener, NULL, NULL);
   assert(fd >= 0);
   char req[8192] = {0};
   size_t n = 0;
   char *body = NULL;
   for (;;)
   {
      ssize_t got = read(fd, req + n, sizeof req - n - 1);
      assert(got > 0);
      n += (size_t)got;
      req[n] = 0;
      body = strstr(req, "\r\n\r\n");
      if (!body)
         continue;
      const char *length = strstr(req, "Content-Length:");
      assert(length);
      size_t bytes = strtoul(length + 15, NULL, 10);
      body += 4;
      if (n - (size_t)(body - req) >= bytes)
         break;
   }
   assert(strstr(req, "POST /v1/chat/stream HTTP/1.1"));
   cJSON *request = cJSON_Parse(body);
   assert(request);
   if (check_negotiation)
   {
      assert(aimee_ir_json_unique(request));
      assert(cJSON_GetObjectItem(request, "ir_version")->valuedouble == 1);
      assert(cJSON_IsFalse(cJSON_GetObjectItem(request, "control_replies")));
   }
   aimee_message_t input = {0};
   assert(!aimee_ir_message_parse(cJSON_GetObjectItemCaseSensitive(request, "message"), &input));
   assert(!message_admit(&input));
   if (expected_request_message)
   {
      cJSON *a = aimee_ir_message_json(&input),
            *b = aimee_ir_message_json(expected_request_message);
      assert(a && b && cJSON_Compare(a, b, 1));
      cJSON_Delete(a);
      cJSON_Delete(b);
   }
   else
      assert(!strcmp(input.role, "user") && input.n_blocks == 1 &&
             !strcmp(input.blocks[0].text, "hello") &&
             input.blocks[0].context.authority == AIMEE_CTX_AUTH_TASK_INSTRUCTION);
   aimee_message_free(&input);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(request, "aimee_session_id")), sid));
   cJSON_Delete(request);
   char header[256];
   int len = snprintf(
       header, sizeof header,
       "HTTP/1.1 %d Test\r\nContent-Type: application/x-ndjson\r\nConnection: close\r\n\r\n",
       status_code);
   assert(send(fd, header, (size_t)len, MSG_NOSIGNAL) == len);
   /* Split frames across real reads, including headers and newline boundaries. */
   for (size_t at = 0; at < strlen(wire);)
   {
      size_t left = strlen(wire) - at, nbytes = left < 17 ? left : 17;
      ssize_t sent = send(fd, wire + at, nbytes, MSG_NOSIGNAL);
      if (sent <= 0)
         break;
      at += (size_t)sent;
   }
   close(fd);
   return NULL;
}
static void text_callback(const char *text, void *unused)
{
   (void)unused;
   assert(strlen(observed) + strlen(text) < sizeof observed);
   strcat(observed, text);
   text_calls++;
}
static char tool_ids[8][256], phases[8][16];
static void tool_callback(const char *phase, const char *id, const char *name, void *unused)
{
   (void)unused;
   assert(tool_calls < 8);
   snprintf(tool_ids[tool_calls], 256, "%s", id);
   snprintf(phases[tool_calls++], 16, "%s", phase);
   char out[2048];
   assert(acp_build_tool_update("editor", id, phase, name, out, sizeof out) == 1);
   cJSON *j = cJSON_Parse(out),
         *u = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "params"), "update");
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(u, "toolCallId")), id));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(u, "status")),
                  !strcmp(phase, "started") ? "in_progress" : phase));
   cJSON_Delete(j);
}
static char *run(int wrapper)
{
   char socket_path[128], endpoint[136];
   (void)endpoint;
   snprintf(socket_path, sizeof socket_path, "%s/aimee-http.sock", home);
   unlink(socket_path);
   listener = socket(AF_UNIX, SOCK_STREAM, 0);
   assert(listener >= 0);
   struct sockaddr_un a = {.sun_family = AF_UNIX};
   snprintf(a.sun_path, sizeof a.sun_path, "%s", socket_path);
   assert(!bind(listener, (void *)&a, sizeof a) && !listen(listener, 1));
   pthread_t thread;
   assert(!pthread_create(&thread, NULL, serve, NULL));
   char *reply;
   if (wrapper == 2 || wrapper == 3 || wrapper == 4)
   {
      aimee_message_t input = {0};
      assert(!user_text_message("hello", &input));
      const aimee_message_t *source = expected_request_message ? expected_request_message : &input;
      cli_chat_result_free(&canonical_result);
      int rc;
      if (wrapper == 3 || wrapper == 4)
      {
         snprintf(endpoint, sizeof endpoint, "unix:%s", socket_path);
         cJSON *request = cJSON_CreateObject();
         assert(request &&
                cJSON_AddItemToObject(request, "message", aimee_ir_message_json(source)) &&
                cJSON_AddStringToObject(request, "aimee_session_id", sid));
         char *before = cJSON_PrintUnformatted(request);
         if (wrapper == 4)
         {
            cli_http_stream_t *stream = cli_http_stream_new();
            cli_http_options_t options = {
                .endpoint = endpoint, .io_timeout_ms = 2000, .connect_timeout_ms = 1000};
            char *json = NULL;
            assert(stream);
            rc = cli_chat_request_stream_json(stream, &options, before, &json);
            if (!rc)
            {
               cJSON *owned = aimee_ir_json_parse_frame(json, strlen(json));
               cJSON *messages = cJSON_GetObjectItemCaseSensitive(owned, "messages");
               assert(cJSON_IsArray(messages));
               canonical_result.n_messages = cJSON_GetArraySize(messages);
               canonical_result.messages =
                   calloc((size_t)canonical_result.n_messages, sizeof *canonical_result.messages);
               assert(canonical_result.messages);
               for (int i = 0; i < canonical_result.n_messages; i++)
                  assert(!aimee_ir_message_parse(cJSON_GetArrayItem(messages, i),
                                                 canonical_result.messages + i));
               cJSON_Delete(owned);
            }
            cli_chat_string_free(json);
            cli_http_stream_free(stream);
         }
         else
            rc = cli_chat_request_stream_ir(endpoint, NULL, request, 2000, &canonical_result);
         char *after = cJSON_PrintUnformatted(request);
         assert(before && after && !strcmp(before, after));
         free(before);
         free(after);
         cJSON_Delete(request);
      }
      else
      {
         snprintf(endpoint, sizeof endpoint, "unix:%s", socket_path);
         rc = chat_send_stream_ir(endpoint, sid, source, &canonical_result);
      }
      if (rc)
         reply = NULL;
      else
      {
         aimee_message_t *authored =
             calloc((size_t)(canonical_result.n_messages ? canonical_result.n_messages : 1),
                    sizeof *authored);
         assert(authored);
         int n = 0;
         for (int i = 0; i < canonical_result.n_messages; i++)
            if (!strcmp(canonical_result.messages[i].role, "assistant"))
               authored[n++] = canonical_result.messages[i];
         reply = visible_text(authored, n);
         free(authored);
      }
      aimee_message_free(&input);
   }
   else if (wrapper)
      reply = cli_chat_stream(NULL, sid, "hello", text_callback, tool_callback, NULL);
   else
   {
      snprintf(endpoint, sizeof endpoint, "unix:%s", socket_path);
      cJSON *r = cJSON_CreateObject();
      cJSON_AddItemToObject(r, "message", cli_ir_user_message("hello", NULL, NULL));
      cJSON_AddStringToObject(r, "aimee_session_id", sid);
      cJSON_AddNumberToObject(r, "ir_version", 0);
      cJSON_AddBoolToObject(r, "control_replies", 1);
      char *before = cJSON_PrintUnformatted(r);
      reply = cli_chat_request_stream(endpoint, NULL, r, 2000, text_callback, tool_callback, NULL);
      char *after = cJSON_PrintUnformatted(r);
      assert(!strcmp(before, after));
      free(before);
      free(after);
      cJSON_Delete(r);
   }
   assert(!pthread_join(thread, NULL));
   close(listener);
   unlink(socket_path);
   return reply;
}
static void reset(void)
{
   cli_chat_result_free(&canonical_result);
   expected_request_message = NULL;
   wire[0] = observed[0] = 0;
   text_calls = tool_calls = 0;
   cursor = 9007199254740992ULL;
   status_code = 200;
   check_negotiation = 1;
}
int main(int argc, char **argv)
{
   (void)argc;
   (void)argv;
   (void)tool_ids;
   (void)phases;
   (void)text_callback;
   const char *tmpdir = getenv("TMPDIR");
   int path_length = snprintf(home, sizeof home, "%s/acp-ir-%ld-XXXXXX",
                              tmpdir && tmpdir[0] ? tmpdir : "/tmp", (long)getpid());
   assert(path_length > 0 && (size_t)path_length < sizeof home);
   assert((size_t)path_length + strlen("/aimee-http.sock") <
          sizeof(((struct sockaddr_un *)0)->sun_path));
   assert(mkdtemp(home));
   signal(SIGPIPE, SIG_IGN);
   if (argc == 3 && !strcmp(argv[1], "--replay"))
   {
      FILE *file = fopen(argv[2], "rb");
      assert(file);
      assert(!fseek(file, 0, SEEK_END));
      long size = ftell(file);
      assert(size > 0 && size < (long)sizeof wire);
      rewind(file);
      char *bytes = malloc((size_t)size + 1);
      assert(bytes && fread(bytes, 1, (size_t)size, file) == (size_t)size);
      bytes[size] = 0;
      fclose(file);
      cJSON *rows = aimee_ir_json_parse_frame(bytes, (size_t)size);
      assert(cJSON_IsArray(rows));
      free(bytes);
      reset();
      sid = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(rows, 0), "session_id"));
      assert(sid);
      const cJSON *row;
      cJSON_ArrayForEach(row, rows)
      {
         char *line = cJSON_PrintUnformatted(row);
         assert(line && strlen(wire) + strlen(line) + 2 < sizeof wire);
         strcat(wire, line);
         strcat(wire, "\n");
         free(line);
      }
      char *reply = run(2);
      if (!reply)
      {
         fprintf(stderr, "observed canonical replay refused\n");
         cJSON_Delete(rows);
         rmdir(home);
         return 1;
      }
      char hash[65];
      assert(!aimee_sha256_hex(reply, strlen(reply), hash));
      printf("observed replay bytes=%zu sha256=%s\n", strlen(reply), hash);
      free(reply);
      cJSON_Delete(rows);
      cli_chat_result_free(&canonical_result);
      assert(!rmdir(home));
      return 0;
   }
   reset();
   lifecycle("running", NULL);
   delta("TURN_START", NULL);
   delta("BLOCK_START", "Hi");
   delta("BLOCK_DELTA", " there");
   text_snapshot("Hi there", 1);
   text_snapshot("Hi there!", 0);
   delta("TURN_STOP", NULL);
   lifecycle("completed", NULL);
   char *reply = run(1);
   assert(reply && !strcmp(reply, "Hi there!") && !strcmp(observed, "Hi there!") &&
          text_calls == 3);
   free(reply);
   reset();
   lifecycle("running", NULL);
   aimee_block_t calls[2] = {
       {.type = AIMEE_BLK_TOOL_USE, .tool_id = "a", .tool_name = "read_file"},
       {.type = AIMEE_BLK_TOOL_USE, .tool_id = "b", .tool_name = "read_file"}};
   calls[0].tool_input = cJSON_CreateObject();
   calls[1].tool_input = cJSON_CreateObject();
   assert(cJSON_AddNumberToObject(calls[0].tool_input, "limit", 17) &&
          cJSON_AddNumberToObject(calls[1].tool_input, "offset", 9007199254740991.0));
   snapshot("calls", calls, 2, "assistant", 1);
   snapshot("calls", calls, 2, "assistant", 0);
   aimee_block_t result = {.type = AIMEE_BLK_TOOL_RESULT,
                           .tool_id = "b",
                           .tool_result = cJSON_CreateString("failure"),
                           .tool_is_error = 1};
   snapshot("result-b", &result, 1, "tool", 0);
   result.tool_id = "a";
   result.tool_is_error = 0;
   snapshot("result-a", &result, 1, "tool", 0);
   text_snapshot("done", 0);
   lifecycle("completed", NULL);
   reply = run(0);
   assert(reply && !strcmp(reply, "done") && tool_calls == 4);
   assert(strcmp(tool_ids[0], tool_ids[1]) && !strcmp(tool_ids[1], tool_ids[2]) &&
          !strcmp(tool_ids[0], tool_ids[3]));
   assert(!strcmp(phases[2], "failed"));
   free(reply);
   reply = run(3); /* Direct typed API, actual HTTP reader, no display subscriber. */
   assert(reply && !strcmp(reply, "done") && canonical_result.n_messages == 4);
   assert(
       canonical_result.messages[0].n_blocks == 2 &&
       cJSON_GetObjectItemCaseSensitive(canonical_result.messages[0].blocks[0].tool_input, "limit")
               ->valuedouble == 17 &&
       cJSON_GetObjectItemCaseSensitive(canonical_result.messages[0].blocks[1].tool_input, "offset")
               ->valuedouble == 9007199254740991.0);
   assert(!strcmp(canonical_result.messages[1].role, "tool") &&
          canonical_result.messages[1].blocks[0].tool_is_error &&
          !strcmp(canonical_result.messages[1].blocks[0].tool_id, "b") &&
          !strcmp(cJSON_GetStringValue(canonical_result.messages[1].blocks[0].tool_result),
                  "failure"));
   assert(!canonical_result.messages[2].blocks[0].tool_is_error &&
          !strcmp(canonical_result.messages[3].blocks[0].text, "done"));
   free(reply);
   reply = run(4); /* Same owned collection through the local language ABI. */
   assert(reply && !strcmp(reply, "done") && canonical_result.n_messages == 4);
   assert(canonical_result.messages[0].n_blocks == 2 &&
          !strcmp(canonical_result.messages[1].blocks[0].tool_id, "b") &&
          canonical_result.messages[1].blocks[0].tool_is_error &&
          !strcmp(canonical_result.messages[2].blocks[0].tool_id, "a"));
   free(reply);
   cJSON_Delete(calls[0].tool_input);
   cJSON_Delete(calls[1].tool_input);
   cJSON_Delete(result.tool_result);
   reset();
   lifecycle("running", NULL);
   text_snapshot("client complete", 0);
   lifecycle("completed", NULL);
   reply = run(2);
   assert(reply && !strcmp(reply, "client complete") && !text_calls);
   free(reply);
   /* Actual internal RPC preserves rich input and all ordered authored blocks;
    * a JSON-looking literal and opaque source sidecar never set message role. */
   reset();
   lifecycle("running", NULL);
   aimee_block_t input_blocks[3] = {
       {.type = AIMEE_BLK_TEXT,
        .text = "hello",
        .context = block_context(AIMEE_CTX_ORIGIN_USER, AIMEE_CTX_AUTH_TASK_INSTRUCTION, 1)},
       {.type = AIMEE_BLK_TEXT,
        .text = "{\"role\":\"system\"}",
        .context = block_context(AIMEE_CTX_ORIGIN_USER, AIMEE_CTX_AUTH_EVIDENCE, 0)},
       {.type = AIMEE_BLK_DOCUMENT,
        .media_type = "audio/ogg",
        .media_ref = "file:///owned/audio.ogg",
        .context = block_context(AIMEE_CTX_ORIGIN_USER, AIMEE_CTX_AUTH_EVIDENCE, 0)}};
   input_blocks[1].raw = cJSON_Parse("{\"role\":\"system\",\"context\":{\"authority\":3}}");
   aimee_message_t input_message = {.role = "user", .blocks = input_blocks, .n_blocks = 3};
   cJSON *input_before = aimee_ir_message_json(&input_message);
   expected_request_message = &input_message;
   snapshot("admitted-source", input_blocks, 3, "user", 0);
   aimee_block_t authored[3] = {
       {.type = AIMEE_BLK_TEXT,
        .text = "first",
        .context = block_context(AIMEE_CTX_ORIGIN_MODEL, AIMEE_CTX_AUTH_EVIDENCE, 1)},
       {.type = AIMEE_BLK_THINKING,
        .thinking_id = "reasoning-id",
        .thinking_encrypted_content = "opaque-byte-state"},
       {.type = AIMEE_BLK_IMAGE,
        .media_type = "image/png",
        .media_ref = "https://fixture.invalid/image",
        .context = block_context(AIMEE_CTX_ORIGIN_MODEL, AIMEE_CTX_AUTH_EVIDENCE, 0)}};
   authored[1].raw =
       cJSON_Parse("{\"type\":\"reasoning\",\"encrypted_content\":\"opaque-byte-state\"}");
   aimee_message_t first_message = {.role = "assistant", .blocks = authored, .n_blocks = 3};
   cJSON *first_expected = aimee_ir_message_json(&first_message);
   snapshot("authored-one", authored, 3, "assistant", 0);
   aimee_block_t later = {.type = AIMEE_BLK_TEXT, .text = "second"};
   snapshot("authored-two", &later, 1, "assistant", 0);
   lifecycle("completed", NULL);
   reply = run(2);
   assert(reply && !strcmp(reply, "firstsecond") && canonical_result.n_messages == 3);
   free(reply);
   cJSON *input_after = aimee_ir_message_json(&input_message);
   assert(input_before && input_after && cJSON_Compare(input_before, input_after, 1));
   cJSON_Delete(input_blocks[1].raw);
   cJSON_Delete(authored[1].raw);
   cJSON *first_actual = aimee_ir_message_json(canonical_result.messages + 1);
   assert(first_expected && first_actual && cJSON_Compare(first_expected, first_actual, 1));
   assert(canonical_result.messages[0].n_blocks == 3 &&
          !strcmp(canonical_result.messages[0].role, "user") &&
          canonical_result.messages[1].n_blocks == 3 &&
          !strcmp(canonical_result.messages[2].blocks[0].text, "second"));
   cJSON_Delete(first_expected);
   cJSON_Delete(first_actual);
   cJSON_Delete(input_before);
   cJSON_Delete(input_after);
   reset();
   strcpy(wire, "{\"text\":\"partial legacy poison\"}\n{\"status\":\"error\"}\n");
   status_code = 500;
   assert(!run(2));
   for (int mode = 0; mode < 19; mode++)
   {
      reset();
      lifecycle("running", NULL);
      if (mode == 0)
      {
         text_snapshot("partial", 0);
         delta("TURN_STOP", NULL);
      }
      if (mode == 1 || mode == 2)
      {
         text_snapshot("partial", 0);
         lifecycle(mode == 1 ? "failed" : "cancelled", NULL);
      }
      if (mode == 3)
      {
         text_snapshot("bad http", 0);
         lifecycle("completed", NULL);
         status_code = 503;
      }
      if (mode == 4)
      {
         cursor++;
         text_snapshot("gap", 0);
         lifecycle("completed", NULL);
      }
      if (mode == 5)
      {
         cursor--;
         text_snapshot("duplicate", 0);
         lifecycle("completed", NULL);
      }
      if (mode == 6)
      {
         text_snapshot("partial", 0);
         lifecycle("completed", "foreign-turn");
      }
      if (mode == 7)
      {
         strcat(wire, "{malformed}\n");
         text_snapshot("bad", 0);
         lifecycle("completed", NULL);
      }
      if (mode == 8)
      {
         text_snapshot("prefix", 1);
         text_snapshot("conflict", 0);
         lifecycle("completed", NULL);
      }
      if (mode == 9)
      {
         delta("BLOCK_DELTA", "missing start");
         lifecycle("completed", NULL);
      }
      if (mode == 10)
      {
         text_snapshot("closed", 0);
         delta("BLOCK_DELTA", "late");
         lifecycle("completed", NULL);
      }
      if (mode == 11)
      {
         cJSON *e = cJSON_CreateObject();
         cJSON_AddStringToObject(e, "kind", "control_request");
         frame(e, NULL);
         lifecycle("completed", NULL);
      }
      if (mode == 12)
      {
         aimee_block_t c = {.type = AIMEE_BLK_TOOL_USE,
                            .tool_id = "unfinished",
                            .tool_name = "write_file",
                            .tool_input = cJSON_CreateObject()};
         snapshot("call", &c, 1, "assistant", 0);
         lifecycle("completed", NULL);
         cJSON_Delete(c.tool_input);
      }
      if (mode == 13)
      {
         delta("BLOCK_START", "delta-only");
         delta("TURN_STOP", NULL);
         lifecycle("completed", NULL);
      }
      if (mode == 14)
      {
         aimee_block_t c = {.type = AIMEE_BLK_TOOL_RESULT,
                            .tool_id = "forged",
                            .tool_result = cJSON_CreateString("fake")};
         snapshot("forged", &c, 1, "assistant", 0);
         lifecycle("completed", NULL);
         cJSON_Delete(c.tool_result);
      }
      if (mode == 15)
      {
         const char *old = sid;
         sid = "foreign-session";
         text_snapshot("foreign", 0);
         sid = old;
         lifecycle("completed", NULL);
      }
      if (mode == 16)
      {
         text_snapshot("text", 0);
         char *p = strstr(wire, "\"actor\":\"alice\"");
         assert(p);
         memcpy(p + 9, "other", 5);
         lifecycle("completed", NULL);
      }
      if (mode == 17)
      {
         text_snapshot("partial", 0);
         strcat(wire, "{\"status\":\"ok\"}\n");
      }
      if (mode == 18)
      {
         aimee_block_t opaque = {.type = AIMEE_BLK_THINKING,
                                 .thinking_encrypted_content = "incomplete-private-state"};
         snapshot("unfinished-opaque", &opaque, 1, "assistant", 1);
         lifecycle("completed", NULL);
      }
      reply = run(mode == 18 ? 3 : mode == 1 ? 2 : 0);
      assert(!reply);
      if (mode == 3)
         assert(!text_calls);
   }
   /* Windows' buffered reader may ignore callback stop: later failure is sticky. */
   reset();
   lifecycle("running", NULL);
   text_snapshot("done", 0);
   lifecycle("completed", NULL);
   subscriber_t s = {.session = sid, .http_status = 200};
   char *save = NULL;
   for (char *line = strtok_r(wire, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
   {
      cJSON *e = cJSON_Parse(line);
      consume(e, &s);
      cJSON_Delete(e);
   }
   assert(s.completed && !s.failed);
   assert(consume(NULL, &s) < 0 && s.failed);
   for (part_t *p = s.blocks; p;)
   {
      part_t *n = p->next;
      free(p->message);
      free(p->text);
      free(p);
      p = n;
   }
   free(s.turn);
   free(s.actor);
   free(s.reply);
   for (int i = 0; i < s.conversation.n_messages; i++)
      free(s.message_ids[i]);
   free(s.message_ids);
   free(s.message_final);
   aimee_request_free(&s.conversation);
   cli_chat_result_free(&canonical_result);
   assert(!rmdir(home));
   puts("canonical ACP UDS subscriber: terminal, identity, bounded prefix and tool "
        "ownership passed");
   return 0;
}
