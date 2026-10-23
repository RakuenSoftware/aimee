/* test_provider_client.c: shared provider-client request builder + response parser.
 * Network-free: exercises the pure build/parse halves (provider_client_complete_ir's
 * transport is covered by integration once a curator endpoint is wired). */
#include "provider_client.h"
#include "cJSON.h"
#include "support/mock_agent_http.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void assert_text(const provider_completion_t *out, const char *expected)
{
   char *text = aimee_ir_response_text_alloc(&out->ir);
   assert(text && strcmp(text, expected) == 0);
   free(text);
}

static aimee_request_t complete_request(void);
static cJSON *captured_request;
static int capture_build_request(const char *url, const char *auth, const char *body,
                                 char **response, int timeout, const char *extra)
{
   (void)url;
   (void)auth;
   (void)timeout;
   (void)extra;
   assert(!captured_request);
   captured_request = cJSON_Parse(body);
   assert(captured_request);
   *response = strdup("{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":"
                      "\"assistant\",\"content\":\"captured\"}}]}");
   assert(*response);
   return 200;
}

static void test_build_basic(void)
{
   provider_def_t def = {
       .base_url = "http://h:8080/v1",
       .model = "granite-3b",
       .wire = PROVIDER_WIRE_OPENAI_CHAT,
       .temperature = -1.0, /* omit */
       .max_tokens = 0,     /* omit */
   };
   aimee_request_t request = complete_request();
   provider_completion_t result = {0};
   mock_agent_http_set_post_handler(capture_build_request);
   assert(!provider_client_complete_ir(&def, &request, &result, NULL, 0));
   cJSON *req = captured_request;
   captured_request = NULL;
   assert(req && result.http_status == 200);

   cJSON *model = cJSON_GetObjectItemCaseSensitive(req, "model");
   assert(cJSON_IsString(model) && strcmp(model->valuestring, "granite-3b") == 0);
   /* messages copied, not aliased: mutating the original must not change req. */
   cJSON *rmsgs = cJSON_GetObjectItemCaseSensitive(req, "messages");
   assert(cJSON_IsArray(rmsgs) && cJSON_GetArraySize(rmsgs) == 1);
   /* omitted optionals absent */
   assert(!cJSON_GetObjectItemCaseSensitive(req, "temperature"));
   assert(!cJSON_GetObjectItemCaseSensitive(req, "max_tokens"));
   assert(!cJSON_GetObjectItemCaseSensitive(req, "response_format"));
   /* disable_thinking defaults off => no chat_template_kwargs. */
   assert(!cJSON_GetObjectItemCaseSensitive(req, "chat_template_kwargs"));

   request.messages[0].blocks[0].text[0] = 'X';
   assert(!strcmp(cJSON_GetStringValue(
                      cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rmsgs, 0), "content")),
                  "extract entities"));
   cJSON_Delete(req);
   aimee_request_free(&request);
   provider_completion_free(&result);
   mock_agent_http_reset();
   printf("provider_client: build basic ok\n");
}

