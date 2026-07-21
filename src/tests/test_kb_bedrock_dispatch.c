#include "kb/kb_bedrock_egress.h"
#include "kb/http/kb_http_client.h"
#include "tests/support/aws_eventstream_fixture.h"
#include "cJSON.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct
{
   int status;
   const char *content_type;
   const unsigned char *body;
   size_t body_len;
   size_t fragment;
   kb_http_result_t transport_result;
   kb_http_result_t post_body_result;
   int calls, body_calls;
} dispatch_mock_t;

static dispatch_mock_t dispatch_mock;

kb_http_result_t kb_http_tls_exchange(const kb_http_request_t *request,
                                     kb_http_response_t *response,
                                     kb_http_headers_fn headers_cb, kb_http_body_fn body_cb,
                                     void *context)
{
   dispatch_mock.calls++;
   memset(response, 0, sizeof(*response));
   assert(request && request->authority && strstr(request->authority, "bedrock-runtime.") ==
                                               request->authority);
   assert(strcmp(request->method, "POST") == 0 && request->target[0] == '/');
   assert(request->response_body_max == KB_BEDROCK_BODY_MAX);
   int hosts = 0;
   for (size_t i = 0; i < request->header_count; i++)
      if (strcasecmp(request->headers[i].name, "host") == 0)
      {
         hosts++;
         assert(strcmp(request->headers[i].value, request->authority) == 0);
      }
   assert(hosts == 1);
   if (dispatch_mock.transport_result != KB_HTTP_OK)
      return dispatch_mock.transport_result;
   kb_http_response_t metadata = {.status = dispatch_mock.status,
                                  .framing = KB_HTTP_FRAMING_CONTENT_LENGTH,
                                  .content_length = dispatch_mock.body_len};
   if (dispatch_mock.content_type)
      snprintf(metadata.content_type, sizeof(metadata.content_type), "%s",
               dispatch_mock.content_type);
   kb_http_gate_t gate = headers_cb(&metadata, context);
   if (gate == KB_HTTP_GATE_ABORT)
      return KB_HTTP_CALLBACK_ABORT;
   if (gate == KB_HTTP_GATE_DELIVER)
   {
      size_t fragment = dispatch_mock.fragment ? dispatch_mock.fragment : dispatch_mock.body_len;
      for (size_t at = 0; at < dispatch_mock.body_len;)
      {
         size_t take = dispatch_mock.body_len - at;
         if (take > fragment)
            take = fragment;
         dispatch_mock.body_calls++;
         if (body_cb(dispatch_mock.body + at, take, context) == KB_HTTP_BODY_CALLER_ABORT)
            return KB_HTTP_CALLBACK_ABORT;
         at += take;
      }
   }
   if (dispatch_mock.post_body_result != KB_HTTP_OK)
      return dispatch_mock.post_body_result;
   *response = metadata;
   return KB_HTTP_OK;
}

static void assert_canonical_uri(const kb_bedrock_wire_request_t *request)
{
   const char *first = strchr(request->sig.canonical_request, '\n');
   assert(first != NULL);
   const char *uri = first + 1;
   const char *end = strchr(uri, '\n');
   assert(end != NULL);
   assert((size_t)(end - uri) == strlen(request->encoded_path));
   assert(memcmp(uri, request->encoded_path, (size_t)(end - uri)) == 0);
}

static test_aws_es_header_t string_header(const char *name, const char *value)
{
   test_aws_es_header_t h = {
       .name = name, .value_type = AWS_ES_HDR_STRING, .value = value, .value_len = strlen(value)};
   return h;
}

static db2_bedrock_target_t target(const char *partition, const char *region, const char *id)
{
   db2_bedrock_target_t t;
   memset(&t, 0, sizeof(t));
   snprintf(t.model_id, sizeof(t.model_id), "%s", id);
   snprintf(t.bedrock_api, sizeof(t.bedrock_api), "converse");
   snprintf(t.model_family, sizeof(t.model_family), "anthropic");
   snprintf(t.target_type, sizeof(t.target_type), "foundation");
   snprintf(t.partition, sizeof(t.partition), "%s", partition);
   snprintf(t.invoke_region, sizeof(t.invoke_region), "%s", region);
   snprintf(t.regions[0], sizeof(t.regions[0]), "%s", region);
   t.n_regions = 1;
   return t;
}

static kb_bedrock_credentials_t credentials(void)
{
   kb_bedrock_credentials_t c = {.access_key_id = "AKIDEXAMPLE",
                                 .secret_access_key = "secret",
                                 .session_token = "token",
                                 .amz_date = "20260101T000000Z",
                                 .date = "20260101"};
   return c;
}

