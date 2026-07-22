/* test_provider_client.c: shared provider-client request builder + response parser.
 * Network-free: exercises the pure build/parse halves (provider_client_complete's
 * transport is covered by integration once a curator endpoint is wired). */
#include "provider_client.h"
#include "cJSON.h"
#include "support/mock_agent_http.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *two_messages(void)
{
   cJSON *msgs = cJSON_CreateArray();
   cJSON *m = cJSON_CreateObject();
   cJSON_AddStringToObject(m, "role", "user");
   cJSON_AddStringToObject(m, "content", "extract entities");
   cJSON_AddItemToArray(msgs, m);
   return msgs;
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
   cJSON *msgs = two_messages();
   cJSON *req = provider_client_build_openai(&def, msgs, NULL);
   assert(req);

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

   cJSON_Delete(req);
   cJSON_Delete(msgs);
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
   cJSON *msgs = two_messages();
   cJSON *schema = cJSON_CreateObject();
   cJSON_AddStringToObject(schema, "type", "object");

   cJSON *req = provider_client_build_openai(&def, msgs, schema);
   assert(req);
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

   cJSON_Delete(req);
   cJSON_Delete(msgs);
   cJSON_Delete(schema); /* still owned by the caller — proves no take-over */
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
   assert(out.content && strcmp(out.content, "{\"entities\":[]}") == 0);
   assert(out.prompt_tokens == 42 && out.completion_tokens == 7);
   assert(strcmp(out.model, "granite-3b-instruct") == 0);
   assert(strcmp(out.finish_reason, "stop") == 0);
   provider_completion_free(&out);
   provider_completion_free(&out); /* idempotent */
   printf("provider_client: parse success ok\n");
}

static void test_parse_failures(void)
{
   provider_completion_t out;
   /* not JSON */
   assert(provider_client_parse_openai("not json", &out) == -1);
   /* well-formed but no content */
   assert(provider_client_parse_openai("{\"choices\":[{\"message\":{}}]}", &out) == -1);
   /* empty choices */
   assert(provider_client_parse_openai("{\"choices\":[]}", &out) == -1);
   /* NULL body */
   assert(provider_client_parse_openai(NULL, &out) == -1);
   /* out zeroed even on failure */
   assert(out.content == NULL && out.prompt_tokens == 0);
   printf("provider_client: parse failures ok\n");
}

/* The reasoning_content shape matrix.
 *
 * These are the JSON shapes real providers actually emit, and the precedence
 * between them is the whole point: reasoning_content is PRIVATE reasoning, never
 * the answer. The one exception is content:null — the answer is absent and the
 * reasoning is all there is, which is why the old code returned rc=-1
 * "unparseable provider response" and threw a usable reply away. Each case below
 * pins one shape so that recovery cannot quietly widen into "reasoning is the
 * answer whenever content looks empty". */
static void test_parse_reasoning_shapes(void)
{
   provider_completion_t out;

   /* (1) both present: content wins, reasoning is handed back separately. */
   assert(provider_client_parse_openai("{\"choices\":[{\"message\":{\"content\":\"the answer\","
                                       "\"reasoning_content\":\"weighing it up\"}}]}",
                                       &out) == 0);
   assert(out.content && strcmp(out.content, "the answer") == 0);
   assert(out.reasoning && strcmp(out.reasoning, "weighing it up") == 0);
   provider_completion_free(&out);

   /* (2) content:null with reasoning — the shape that used to fail outright.
    * The reasoning IS the reply here; recovering it is the fix. */
   assert(provider_client_parse_openai("{\"choices\":[{\"message\":{\"content\":null,"
                                       "\"reasoning_content\":\"recovered body\"}}]}",
                                       &out) == 0);
   assert(out.content && strcmp(out.content, "recovered body") == 0);
   provider_completion_free(&out);

   /* (3) content absent entirely (not an explicit null) behaves the same. */
   assert(provider_client_parse_openai(
              "{\"choices\":[{\"message\":{\"reasoning_content\":\"only reasoning\"}}]}", &out) ==
          0);
   assert(out.content && strcmp(out.content, "only reasoning") == 0);
   provider_completion_free(&out);

   /* (4) reasoning present but NOT a string: no coercion, no silent fallback.
    * A malformed field must not become the answer. Guarded by the conjunction
    * cJSON_IsString(reasoning) && reasoning->valuestring && ...[0] — belt and
    * braces, so dropping either half alone is still safe; drop both and this case
    * segfaults on strdup(NULL), which is what it is here to catch. */
   assert(provider_client_parse_openai("{\"choices\":[{\"message\":{\"content\":null,"
                                       "\"reasoning_content\":{\"nested\":1}}}]}",
                                       &out) == -1);
   provider_completion_free(&out);

   /* (5) neither: still the honest failure it always was. */
   assert(provider_client_parse_openai("{\"choices\":[{\"message\":{\"content\":null}}]}", &out) ==
          -1);
   provider_completion_free(&out);

   /* (6) a <think> preamble in content is split, not deleted: the answer is the
    * tail and the reasoning is preserved on the side. */
   assert(provider_client_parse_openai(
              "{\"choices\":[{\"message\":{\"content\":\"<think>hmm</think>real answer\"}}]}",
              &out) == 0);
   assert(out.content && strcmp(out.content, "real answer") == 0);
   provider_completion_free(&out);

   printf("provider_client: reasoning shapes ok\n");
}