static void test_build_options_and_schema(void)
{
   provider_def_t def = {
       .base_url = "http://h:8080/v1",
       .model = "granite-3b",
       .wire = PROVIDER_WIRE_OPENAI_CHAT,
       .temperature = 0.2,
       .max_tokens = 512,
       .disable_thinking = 1,
   };
   aimee_request_t request = complete_request();
   request.output_format = (aimee_output_format_t){.kind = AIMEE_OUTPUT_JSON_SCHEMA,
                                                   .name = strdup("curator_output"),
                                                   .schema = cJSON_Parse("{\"type\":\"object\"}"),
                                                   .strict = AIMEE_STRICT_TRUE};
   assert(request.output_format.name && request.output_format.schema);
   provider_completion_t result = {0};
   mock_agent_http_set_post_handler(capture_build_request);
   assert(!provider_client_complete_ir(&def, &request, &result, NULL, 0));
   cJSON *req = captured_request;
   captured_request = NULL;
   assert(req && result.http_status == 200);
   cJSON *temp = cJSON_GetObjectItemCaseSensitive(req, "temperature");
   assert(cJSON_IsNumber(temp) && temp->valuedouble > 0.19 && temp->valuedouble < 0.21);
   cJSON *mt = cJSON_GetObjectItemCaseSensitive(req, "max_tokens");
   assert(cJSON_IsNumber(mt) && mt->valueint == 512);
   /* disable_thinking => chat_template_kwargs.enable_thinking:false */
   cJSON *ctk = cJSON_GetObjectItemCaseSensitive(req, "chat_template_kwargs");
   assert(ctk);
   cJSON *et = cJSON_GetObjectItemCaseSensitive(ctk, "enable_thinking");
   assert(cJSON_IsBool(et) && !cJSON_IsTrue(et));

   cJSON *rf = cJSON_GetObjectItemCaseSensitive(req, "response_format");
   assert(rf);
   cJSON *rftype = cJSON_GetObjectItemCaseSensitive(rf, "type");
   assert(cJSON_IsString(rftype) && strcmp(rftype->valuestring, "json_schema") == 0);
   cJSON *js = cJSON_GetObjectItemCaseSensitive(rf, "json_schema");
   assert(js);
   cJSON *strict = cJSON_GetObjectItemCaseSensitive(js, "strict");
   assert(cJSON_IsBool(strict) && cJSON_IsTrue(strict));
   cJSON *embedded = cJSON_GetObjectItemCaseSensitive(js, "schema");
   assert(embedded); /* the caller schema is copied in */
   cJSON *etype = cJSON_GetObjectItemCaseSensitive(embedded, "type");
   assert(cJSON_IsString(etype) && strcmp(etype->valuestring, "object") == 0);

   assert(cJSON_IsObject(request.output_format.schema));
   cJSON_Delete(req);
   aimee_request_free(&request);
   provider_completion_free(&result);
   mock_agent_http_reset();
   printf("provider_client: build options + schema ok\n");
}

static void test_parse_success(void)
{
   const char *body =
       "{\"model\":\"granite-3b-instruct\",\"choices\":[{\"finish_reason\":\"stop\","
       "\"message\":{\"role\":\"assistant\",\"content\":\"{\\\"entities\\\":[]}\"}}],"
       "\"usage\":{\"prompt_tokens\":42,\"completion_tokens\":7}}";
   provider_completion_t out;
   int rc = provider_client_parse_openai(body, &out);
   assert(rc == 0);
   assert_text(&out, "{\"entities\":[]}");
   assert(out.ir.usage_in == 42 && out.ir.usage_out == 7);
   assert(strcmp(out.ir.model, "granite-3b-instruct") == 0);
   assert(strcmp(out.ir.raw_stop_reason, "stop") == 0);
   provider_completion_free(&out);
   provider_completion_free(&out); /* idempotent */
   printf("provider_client: parse success ok\n");
}