static void request_tests(void)
{
   const char *partitions[] = {"aws", "aws-us-gov", "aws-cn"};
   const char *regions[] = {"us-west-2", "us-gov-west-1", "cn-north-1"};
   aimee_request_t ir;
   memset(&ir, 0, sizeof(ir));
   aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = "hello"};
   aimee_message_t message = {.role = "user", .blocks = &block, .n_blocks = 1};
   ir.messages = &message;
   ir.n_messages = 1;
   kb_bedrock_credentials_t c = credentials();
   for (int i = 0; i < 3; i++)
   {
      db2_bedrock_target_t t = target(partitions[i], regions[i], "model/a:b");
      if (i == 1)
         snprintf(t.model_id, sizeof(t.model_id), "model:id");
      kb_bedrock_wire_request_t q;
      kb_bedrock_wire_request_init(&q);
      assert(kb_bedrock_wire_request_build(&t, &ir, i & 1, &c, &q) == KB_BEDROCK_OK);
      assert(strstr(q.host, i == 2 ? "amazonaws.com.cn" : "amazonaws.com") != NULL);
      assert(strstr(q.encoded_path, "%3A") != NULL);
      assert_canonical_uri(&q);
      assert(strstr(q.sig.signed_headers, "x-amz-content-sha256") != NULL);
      kb_bedrock_header_t h[KB_BEDROCK_MAX_HEADERS];
      size_t n = 0;
      assert(kb_bedrock_wire_request_headers(&q, h, 6, &n) == KB_BEDROCK_OK && n == 6);
      assert(strcmp(h[3].value, q.payload_hash) == 0);
      kb_bedrock_wire_request_clear(&q);
      assert(q.body == NULL && q.body_len == 0 && q.host[0] == 0);
   }

   db2_bedrock_target_t t = target("aws", "us-east-1", "model");
   kb_bedrock_wire_request_t q;
   kb_bedrock_wire_request_init(&q);
   char invalid_text[] = {'x', (char)0xc0, (char)0xaf, 0};
   block.text = invalid_text;
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_ARGUMENT);
   char truncated_text[] = {'x', (char)0xe2, (char)0x82, 0};
   block.text = truncated_text;
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_ARGUMENT);
   char surrogate_text[] = {'x', (char)0xed, (char)0xa0, (char)0x80, 0};
   block.text = surrogate_text;
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_ARGUMENT);
   block.text = "\xf0\x9f\x98\x80";
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_OK);
   kb_bedrock_wire_request_clear(&q);
   block.text = "hello";
   cJSON *schema = cJSON_CreateObject();
   cJSON *bad_choice_type = cJSON_CreateString("auto");
   cJSON *bad_choice = cJSON_CreateObject();
   assert(schema && bad_choice_type && bad_choice);
   free(bad_choice_type->valuestring);
   bad_choice_type->valuestring = NULL;
   cJSON_AddItemToObject(bad_choice, "type", bad_choice_type);
   aimee_tool_t one_tool = {.name = "tool", .schema = schema};
   ir.tools = &one_tool;
   ir.n_tools = 1;
   ir.tool_choice = bad_choice;
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_ARGUMENT);
   cJSON *bad_key_choice = cJSON_Parse("{\"type\":\"auto\"}");
   assert(bad_key_choice && bad_key_choice->child);
   free(bad_key_choice->child->string);
   bad_key_choice->child->string = NULL;
   ir.tool_choice = bad_key_choice;
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_ARGUMENT);
   ir.tool_choice = NULL;
   assert(cJSON_AddNumberToObject(schema, "x", 1) != NULL && schema->child);
   free(schema->child->string);
   schema->child->string = NULL;
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_ARGUMENT);
   ir.tools = NULL;
   ir.n_tools = 0;
   ir.tool_choice = NULL;
   cJSON_Delete(bad_choice);
   cJSON_Delete(bad_key_choice);
   cJSON_Delete(schema);
   ir.has_top_k = 1;
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_ARGUMENT);
   t.endpoint[0] = 'x';
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_TARGET);

   memset(&ir, 0, sizeof(ir));
   t = target("aws", "us-east-1",
              "arn:aws:bedrock:us-east-1::foundation-model/anthropic.claude-v2");
   block = (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = "line one\nline two"};
   message = (aimee_message_t){.role = "user", .blocks = &block, .n_blocks = 1};
   ir.messages = &message;
   ir.n_messages = 1;
   c.session_token = NULL;
   assert(kb_bedrock_wire_request_build(&t, &ir, 1, &c, &q) == KB_BEDROCK_OK);
   assert(strstr(q.encoded_path, "arn%3Aaws%3Abedrock%3Aus-east-1%3A%3A") != NULL);
   assert(strstr(q.encoded_path, "foundation-model/anthropic.claude-v2/converse-stream") != NULL);
   assert_canonical_uri(&q);
   assert(strstr(q.body, "line one\\nline two") != NULL);
   kb_bedrock_header_t headers[KB_BEDROCK_MAX_HEADERS];
   size_t n_headers = 0;
   assert(kb_bedrock_wire_request_headers(&q, headers, KB_BEDROCK_MAX_HEADERS, &n_headers) ==
          KB_BEDROCK_OK);
   assert(n_headers == 5);
   kb_bedrock_credentials_t stale_failure = c;
   stale_failure.date = "20260102";
   assert(kb_bedrock_wire_request_build(&t, &ir, 1, &stale_failure, &q) ==
          KB_BEDROCK_INVALID_ARGUMENT);
   assert(q.body == NULL && q.body_len == 0 && q.host[0] == 0 && q.sig.authorization[0] == 0);

   c = credentials();
   c.date = "20260102";
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_ARGUMENT);
   c = credentials();
   snprintf(t.model_family, sizeof(t.model_family), "unknown-family");
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_TARGET);

   t = target("aws", "us-east-1", "model");
   char unterminated_amz[17];
   memset(unterminated_amz, '1', sizeof(unterminated_amz));
   c = credentials();
   c.amz_date = unterminated_amz;
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_ARGUMENT);
   c = credentials();
   memset(t.bedrock_api, 'x', sizeof(t.bedrock_api));
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_TARGET);
   t = target("aws", "us-east-1", "model");
   memset(t.regions[0], 'x', sizeof(t.regions[0]));
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_TARGET);
   t = target("aws-cn", "us-west-2", "model");
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_TARGET);
   t = target("aws-us-gov", "us-west-2", "model");
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_TARGET);
   t = target("aws", "cn-north-1", "model");
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_TARGET);

   t = target("aws", "us-east-1", "model");
   message.n_blocks = INT_MAX;
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_ARGUMENT);
   message.n_blocks = 1;
   block = (aimee_block_t){.type = AIMEE_BLK_TOOL_USE,
                           .tool_id = "",
                           .tool_name = "tool",
                           .tool_input = cJSON_CreateObject()};
   assert(block.tool_input != NULL);
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_ARGUMENT);
   cJSON_Delete(block.tool_input);
   block.tool_id = "id";
   block.tool_input = cJSON_CreateRaw("{bad-json");
   assert(block.tool_input != NULL);
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_ARGUMENT);
   cJSON_Delete(block.tool_input);
   block.tool_input = cJSON_CreateObject();
   assert(block.tool_input != NULL);
   assert(cJSON_AddNumberToObject(block.tool_input, "value", NAN) != NULL);
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_ARGUMENT);
   cJSON_Delete(block.tool_input);
   block = (aimee_block_t){
       .type = AIMEE_BLK_IMAGE, .media_type = "image/png", .media_ref = "not base64"};
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_ARGUMENT);
   block.media_ref = "AB==";
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_ARGUMENT);
   block.media_ref = "AA==";
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_OK);
   kb_bedrock_wire_request_clear(&q);
   snprintf(t.model_id, sizeof(t.model_id), "model/");
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_TARGET);
   snprintf(t.model_id, sizeof(t.model_id), "model");
   memset(&ir, 0, sizeof(ir));
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_ARGUMENT);
   ir.messages = &message;
   ir.n_messages = 1;
   message.n_blocks = 0;
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_ARGUMENT);
   message.n_blocks = 1;
   ir.tool_choice = cJSON_Parse("{\"type\":\"auto\"}");
   assert(ir.tool_choice != NULL);
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_ARGUMENT);
   cJSON_Delete(ir.tool_choice);

}