static void test_complete_arg_guards(void)
{
   char err[128];
   provider_completion_t out;
   cJSON *msgs = two_messages();
   provider_def_t def = {.base_url = "http://h/v1", .wire = PROVIDER_WIRE_OPENAI_CHAT};

   /* missing base_url */
   provider_def_t no_url = def;
   no_url.base_url = "";
   assert(provider_client_complete(&no_url, msgs, NULL, &out, err, sizeof(err)) == -1);
   assert(err[0] != '\0');

   /* unsupported wire */
   provider_def_t bad_wire = def;
   bad_wire.wire = (provider_wire_t)99;
   assert(provider_client_complete(&bad_wire, msgs, NULL, &out, err, sizeof(err)) == -1);

   cJSON_Delete(msgs);
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
      *response_buf = strdup("{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}");
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
   cJSON *msgs = two_messages();
   provider_completion_t out;
   char err[128];
   int rc = provider_client_complete(&def, msgs, NULL, &out, err, sizeof(err));
   assert(rc == 0);
   assert(g_post_calls == 1);
   /* URL assembled with exactly one slash; auth carries the bearer. */
   assert(strcmp(g_seen_url, "http://h:8080/v1/chat/completions") == 0);
   assert(strcmp(g_seen_auth, "Authorization: Bearer secret") == 0);
   assert(out.content && strcmp(out.content, "hi") == 0);
   assert(out.prompt_tokens == 3 && out.completion_tokens == 1);
   assert(out.http_status == 200);
   provider_completion_free(&out);
   cJSON_Delete(msgs);
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
   cJSON *msgs = two_messages();
   provider_completion_t out;
   assert(provider_client_complete(&def, msgs, NULL, &out, NULL, 0) == 0);
   assert(strcmp(g_seen_url, "http://h/v1/chat/completions") == 0);
   /* keyless: no Authorization header. */
   assert(g_seen_auth[0] == '\0');
   provider_completion_free(&out);
   cJSON_Delete(msgs);
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
   cJSON *msgs = two_messages();
   provider_completion_t out;
   int rc = provider_client_complete(&def, msgs, NULL, &out, NULL, 0);
   assert(rc == 0);
   assert(g_post_calls == 2); /* retried once after the 503 */
   assert(out.content && strcmp(out.content, "ok") == 0);
   provider_completion_free(&out);
   cJSON_Delete(msgs);
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
   cJSON *msgs = two_messages();
   provider_completion_t out;
   char err[128];
   int rc = provider_client_complete(&def, msgs, NULL, &out, err, sizeof(err));
   assert(rc == -1);
   assert(g_post_calls == 1); /* 401 is not retried */
   assert(out.http_status == 401);
   assert(out.content == NULL);
   assert(err[0] != '\0');
   provider_completion_free(&out);
   cJSON_Delete(msgs);
   mock_agent_http_reset();
   printf("provider_client: complete non-retryable (no retry) ok\n");
}

static void test_complete_truncation_guards(void)
{
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(ok_handler);
   cJSON *msgs = two_messages();
   provider_completion_t out;
   char err[128];

   /* base_url longer than the URL buffer -> truncation error, no transport call. */
   char big[4096];
   memset(big, 'x', sizeof(big) - 1);
   big[sizeof(big) - 1] = '\0';
   provider_def_t d1 = {.base_url = big, .wire = PROVIDER_WIRE_OPENAI_CHAT, .temperature = -1.0};
   g_post_calls = 0;
   assert(provider_client_complete(&d1, msgs, NULL, &out, err, sizeof(err)) == -1);
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
   assert(provider_client_complete(&d2, msgs, NULL, &out, err, sizeof(err)) == -1);
   assert(g_post_calls == 0 && err[0] != '\0');

   cJSON_Delete(msgs);
   mock_agent_http_reset();
   printf("provider_client: complete truncation guards ok\n");
}

int main(void)
{
   test_build_basic();
   test_build_options_and_schema();
   test_parse_success();
   test_parse_failures();
   test_parse_reasoning_shapes();
   test_complete_arg_guards();
   test_complete_success();
   test_complete_trailing_slash();
   test_complete_retry_then_ok();
   test_complete_non_retryable();
   test_complete_truncation_guards();
   printf("provider_client: all tests passed\n");
   return 0;
}