static void test_owned_response_blocks(void)
{
   cJSON *root =
       cJSON_Parse("{\"model\":\"native-model\",\"choices\":[{\"finish_reason\":\"tool_calls\","
                   "\"message\":{\"role\":\"assistant\",\"reasoning_content\":\"why\",\"content\":["
                   "{\"type\":\"text\",\"text\":\"before\"},{\"type\":\"image_url\",\"image_url\":{"
                   "\"url\":\"https://example.test/image\"}},"
                   "{\"type\":\"text\",\"text\":\"after\"},{\"type\":\"file\",\"file\":{\"file_"
                   "id\":\"file-stable\"}}],\"tool_calls\":[]}}],"
                   "\"usage\":{\"prompt_tokens\":42,\"completion_tokens\":9,\"prompt_tokens_"
                   "details\":{\"cached_tokens\":17},"
                   "\"completion_tokens_details\":{\"reasoning_tokens\":3}}}");
   assert(root);
   cJSON *msg =
       cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(root, "choices"), 0), "message");
   cJSON *calls = cJSON_GetObjectItem(msg, "tool_calls");
   char id[512], payload[8193];
   memset(id, 'i', sizeof id - 1);
   id[sizeof id - 1] = 0;
   memset(payload, 'p', sizeof payload - 1);
   payload[sizeof payload - 1] = 0;
   for (int i = 0; i < 17; i++)
   {
      id[0] = (char)('a' + i);
      cJSON *call = cJSON_CreateObject(), *args = cJSON_CreateObject();
      cJSON_AddStringToObject(call, "id", id);
      cJSON_AddStringToObject(call, "namespace", "mcp__aimee");
      cJSON_AddStringToObject(call, "type", "function");
      cJSON *fn = cJSON_AddObjectToObject(call, "function");
      cJSON_AddStringToObject(fn, "name", "read");
      cJSON_AddStringToObject(args, "payload", payload);
      char *serialized = cJSON_PrintUnformatted(args);
      assert(serialized);
      cJSON_AddStringToObject(fn, "arguments", serialized);
      free(serialized);
      cJSON_Delete(args);
      cJSON_AddItemToArray(calls, call);
   }
   char *body = cJSON_PrintUnformatted(root);
   provider_completion_t out;
   assert(body && !provider_client_parse_openai(body, &out));
   assert(out.ir.n_content == 22);
   assert_text(&out, "beforeafter");
   assert(!strcmp(out.ir.content[0].text, "why"));
   assert(out.ir.content[0].type == AIMEE_BLK_THINKING);
   assert(out.ir.content[1].type == AIMEE_BLK_TEXT && out.ir.content[2].type == AIMEE_BLK_IMAGE);
   assert(out.ir.content[3].type == AIMEE_BLK_TEXT && out.ir.content[4].type == AIMEE_BLK_DOCUMENT);
   assert(out.ir.usage_cache_read == 17 && out.ir.usage_reasoning == 3);
   for (int i = 0; i < 17; i++)
   {
      const aimee_block_t *block = &out.ir.content[5 + i];
      id[0] = (char)('a' + i);
      assert(block->type == AIMEE_BLK_TOOL_USE && !strcmp(block->tool_id, id));
      assert(!strcmp(block->tool_namespace, "mcp__aimee"));
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(block->tool_input, "payload")),
                     payload));
   }
   provider_completion_free(&out);
   free(body);
   cJSON_ReplaceItemInObject(msg, "role", cJSON_CreateString("user"));
   body = cJSON_PrintUnformatted(root);
   assert(body && provider_client_parse_openai(body, &out) < 0);
   provider_completion_free(&out);
   free(body);
   cJSON_Delete(root);
   puts("provider_client: owned ordered media, reasoning and 17 full tool calls ok");
}

static void test_parse_failures(void)
{
   provider_completion_t out;
   /* not JSON */
   assert(provider_client_parse_openai("not json", &out) == -1);
   provider_completion_free(&out);
   /* well-formed but no content */
   assert(provider_client_parse_openai(
              "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{}}]}", &out) == -1);
   provider_completion_free(&out);
   /* empty choices */
   assert(provider_client_parse_openai("{\"choices\":[]}", &out) == -1);
   provider_completion_free(&out);
   /* NULL body */
   assert(provider_client_parse_openai(NULL, &out) == -1);
   provider_completion_free(&out);
   /* out zeroed even on failure */
   assert(out.ir.content == NULL && out.ir.usage_in == 0);
   printf("provider_client: parse failures ok\n");
}

/* Canonical THINKING is diagnostic state, never a substitute authored answer.
 * A reasoning-only response remains available in out.ir but is not a usable
 * textual completion for a curator or other application consumer. */