static void response_tests(void)
{
   static const unsigned char good[] =
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":[{\"text\":\"ok\"}]}},"
       "\"stopReason\":\"end_turn\",\"usage\":{\"inputTokens\":1,\"outputTokens\":2}}";
   aimee_response_t r;
   kb_bedrock_response_init(&r);
   assert(kb_bedrock_nonstream_parse(good, sizeof(good) - 1, &r) == KB_BEDROCK_OK);
   assert(r.n_content == 1 && strcmp(r.content[0].text, "ok") == 0);
   unsigned char bad[] = "{}x";
   assert(kb_bedrock_nonstream_parse(bad, sizeof(bad) - 1, &r) == KB_BEDROCK_MALFORMED_RESPONSE);
   assert(r.raw == NULL && r.content == NULL);
   assert(kb_bedrock_nonstream_parse(NULL, 0, &r) == KB_BEDROCK_MALFORMED_RESPONSE);

   static const unsigned char trailing_space[] =
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":[{\"text\":\"ok\"}]}},"
       "\"stopReason\":\"end_turn\",\"usage\":{\"inputTokens\":0,\"outputTokens\":0}} \r\n\t";
   assert(kb_bedrock_nonstream_parse(trailing_space, sizeof(trailing_space) - 1, &r) ==
          KB_BEDROCK_OK);
   aimee_response_free(&r);

   static const unsigned char not_object[] = "[]";
   static const unsigned char duplicate_key[] =
       "{\"output\":{},\"output\":{},\"stopReason\":\"end_turn\","
       "\"usage\":{\"inputTokens\":0,\"outputTokens\":0}}";
   static const unsigned char negative_usage[] =
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":[]}},"
       "\"stopReason\":\"end_turn\",\"usage\":{\"inputTokens\":-1,\"outputTokens\":0}}";
   static const unsigned char fractional_usage[] =
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":[]}},"
       "\"stopReason\":\"end_turn\",\"usage\":{\"inputTokens\":1.5,\"outputTokens\":0}}";
   static const unsigned char inexact_usage[] =
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":[]}},"
       "\"stopReason\":\"end_turn\",\"usage\":{\"inputTokens\":9007199254740992,"
       "\"outputTokens\":0}}";
   static const unsigned char empty_content[] =
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":[]}},"
       "\"stopReason\":\"end_turn\",\"usage\":{\"inputTokens\":0,\"outputTokens\":0}}";
   static const unsigned char empty_text[] =
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":[{\"text\":\"\"}]}},"
       "\"stopReason\":\"end_turn\",\"usage\":{\"inputTokens\":0,\"outputTokens\":0}}";
   static const unsigned char ambiguous_content[] =
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":["
       "{\"text\":\"x\",\"toolUse\":{}}]}},\"stopReason\":\"end_turn\","
       "\"usage\":{\"inputTokens\":0,\"outputTokens\":0}}";
   static const unsigned char extra_union_member[] =
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":["
       "{\"text\":\"x\",\"future\":{}}]}},\"stopReason\":\"end_turn\","
       "\"usage\":{\"inputTokens\":0,\"outputTokens\":0}}";
   static const unsigned char nul_escape[] =
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":["
       "{\"text\":\"x\\u0000y\"}]}},\"stopReason\":\"end_turn\","
       "\"usage\":{\"inputTokens\":0,\"outputTokens\":0}}";
   static const unsigned char lone_high_surrogate[] =
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":["
       "{\"text\":\"\\uD800\"}]}},\"stopReason\":\"end_turn\","
       "\"usage\":{\"inputTokens\":0,\"outputTokens\":0}}";
   static const unsigned char lone_low_surrogate[] =
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":["
       "{\"text\":\"\\uDC00\"}]}},\"stopReason\":\"end_turn\","
       "\"usage\":{\"inputTokens\":0,\"outputTokens\":0}}";
   const unsigned char *invalid[] = {not_object,         duplicate_key,       negative_usage,
                                     fractional_usage,   inexact_usage,       ambiguous_content,
                                     extra_union_member, nul_escape,          empty_content,
                                     empty_text,         lone_high_surrogate, lone_low_surrogate};
   const size_t invalid_len[] = {sizeof(not_object) - 1,          sizeof(duplicate_key) - 1,
                                 sizeof(negative_usage) - 1,      sizeof(fractional_usage) - 1,
                                 sizeof(inexact_usage) - 1,       sizeof(ambiguous_content) - 1,
                                 sizeof(extra_union_member) - 1,  sizeof(nul_escape) - 1,
                                 sizeof(empty_content) - 1,       sizeof(empty_text) - 1,
                                 sizeof(lone_high_surrogate) - 1, sizeof(lone_low_surrogate) - 1};
   for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
   {
      assert(kb_bedrock_nonstream_parse(invalid[i], invalid_len[i], &r) ==
             KB_BEDROCK_MALFORMED_RESPONSE);
      assert(r.raw == NULL && r.content == NULL && r.role == NULL);
   }

   unsigned char embedded_nul[sizeof(good)];
   memcpy(embedded_nul, good, sizeof(good));
   embedded_nul[12] = 0;
   assert(kb_bedrock_nonstream_parse(embedded_nul, sizeof(good) - 1, &r) ==
          KB_BEDROCK_MALFORMED_RESPONSE);

   static const unsigned char literal_nul_escape[] =
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":["
       "{\"text\":\"x\\\\u0000y\"}]}},\"stopReason\":\"end_turn\","
       "\"usage\":{\"inputTokens\":0,\"outputTokens\":0}}";
   assert(kb_bedrock_nonstream_parse(literal_nul_escape, sizeof(literal_nul_escape) - 1, &r) ==
          KB_BEDROCK_OK);
   assert(r.n_content == 1 && strcmp(r.content[0].text, "x\\u0000y") == 0);
   aimee_response_free(&r);

   static const unsigned char surrogate_pair[] =
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":["
       "{\"text\":\"\\uD83D\\uDE00\"}]}},\"stopReason\":\"end_turn\","
       "\"usage\":{\"inputTokens\":0,\"outputTokens\":0}}";
   assert(kb_bedrock_nonstream_parse(surrogate_pair, sizeof(surrogate_pair) - 1, &r) ==
          KB_BEDROCK_OK);
   aimee_response_free(&r);

   static const unsigned char invalid_utf8[] =
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":[{\"text\":\"\xc0\xaf\"}]}},"
       "\"stopReason\":\"end_turn\",\"usage\":{\"inputTokens\":0,\"outputTokens\":0}}";
   assert(kb_bedrock_nonstream_parse(invalid_utf8, sizeof(invalid_utf8) - 1, &r) ==
          KB_BEDROCK_MALFORMED_RESPONSE);
   unsigned char encoded_surrogate[sizeof(invalid_utf8) + 1];
   memcpy(encoded_surrogate, invalid_utf8, sizeof(invalid_utf8));
   unsigned char *bad_text = (unsigned char *)strstr((char *)encoded_surrogate, "\xc0\xaf");
   assert(bad_text != NULL);
   bad_text[0] = 0xed;
   bad_text[1] = 0xa0;
   memmove(bad_text + 3, bad_text + 2,
           sizeof(invalid_utf8) - (size_t)(bad_text + 2 - encoded_surrogate));
   bad_text[2] = 0x80;
   assert(kb_bedrock_nonstream_parse(encoded_surrogate, sizeof(invalid_utf8), &r) ==
          KB_BEDROCK_MALFORMED_RESPONSE);

   static const unsigned char valid_utf8[] =
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":[{\"text\":"
       "\"\xf0\x9f\x98\x80\"}]}},"
       "\"stopReason\":\"end_turn\",\"usage\":{\"inputTokens\":0,\"outputTokens\":0}}";
   assert(kb_bedrock_nonstream_parse(valid_utf8, sizeof(valid_utf8) - 1, &r) == KB_BEDROCK_OK);
   aimee_response_free(&r);
}