static void test_refused_text_is_not_answer(void)
{
   provider_completion_t out = {0};
   const char *body = "{\"choices\":[{\"finish_reason\":\"content_filter\","
                      "\"message\":{\"role\":\"assistant\","
                      "\"content\":\"{\\\"synthesis\\\":\\\"partial unapproved data\\\"}\"}}],"
                      "\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":3}}";
   assert(provider_client_parse_openai(body, &out) < 0);
   assert(out.ir.stop_reason == AIMEE_STOP_CONTENT_FILTER);
   assert(out.ir.usage_in == 7 && out.ir.usage_out == 3);
   assert(out.ir.n_content == 1 && out.ir.content[0].type == AIMEE_BLK_TEXT);
   assert(strstr(out.ir.content[0].text, "partial unapproved data"));
   provider_completion_free(&out);
}

static void test_parse_reasoning_shapes(void)
{
   provider_completion_t out;

   /* (1) both present: content wins, reasoning is handed back separately. */
   assert(provider_client_parse_openai(
              "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"the answer\","
              "\"reasoning_content\":\"weighing it up\"}}]}",
              &out) == 0);
   assert_text(&out, "the answer");
   assert(out.ir.content[0].type == AIMEE_BLK_THINKING &&
          !strcmp(out.ir.content[0].text, "weighing it up"));
   assert(out.ir.n_content == 2 && out.ir.content[0].type == AIMEE_BLK_THINKING &&
          out.ir.content[1].type == AIMEE_BLK_TEXT);
   provider_completion_free(&out);

   /* (2) A reasoning-only response cannot become application answer data. */
   assert(provider_client_parse_openai(
              "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":null,"
              "\"reasoning_content\":\"recovered body\"}}]}",
              &out) == -1);
   assert_text(&out, "");
   assert(!strcmp(out.ir.content[0].text, "recovered body"));
   assert(out.ir.n_content == 1 && out.ir.content[0].type == AIMEE_BLK_THINKING);
   provider_completion_free(&out);

   /* (3) content absent entirely (not an explicit null) behaves the same. */
   assert(provider_client_parse_openai("{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{"
                                       "\"reasoning_content\":\"only reasoning\"}}]}",
                                       &out) == -1);
   assert_text(&out, "");
   assert(out.ir.n_content == 1 && out.ir.content[0].type == AIMEE_BLK_THINKING);
   provider_completion_free(&out);

   /* (4) reasoning present but NOT a string: no coercion, no silent fallback.
    * A malformed field must not become the answer. Guarded by the conjunction
    * cJSON_IsString(reasoning) && reasoning->valuestring && ...[0] — belt and
    * braces, so dropping either half alone is still safe; drop both and this case
    * segfaults on strdup(NULL), which is what it is here to catch. */
   assert(provider_client_parse_openai(
              "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":null,"
              "\"reasoning_content\":{\"nested\":1}}}]}",
              &out) == -1);
   provider_completion_free(&out);

   /* (5) neither: still the honest failure it always was. */
   assert(provider_client_parse_openai(
              "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":null}}]}",
              &out) == -1);
   provider_completion_free(&out);

   /* (6) a <think> preamble in content is split, not deleted: the answer is the
    * tail and the reasoning is preserved on the side. */
   assert(provider_client_parse_openai("{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{"
                                       "\"content\":\"<think>hmm</think>real answer\"}}]}",
                                       &out) == 0);
   assert_text(&out, "real answer");
   provider_completion_free(&out);

   printf("provider_client: reasoning shapes ok\n");
}

static aimee_request_t complete_request(void)
{
   aimee_request_t request = {0};
   request.messages = calloc(1, sizeof *request.messages);
   assert(request.messages);
   request.n_messages = 1;
   request.messages[0].role = strdup("user");
   request.messages[0].blocks = calloc(1, sizeof *request.messages[0].blocks);
   assert(request.messages[0].role && request.messages[0].blocks);
   request.messages[0].n_blocks = 1;
   request.messages[0].blocks[0].type = AIMEE_BLK_TEXT;
   request.messages[0].blocks[0].text = strdup("extract entities");
   assert(request.messages[0].blocks[0].text && !aimee_ir_request_admit(&request));
   return request;
}

static void test_complete_arg_guards(void)
{
   char err[128];
   provider_completion_t out;
   aimee_request_t request = complete_request();
   provider_def_t def = {.base_url = "http://h/v1", .wire = PROVIDER_WIRE_OPENAI_CHAT};

   /* missing base_url */
   provider_def_t no_url = def;
   no_url.base_url = "";
   assert(provider_client_complete_ir(&no_url, &request, &out, err, sizeof(err)) == -1);
   assert(err[0] != '\0');

   /* unsupported wire */
   provider_def_t bad_wire = def;
   bad_wire.wire = (provider_wire_t)99;
   assert(provider_client_complete_ir(&bad_wire, &request, &out, err, sizeof(err)) == -1);

   aimee_request_free(&request);
   printf("provider_client: complete arg guards ok\n");
}

/* --- complete() over the mocked transport: URL/auth/retry/parse --- */

static char g_seen_url[1024];
static char g_seen_auth[1024];
static int g_post_calls;

static int ok_handler(const char *url, const char *auth_header, const char *body,
                      char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)body;
   (void)timeout_ms;
   (void)extra_headers;
   g_post_calls++;
   snprintf(g_seen_url, sizeof(g_seen_url), "%s", url ? url : "");
   snprintf(g_seen_auth, sizeof(g_seen_auth), "%s", auth_header ? auth_header : "");
   if (response_buf)
      *response_buf = strdup("{\"model\":\"m\",\"choices\":[{\"finish_reason\":\"stop\","
                             "\"message\":{\"content\":\"hi\"}}],"
                             "\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":1}}");
   return 200;
}

/* 503 on the first call, 200 on the second — exercises the retry loop. */
static int flaky_handler(const char *url, const char *auth_header, const char *body,
                         char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)timeout_ms;
   (void)extra_headers;
   g_post_calls++;
   if (g_post_calls == 1)
   {
      if (response_buf)
         *response_buf = NULL; /* explicit: no body on a transient error */
      return 503;              /* retryable */
   }
   if (response_buf)
      *response_buf =
          strdup("{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"ok\"}}]}");
   return 200;
}

static int err_handler(const char *url, const char *auth_header, const char *body,
                       char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)timeout_ms;
   (void)extra_headers;
   (void)response_buf;
   g_post_calls++;
   return 401; /* non-retryable */
}

static void test_complete_success(void)
{
   mock_agent_http_reset();
   g_post_calls = 0;
   mock_agent_http_set_post_handler(ok_handler);

   provider_def_t def = {.base_url = "http://h:8080/v1",
                         .model = "m",
                         .api_key = "secret",
                         .wire = PROVIDER_WIRE_OPENAI_CHAT,
                         .temperature = -1.0};
   aimee_request_t request = complete_request();
   provider_completion_t out;
   char err[128];
   int rc = provider_client_complete_ir(&def, &request, &out, err, sizeof(err));
   assert(rc == 0);
   assert(g_post_calls == 1);
   /* URL assembled with exactly one slash; auth carries the bearer. */
   assert(strcmp(g_seen_url, "http://h:8080/v1/chat/completions") == 0);
   assert(strcmp(g_seen_auth, "Authorization: Bearer secret") == 0);
   assert_text(&out, "hi");
   assert(out.ir.usage_in == 3 && out.ir.usage_out == 1);
   assert(out.http_status == 200);
   provider_completion_free(&out);
   aimee_request_free(&request);
   mock_agent_http_reset();
   printf("provider_client: complete success (url/auth/parse) ok\n");
}

static void test_complete_trailing_slash(void)
{
   mock_agent_http_reset();
   g_post_calls = 0;
   mock_agent_http_set_post_handler(ok_handler);
   /* a base_url WITH a trailing slash must not double up. */
   provider_def_t def = {
       .base_url = "http://h/v1/", .wire = PROVIDER_WIRE_OPENAI_CHAT, .temperature = -1.0};
   aimee_request_t request = complete_request();
   provider_completion_t out;
   assert(provider_client_complete_ir(&def, &request, &out, NULL, 0) == 0);
   assert(strcmp(g_seen_url, "http://h/v1/chat/completions") == 0);
   /* keyless: no Authorization header. */
   assert(g_seen_auth[0] == '\0');
   provider_completion_free(&out);
   aimee_request_free(&request);
   mock_agent_http_reset();
   printf("provider_client: complete trailing-slash + keyless ok\n");
}