static size_t parser_allocations;

static void *counting_cjson_malloc(size_t size)
{
   parser_allocations++;
   return malloc(size);
}

static void response_preparse_node_cap_test(void)
{
   const size_t values = 17000;
   size_t cap = values * 2 + 16;
   unsigned char *body = malloc(cap);
   assert(body != NULL);
   size_t at = 0;
   memcpy(body + at, "{\"x\":[", 6);
   at += 6;
   for (size_t i = 0; i < values; i++)
   {
      body[at++] = '0';
      if (i + 1 < values)
         body[at++] = ',';
   }
   body[at++] = ']';
   body[at++] = '}';
   cJSON_Hooks hooks = {.malloc_fn = counting_cjson_malloc, .free_fn = free};
   cJSON_InitHooks(&hooks);
   parser_allocations = 0;
   aimee_response_t response;
   kb_bedrock_response_init(&response);
   assert(kb_bedrock_nonstream_parse(body, at, &response) == KB_BEDROCK_MALFORMED_RESPONSE);
   assert(parser_allocations == 0);
   cJSON_InitHooks(NULL);
   free(body);
}

static void stream_state_tests(void)
{
   kb_bedrock_stream_t *s = NULL;
   assert(kb_bedrock_stream_init(&s, NULL, NULL) == KB_BEDROCK_OK);
   assert(kb_bedrock_stream_feed(s, NULL, 0) == KB_BEDROCK_OK);
   assert(kb_bedrock_stream_finish(s) == KB_BEDROCK_INCOMPLETE_STREAM);
   assert(kb_bedrock_stream_finish(s) == KB_BEDROCK_POISONED);
   assert(kb_bedrock_stream_clear(&s) == KB_BEDROCK_OK && s == NULL);
}

typedef struct
{
   int count;
   int abort_after;
   aimee_delta_type_t type[16];
   aimee_stop_reason_t stop_reason;
   long usage_in, usage_out;
   char error[128];
} delta_log_t;

static int collect_delta(const aimee_delta_t *delta, void *ctx)
{
   delta_log_t *log = ctx;
   assert(log->count < (int)(sizeof(log->type) / sizeof(log->type[0])));
   log->type[log->count++] = delta->type;
   if (delta->type == AIMEE_DELTA_TURN_STOP)
   {
      log->stop_reason = delta->stop_reason;
      log->usage_in = delta->usage_in;
      log->usage_out = delta->usage_out;
   }
   if (delta->type == AIMEE_DELTA_ERROR && delta->error_message)
      snprintf(log->error, sizeof(log->error), "%s", delta->error_message);
   return log->abort_after > 0 && log->count >= log->abort_after;
}

typedef struct
{
   kb_bedrock_stream_t **stream;
   int called;
} reentrant_log_t;

static int reenter_stream(const aimee_delta_t *delta, void *ctx)
{
   reentrant_log_t *log = ctx;
   (void)delta;
   log->called++;
   assert(kb_bedrock_stream_feed(*log->stream, NULL, 0) == KB_BEDROCK_BUSY);
   assert(kb_bedrock_stream_finish(*log->stream) == KB_BEDROCK_BUSY);
   assert(kb_bedrock_stream_clear(log->stream) == KB_BEDROCK_BUSY);
   assert(*log->stream != NULL);
   return 0;
}

static kb_bedrock_result_t feed_frame(kb_bedrock_stream_t *stream, uint8_t **frame,
                                      size_t frame_len)
{
   kb_bedrock_result_t result = kb_bedrock_stream_feed(stream, *frame, frame_len);
   test_aws_es_fixture_free(frame, frame_len);
   return result;
}

static kb_bedrock_result_t feed_event_result(kb_bedrock_stream_t *stream, const char *type,
                                             const char *json)
{
   uint8_t *frame = NULL;
   size_t frame_len = 0;
   assert(test_aws_es_event(type, json, &frame, &frame_len) == 0);
   return feed_frame(stream, &frame, frame_len);
}

static void feed_event(kb_bedrock_stream_t *stream, const char *type, const char *json)
{
   uint8_t *frame = NULL;
   size_t frame_len = 0;
   assert(test_aws_es_event(type, json, &frame, &frame_len) == 0);
   for (size_t i = 0; i < frame_len; i++)
      assert(kb_bedrock_stream_feed(stream, frame + i, 1) == KB_BEDROCK_OK);
   test_aws_es_fixture_free(&frame, frame_len);
}