static void test_complete_retry_then_ok(void)
{
   mock_agent_http_reset();
   g_post_calls = 0;
   mock_agent_http_set_post_handler(flaky_handler);
   provider_def_t def = {.base_url = "http://h/v1",
                         .wire = PROVIDER_WIRE_OPENAI_CHAT,
                         .temperature = -1.0,
                         .max_attempts = 3};
   aimee_request_t request = complete_request();
   provider_completion_t out;
   int rc = provider_client_complete_ir(&def, &request, &out, NULL, 0);
   assert(rc == 0);
   assert(g_post_calls == 2); /* retried once after the 503 */
   assert_text(&out, "ok");
   provider_completion_free(&out);
   aimee_request_free(&request);
   mock_agent_http_reset();
   printf("provider_client: complete retry-then-ok ok\n");
}

static void test_complete_non_retryable(void)
{
   mock_agent_http_reset();
   g_post_calls = 0;
   mock_agent_http_set_post_handler(err_handler);
   provider_def_t def = {.base_url = "http://h/v1",
                         .wire = PROVIDER_WIRE_OPENAI_CHAT,
                         .temperature = -1.0,
                         .max_attempts = 3};
   aimee_request_t request = complete_request();
   provider_completion_t out;
   char err[128];
   int rc = provider_client_complete_ir(&def, &request, &out, err, sizeof(err));
   assert(rc == -1);
   assert(g_post_calls == 1); /* 401 is not retried */
   assert(out.http_status == 401);
   assert(out.ir.content == NULL);
   assert(err[0] != '\0');
   provider_completion_free(&out);
   aimee_request_free(&request);
   mock_agent_http_reset();
   printf("provider_client: complete non-retryable (no retry) ok\n");
}

static void test_complete_truncation_guards(void)
{
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(ok_handler);
   aimee_request_t request = complete_request();
   provider_completion_t out;
   char err[128];

   /* base_url longer than the URL buffer -> truncation error, no transport call. */
   char big[4096];
   memset(big, 'x', sizeof(big) - 1);
   big[sizeof(big) - 1] = '\0';
   provider_def_t d1 = {.base_url = big, .wire = PROVIDER_WIRE_OPENAI_CHAT, .temperature = -1.0};
   g_post_calls = 0;
   assert(provider_client_complete_ir(&d1, &request, &out, err, sizeof(err)) == -1);
   assert(g_post_calls == 0 && err[0] != '\0');

   /* api_key longer than the auth buffer -> truncation error, no transport call. */
   char bigkey[2048];
   memset(bigkey, 'k', sizeof(bigkey) - 1);
   bigkey[sizeof(bigkey) - 1] = '\0';
   provider_def_t d2 = {.base_url = "http://h/v1",
                        .api_key = bigkey,
                        .wire = PROVIDER_WIRE_OPENAI_CHAT,
                        .temperature = -1.0};
   g_post_calls = 0;
   assert(provider_client_complete_ir(&d2, &request, &out, err, sizeof(err)) == -1);
   assert(g_post_calls == 0 && err[0] != '\0');

   aimee_request_free(&request);
   mock_agent_http_reset();
   printf("provider_client: complete truncation guards ok\n");
}

int main(void)
{
   test_build_basic();
   test_build_options_and_schema();
   test_parse_success();
   test_owned_response_blocks();
   test_parse_failures();
   test_parse_reasoning_shapes();
   test_refused_text_is_not_answer();
   test_complete_arg_guards();
   test_complete_success();
   test_complete_trailing_slash();
   test_complete_retry_then_ok();
   test_complete_non_retryable();
   test_complete_truncation_guards();
   printf("provider_client: all tests passed\n");
   return 0;
}