static void stream_lifecycle_tests(void)
{
   delta_log_t log = {0};
   kb_bedrock_stream_t *stream = NULL;
   assert(kb_bedrock_stream_init(&stream, collect_delta, &log) == KB_BEDROCK_OK);
   feed_event(stream, "messageStart", "{\"role\":\"assistant\"}");
   feed_event(stream, "contentBlockDelta",
              "{\"contentBlockIndex\":0,\"delta\":{\"text\":\"hello\"}}");
   feed_event(stream, "contentBlockStop", "{\"contentBlockIndex\":0}");
   feed_event(stream, "messageStop", "{\"stopReason\":\"end_turn\"}");
   feed_event(stream, "metadata", "{\"usage\":{\"inputTokens\":3,\"outputTokens\":1}}");
   assert(kb_bedrock_stream_finish(stream) == KB_BEDROCK_OK);
   assert(log.count == 5);
   assert(log.type[0] == AIMEE_DELTA_TURN_START);
   assert(log.type[1] == AIMEE_DELTA_BLOCK_START);
   assert(log.type[2] == AIMEE_DELTA_BLOCK_DELTA);
   assert(log.type[3] == AIMEE_DELTA_BLOCK_STOP);
   assert(log.type[4] == AIMEE_DELTA_TURN_STOP);
   assert(log.stop_reason == AIMEE_STOP_END_TURN && log.usage_in == 3 && log.usage_out == 1);
   assert(kb_bedrock_stream_clear(&stream) == KB_BEDROCK_OK);
}

static void assert_poisoned(kb_bedrock_stream_t *stream)
{
   assert(kb_bedrock_stream_feed(stream, NULL, 0) == KB_BEDROCK_POISONED);
   assert(kb_bedrock_stream_finish(stream) == KB_BEDROCK_POISONED);
}

static void stream_semantic_header_tests(void)
{
   static const char payload[] = "{\"role\":\"assistant\"}";
   test_aws_es_header_t base[] = {string_header(":message-type", "event"),
                                  string_header(":event-type", "messageStart"),
                                  string_header(":content-type", "application/json")};
   for (int which = 0; which < 3; which++)
   {
      test_aws_es_header_t h[4];
      memcpy(h, base, sizeof(base));
      h[3] = base[which];
      uint8_t *frame = NULL;
      size_t frame_len = 0;
      assert(test_aws_es_message(h, 4, payload, strlen(payload), &frame, &frame_len) == 0);
      kb_bedrock_stream_t *stream = NULL;
      assert(kb_bedrock_stream_init(&stream, NULL, NULL) == KB_BEDROCK_OK);
      assert(feed_frame(stream, &frame, frame_len) == KB_BEDROCK_MALFORMED_STREAM);
      assert_poisoned(stream);
      assert(kb_bedrock_stream_clear(&stream) == KB_BEDROCK_OK);
   }

   test_aws_es_header_t wrong[] = {
       string_header(":message-type", "event"),
       {.name = ":event-type", .value_type = AWS_ES_HDR_BOOL_TRUE, .value = NULL, .value_len = 0},
       string_header(":content-type", "application/json")};
   uint8_t *frame = NULL;
   size_t frame_len = 0;
   assert(test_aws_es_message(wrong, 3, payload, strlen(payload), &frame, &frame_len) == 0);
   kb_bedrock_stream_t *stream = NULL;
   assert(kb_bedrock_stream_init(&stream, NULL, NULL) == KB_BEDROCK_OK);
   assert(feed_frame(stream, &frame, frame_len) == KB_BEDROCK_MALFORMED_STREAM);
   assert_poisoned(stream);
   assert(kb_bedrock_stream_clear(&stream) == KB_BEDROCK_OK);

   reentrant_log_t reentrant = {.stream = &stream};
   assert(kb_bedrock_stream_init(&stream, reenter_stream, &reentrant) == KB_BEDROCK_OK);
   feed_event(stream, "messageStart", "{\"role\":\"assistant\"}");
   assert(reentrant.called == 1);
   assert(kb_bedrock_stream_clear(&stream) == KB_BEDROCK_OK);
}

static void stream_provider_error_tests(void)
{
   delta_log_t log = {0};
   kb_bedrock_stream_t *stream = NULL;
   uint8_t *frame = NULL;
   size_t frame_len = 0;
   assert(kb_bedrock_stream_init(&stream, collect_delta, &log) == KB_BEDROCK_OK);
   assert(test_aws_es_exception("throttlingException", "{\"message\":\"slow down\"}", &frame,
                                &frame_len) == 0);
   assert(feed_frame(stream, &frame, frame_len) == KB_BEDROCK_PROVIDER_ERROR);
   assert(log.count == 1 && log.type[0] == AIMEE_DELTA_ERROR);
   assert(strcmp(log.error, "slow down") == 0);
   assert_poisoned(stream);
   assert(kb_bedrock_stream_clear(&stream) == KB_BEDROCK_OK);

   memset(&log, 0, sizeof(log));
   assert(kb_bedrock_stream_init(&stream, collect_delta, &log) == KB_BEDROCK_OK);
   assert(test_aws_es_error("InternalFailure", "provider boom", &frame, &frame_len) == 0);
   assert(feed_frame(stream, &frame, frame_len) == KB_BEDROCK_PROVIDER_ERROR);
   assert(log.count == 1 && log.type[0] == AIMEE_DELTA_ERROR);
   assert(strcmp(log.error, "provider boom") == 0);
   assert_poisoned(stream);
   assert(kb_bedrock_stream_clear(&stream) == KB_BEDROCK_OK);

   memset(&log, 0, sizeof(log));
   log.abort_after = 1;
   assert(kb_bedrock_stream_init(&stream, collect_delta, &log) == KB_BEDROCK_OK);
   assert(test_aws_es_exception("throttlingException", "{\"message\":\"abort me\"}", &frame,
                                &frame_len) == 0);
   assert(feed_frame(stream, &frame, frame_len) == KB_BEDROCK_CALLBACK_ABORT);
   assert(log.count == 1 && strcmp(log.error, "abort me") == 0);
   assert_poisoned(stream);
   assert(kb_bedrock_stream_clear(&stream) == KB_BEDROCK_OK);
}

static void stream_terminal_error_tests(void)
{
   for (int with_metadata = 0; with_metadata <= 1; with_metadata++)
      for (int exception = 0; exception <= 1; exception++)
      {
         delta_log_t log = {0};
         kb_bedrock_stream_t *stream = NULL;
         assert(kb_bedrock_stream_init(&stream, collect_delta, &log) == KB_BEDROCK_OK);
         feed_event(stream, "messageStart", "{\"role\":\"assistant\"}");
         feed_event(stream, "contentBlockDelta",
                    "{\"contentBlockIndex\":0,\"delta\":{\"text\":\"x\"}}");
         feed_event(stream, "contentBlockStop", "{\"contentBlockIndex\":0}");
         feed_event(stream, "messageStop", "{\"stopReason\":\"end_turn\"}");
         if (with_metadata)
            feed_event(stream, "metadata", "{\"usage\":{\"inputTokens\":1,\"outputTokens\":1}}");
         int callbacks = log.count;
         uint8_t *frame = NULL;
         size_t frame_len = 0;
         if (exception)
            assert(test_aws_es_exception("throttlingException", "{\"message\":\"late\"}", &frame,
                                         &frame_len) == 0);
         else
            assert(test_aws_es_error("LateError", "late", &frame, &frame_len) == 0);
         assert(feed_frame(stream, &frame, frame_len) == KB_BEDROCK_MALFORMED_STREAM);
         assert(log.count == callbacks);
         assert_poisoned(stream);
         assert(kb_bedrock_stream_clear(&stream) == KB_BEDROCK_OK);
      }
}

static void stream_order_and_abort_tests(void)
{
   kb_bedrock_stream_t *stream = NULL;
   assert(kb_bedrock_stream_init(&stream, NULL, NULL) == KB_BEDROCK_OK);
   feed_event(stream, "futureEvent", "{\"future\":true}");
   feed_event(stream, "messageStart", "{\"role\":\"assistant\"}");
   feed_event(stream, "contentBlockDelta", "{\"contentBlockIndex\":0,\"delta\":{\"text\":\"x\"}}");
   feed_event(stream, "contentBlockStop", "{\"contentBlockIndex\":0}");
   feed_event(stream, "messageStop", "{\"stopReason\":\"end_turn\"}");
   feed_event(stream, "metadata", "{\"usage\":{\"inputTokens\":0,\"outputTokens\":0}}");
   assert(kb_bedrock_stream_finish(stream) == KB_BEDROCK_OK);
   assert(kb_bedrock_stream_clear(&stream) == KB_BEDROCK_OK);

   struct bad_event
   {
      const char *type;
      const char *json;
      int start_first;
   } cases[] = {
       {"contentBlockDelta", "{\"contentBlockIndex\":0,\"delta\":{\"text\":\"x\"}}", 0},
       {"contentBlockStop", "{\"contentBlockIndex\":0}", 1},
       {"contentBlockDelta", "{\"contentBlockIndex\":0.5,\"delta\":{\"text\":\"x\"}}", 1},
       {"contentBlockDelta", "{\"contentBlockIndex\":64,\"delta\":{\"text\":\"x\"}}", 1},
       {"contentBlockDelta", "{\"delta\":{\"text\":\"x\"}}", 1},
       {"contentBlockDelta", "{\"contentBlockIndex\":0,\"delta\":{\"text\":\"x\",\"future\":{}}}",
        1},
       {"contentBlockDelta", "{\"contentBlockIndex\":0,\"delta\":{\"text\":\"\\uD800\"}}", 1},
       {"metadata", "{\"usage\":{\"inputTokens\":1,\"outputTokens\":1}}", 1},
       {"messageStart", "{\"role\":\"assistant\"}", 1}};
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      kb_bedrock_stream_t *stream = NULL;
      assert(kb_bedrock_stream_init(&stream, NULL, NULL) == KB_BEDROCK_OK);
      if (cases[i].start_first)
         feed_event(stream, "messageStart", "{\"role\":\"assistant\"}");
      assert(feed_event_result(stream, cases[i].type, cases[i].json) ==
             KB_BEDROCK_MALFORMED_STREAM);
      assert_poisoned(stream);
      assert(kb_bedrock_stream_clear(&stream) == KB_BEDROCK_OK);
   }

   stream = NULL;
   assert(kb_bedrock_stream_init(&stream, NULL, NULL) == KB_BEDROCK_OK);
   feed_event(stream, "messageStart", "{\"role\":\"assistant\"}");
   feed_event(stream, "contentBlockStart",
              "{\"contentBlockIndex\":0,\"start\":{\"toolUse\":{\"toolUseId\":\"id\","
              "\"name\":\"tool\"}}}");
   assert(feed_event_result(stream, "contentBlockDelta",
                            "{\"contentBlockIndex\":0,\"delta\":{\"toolUse\":{\"input\":\"{}\","
                            "\"future\":true}}}") == KB_BEDROCK_MALFORMED_STREAM);
   assert_poisoned(stream);
   assert(kb_bedrock_stream_clear(&stream) == KB_BEDROCK_OK);

   assert(kb_bedrock_stream_init(&stream, NULL, NULL) == KB_BEDROCK_OK);
   feed_event(stream, "messageStart", "{\"role\":\"assistant\"}");
   feed_event(stream, "contentBlockDelta",
              "{\"contentBlockIndex\":0,\"delta\":{\"text\":\"open\"}}");
   assert(feed_event_result(stream, "messageStop", "{\"stopReason\":\"end_turn\"}") ==
          KB_BEDROCK_MALFORMED_STREAM);
   assert_poisoned(stream);
   assert(kb_bedrock_stream_clear(&stream) == KB_BEDROCK_OK);

   delta_log_t abort_log = {.abort_after = 1};
   assert(kb_bedrock_stream_init(&stream, collect_delta, &abort_log) == KB_BEDROCK_OK);
   assert(feed_event_result(stream, "messageStart", "{\"role\":\"assistant\"}") ==
          KB_BEDROCK_CALLBACK_ABORT);
   assert(abort_log.count == 1);
   assert_poisoned(stream);
   assert(kb_bedrock_stream_clear(&stream) == KB_BEDROCK_OK);

   for (int abort_after = 2; abort_after <= 3; abort_after++)
   {
      abort_log = (delta_log_t){.abort_after = abort_after};
      assert(kb_bedrock_stream_init(&stream, collect_delta, &abort_log) == KB_BEDROCK_OK);
      feed_event(stream, "messageStart", "{\"role\":\"assistant\"}");
      assert(feed_event_result(stream, "contentBlockDelta",
                               "{\"contentBlockIndex\":0,\"delta\":{\"text\":\"x\"}}") ==
             KB_BEDROCK_CALLBACK_ABORT);
      assert(abort_log.count == abort_after);
      assert(abort_log.type[1] == AIMEE_DELTA_BLOCK_START);
      if (abort_after == 3)
         assert(abort_log.type[2] == AIMEE_DELTA_BLOCK_DELTA);
      assert_poisoned(stream);
      assert(kb_bedrock_stream_clear(&stream) == KB_BEDROCK_OK);
   }

   const char *bad_cache_metadata[] = {
       "{\"usage\":{\"inputTokens\":1,\"outputTokens\":1,\"cacheReadInputTokens\":-1}}",
       "{\"usage\":{\"inputTokens\":1,\"outputTokens\":1,\"cacheReadInputTokens\":1.5}}",
       "{\"usage\":{\"inputTokens\":1,\"outputTokens\":1,\"cacheWriteInputTokens\":\"1\"}}",
       "{\"usage\":{\"inputTokens\":1,\"outputTokens\":1,\"cacheWriteInputTokens\":1e9999}}"};
   for (size_t i = 0; i < sizeof(bad_cache_metadata) / sizeof(bad_cache_metadata[0]); i++)
   {
      assert(kb_bedrock_stream_init(&stream, NULL, NULL) == KB_BEDROCK_OK);
      feed_event(stream, "messageStart", "{\"role\":\"assistant\"}");
      feed_event(stream, "contentBlockDelta",
                 "{\"contentBlockIndex\":0,\"delta\":{\"text\":\"x\"}}");
      feed_event(stream, "contentBlockStop", "{\"contentBlockIndex\":0}");
      feed_event(stream, "messageStop", "{\"stopReason\":\"end_turn\"}");
      assert(feed_event_result(stream, "metadata", bad_cache_metadata[i]) ==
             KB_BEDROCK_MALFORMED_STREAM);
      assert_poisoned(stream);
      assert(kb_bedrock_stream_clear(&stream) == KB_BEDROCK_OK);
   }

   assert(kb_bedrock_stream_init(&stream, NULL, NULL) == KB_BEDROCK_OK);
   feed_event(stream, "messageStart", "{\"role\":\"assistant\"}");
   assert(feed_event_result(stream, "messageStop", "{\"stopReason\":\"end_turn\"}") ==
          KB_BEDROCK_MALFORMED_STREAM);
   assert_poisoned(stream);
   assert(kb_bedrock_stream_clear(&stream) == KB_BEDROCK_OK);

   assert(kb_bedrock_stream_init(&stream, NULL, NULL) == KB_BEDROCK_OK);
   feed_event(stream, "messageStart", "{\"role\":\"assistant\"}");
   assert(feed_event_result(stream, "contentBlockDelta",
                            "{\"contentBlockIndex\":0,\"delta\":{\"text\":\"\"}}") ==
          KB_BEDROCK_MALFORMED_STREAM);
   assert_poisoned(stream);
   assert(kb_bedrock_stream_clear(&stream) == KB_BEDROCK_OK);

   assert(kb_bedrock_stream_init(&stream, NULL, NULL) == KB_BEDROCK_OK);
   feed_event(stream, "messageStart", "{\"role\":\"assistant\"}");
   feed_event(stream, "contentBlockDelta", "{\"contentBlockIndex\":0,\"delta\":{\"text\":\"x\"}}");
   feed_event(stream, "contentBlockStop", "{\"contentBlockIndex\":0}");
   feed_event(stream, "messageStop", "{\"stopReason\":\"end_turn\"}");
   assert(feed_event_result(stream, "contentBlockDelta",
                            "{\"contentBlockIndex\":0,\"delta\":{\"text\":\"late\"}}") ==
          KB_BEDROCK_MALFORMED_STREAM);
   assert_poisoned(stream);
   assert(kb_bedrock_stream_clear(&stream) == KB_BEDROCK_OK);
}

static void stream_large_coalesced_test(void)
{
   const char prefix[] = "{\"future\":\"";
   const char suffix[] = "\"}";
   size_t payload_len = KB_BEDROCK_EVENT_JSON_MAX - 128;
   char *payload = malloc(payload_len + 1);
   assert(payload != NULL);
   memcpy(payload, prefix, sizeof(prefix) - 1);
   memset(payload + sizeof(prefix) - 1, 'a',
          payload_len - (sizeof(prefix) - 1) - (sizeof(suffix) - 1));
   memcpy(payload + payload_len - (sizeof(suffix) - 1), suffix, sizeof(suffix));

   uint8_t *frame = NULL;
   size_t frame_len = 0;
   assert(test_aws_es_event("futureEvent", payload, &frame, &frame_len) == 0);
   free(payload);
   size_t repeats = KB_BEDROCK_BODY_MAX / frame_len + 2;
   size_t aggregate_len = repeats * frame_len;
   assert(aggregate_len > KB_BEDROCK_BODY_MAX);
   uint8_t *aggregate = malloc(aggregate_len);
   assert(aggregate != NULL);
   for (size_t i = 0; i < repeats; i++)
      memcpy(aggregate + i * frame_len, frame, frame_len);
   test_aws_es_fixture_free(&frame, frame_len);

   kb_bedrock_stream_t *stream = NULL;
   assert(kb_bedrock_stream_init(&stream, NULL, NULL) == KB_BEDROCK_OK);
   feed_event(stream, "messageStart", "{\"role\":\"assistant\"}");
   assert(kb_bedrock_stream_feed(stream, aggregate, aggregate_len) == KB_BEDROCK_OK);
   free(aggregate);
   feed_event(stream, "contentBlockDelta", "{\"contentBlockIndex\":0,\"delta\":{\"text\":\"x\"}}");
   feed_event(stream, "contentBlockStop", "{\"contentBlockIndex\":0}");
   feed_event(stream, "messageStop", "{\"stopReason\":\"end_turn\"}");
   feed_event(stream, "metadata", "{\"usage\":{\"inputTokens\":0,\"outputTokens\":0}}");
   assert(kb_bedrock_stream_finish(stream) == KB_BEDROCK_OK);
   assert(kb_bedrock_stream_clear(&stream) == KB_BEDROCK_OK);
}

static unsigned char *dispatch_stream_body(size_t *out_len)
{
   static const char *types[] = {"messageStart", "contentBlockDelta", "contentBlockStop",
                                 "messageStop", "metadata"};
   static const char *json[] = {
       "{\"role\":\"assistant\"}",
       "{\"contentBlockIndex\":0,\"delta\":{\"text\":\"ok\"}}",
       "{\"contentBlockIndex\":0}", "{\"stopReason\":\"end_turn\"}",
       "{\"usage\":{\"inputTokens\":1,\"outputTokens\":1}}"};
   unsigned char *body = NULL;
   size_t length = 0;
   for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
   {
      uint8_t *frame = NULL;
      size_t frame_len = 0;
      assert(test_aws_es_event(types[i], json[i], &frame, &frame_len) == 0);
      unsigned char *next = realloc(body, length + frame_len);
      assert(next != NULL);
      body = next;
      memcpy(body + length, frame, frame_len);
      length += frame_len;
      test_aws_es_fixture_free(&frame, frame_len);
   }
   *out_len = length;
   return body;
}

static void dispatch_wrapper_tests(void)
{
   static const unsigned char good[] =
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":[{\"text\":\"ok\"}]}},"
       "\"stopReason\":\"end_turn\",\"usage\":{\"inputTokens\":1,\"outputTokens\":2}}";
   aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = "hello"};
   aimee_message_t message = {.role = "user", .blocks = &block, .n_blocks = 1};
   aimee_request_t request = {.messages = &message, .n_messages = 1};
   db2_bedrock_target_t t = target("aws", "us-east-1", "model");
   kb_bedrock_credentials_t c = credentials();
   aimee_response_t response;
   kb_bedrock_response_init(&response);
   int status = 777;

   dispatch_mock = (dispatch_mock_t){.status = 200,
                                     .content_type = "application/json",
                                     .body = good,
                                     .body_len = sizeof(good) - 1,
                                     .fragment = 3};
   assert(kb_bedrock_dispatch_buffered(&t, &request, &c, &response, &status) == KB_BEDROCK_OK);
   assert(status == 200 && response.n_content == 1 &&
          strcmp(response.content[0].text, "ok") == 0 && dispatch_mock.body_calls > 1);

   int calls = dispatch_mock.calls;
   snprintf(t.endpoint, sizeof(t.endpoint), "https://forbidden.example");
   status = 888;
   assert(kb_bedrock_dispatch_buffered(&t, &request, &c, &response, &status) ==
          KB_BEDROCK_INVALID_TARGET);
   assert(status == 0 && response.content == NULL && dispatch_mock.calls == calls);
   t.endpoint[0] = 0;

   status = 999;
   assert(kb_bedrock_dispatch_buffered(NULL, &request, &c, &response, &status) ==
          KB_BEDROCK_INVALID_ARGUMENT);
   assert(status == 0 && response.content == NULL && dispatch_mock.calls == calls);

   dispatch_mock = (dispatch_mock_t){.status = 200,
                                     .content_type = NULL,
                                     .body = good,
                                     .body_len = sizeof(good) - 1};
   assert(kb_bedrock_dispatch_buffered(&t, &request, &c, &response, &status) ==
          KB_BEDROCK_MALFORMED_RESPONSE);
   assert(status == 0 && dispatch_mock.body_calls == 0 && response.content == NULL);

   dispatch_mock = (dispatch_mock_t){.status = 200,
                                     .content_type = "application/json; charset=utf-8",
                                     .body = good,
                                     .body_len = sizeof(good) - 1};
   assert(kb_bedrock_dispatch_buffered(&t, &request, &c, &response, &status) ==
          KB_BEDROCK_MALFORMED_RESPONSE);
   assert(status == 0 && dispatch_mock.body_calls == 0);

   dispatch_mock = (dispatch_mock_t){.status = 429,
                                     .content_type = "application/json",
                                     .body = good,
                                     .body_len = sizeof(good) - 1};
   assert(kb_bedrock_dispatch_buffered(&t, &request, &c, &response, &status) ==
          KB_BEDROCK_PROVIDER_ERROR);
   assert(status == 429 && dispatch_mock.body_calls == 0);

   dispatch_mock = (dispatch_mock_t){.transport_result = KB_HTTP_TLS_ERROR};
   assert(kb_bedrock_dispatch_buffered(&t, &request, &c, &response, &status) ==
          KB_BEDROCK_TRANSPORT_ERROR);
   assert(status == 0 && response.content == NULL);

   size_t stream_len = 0;
   unsigned char *stream_body = dispatch_stream_body(&stream_len);
   delta_log_t log = {0};
   dispatch_mock = (dispatch_mock_t){.status = 200,
                                     .content_type = "application/vnd.amazon.eventstream",
                                     .body = stream_body,
                                     .body_len = stream_len,
                                     .fragment = 1};
   assert(kb_bedrock_dispatch_stream(&t, &request, &c, collect_delta, &log, &status) ==
          KB_BEDROCK_OK);
   assert(status == 200 && log.count == 5 && dispatch_mock.body_calls == (int)stream_len);

   log = (delta_log_t){0};
   dispatch_mock = (dispatch_mock_t){.status = 200,
                                     .content_type = "application/vnd.amazon.eventstream",
                                     .body = stream_body,
                                     .body_len = stream_len,
                                     .fragment = 13,
                                     .post_body_result = KB_HTTP_MALFORMED_RESPONSE};
   assert(kb_bedrock_dispatch_stream(&t, &request, &c, collect_delta, &log, &status) ==
          KB_BEDROCK_TRANSPORT_ERROR);
   assert(status == 0 && log.count == 4);
   for (int i = 0; i < log.count; i++)
      assert(log.type[i] != AIMEE_DELTA_TURN_STOP);

   log = (delta_log_t){.abort_after = 1};
   dispatch_mock = (dispatch_mock_t){.status = 200,
                                     .content_type = "application/vnd.amazon.eventstream",
                                     .body = stream_body,
                                     .body_len = stream_len,
                                     .fragment = 7};
   assert(kb_bedrock_dispatch_stream(&t, &request, &c, collect_delta, &log, &status) ==
          KB_BEDROCK_CALLBACK_ABORT);
   assert(status == 0 && log.count == 1);

   log = (delta_log_t){.abort_after = 5};
   dispatch_mock = (dispatch_mock_t){.status = 200,
                                     .content_type = "application/vnd.amazon.eventstream",
                                     .body = stream_body,
                                     .body_len = stream_len,
                                     .fragment = 7};
   assert(kb_bedrock_dispatch_stream(&t, &request, &c, collect_delta, &log, &status) ==
          KB_BEDROCK_CALLBACK_ABORT);
   assert(status == 0 && log.count == 5 && log.type[4] == AIMEE_DELTA_TURN_STOP);

   dispatch_mock = (dispatch_mock_t){.status = 200,
                                     .content_type = "application/vnd.amazon.eventstream",
                                     .body = stream_body,
                                     .body_len = stream_len - 1,
                                     .fragment = 11};
   assert(kb_bedrock_dispatch_stream(&t, &request, &c, NULL, NULL, &status) != KB_BEDROCK_OK);
   assert(status == 0);
   free(stream_body);
   aimee_response_free(&response);
}

int main(void)
{
   request_tests();
   response_tests();
   response_preparse_node_cap_test();
   stream_state_tests();
   stream_lifecycle_tests();
   stream_semantic_header_tests();
   stream_provider_error_tests();
   stream_terminal_error_tests();
   stream_order_and_abort_tests();
   stream_large_coalesced_test();
   dispatch_wrapper_tests();
   puts("kb bedrock egress: pure request/response engine tests passed");
   return 0;
}
