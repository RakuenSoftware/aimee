#include "kb/kb_bedrock_egress.h"
#include "tests/support/aws_eventstream_fixture.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

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
   aimee_request_t ir;
   memset(&ir, 0, sizeof(ir));
   kb_bedrock_credentials_t c = credentials();
   for (int i = 0; i < 3; i++)
   {
      db2_bedrock_target_t t = target(partitions[i], "us-west-2", "model/a:b");
      if (i == 1)
         snprintf(t.model_id, sizeof(t.model_id), "model:id");
      kb_bedrock_wire_request_t q;
      kb_bedrock_wire_request_init(&q);
      assert(kb_bedrock_wire_request_build(&t, &ir, i & 1, &c, &q) == KB_BEDROCK_OK);
      assert(strstr(q.host, i == 2 ? "amazonaws.com.cn" : "amazonaws.com") != NULL);
      assert(strstr(q.encoded_path, "%3A") != NULL);
      assert(strstr(q.sig.canonical_request, q.encoded_path) != NULL);
      assert(strstr(q.sig.signed_headers, "x-amz-content-sha256") != NULL);
      kb_bedrock_header_t h[KB_BEDROCK_MAX_HEADERS];
      size_t n = 0;
      assert(kb_bedrock_wire_request_headers(&q, h, 6, &n) == KB_BEDROCK_OK && n == 6);
      assert(strcmp(h[3].value, q.payload_hash) == 0);
      kb_bedrock_wire_request_clear(&q);
      assert(q.body == NULL && q.body_len == 0 && q.host[0] == 0);
   }

   db2_bedrock_target_t t = target("aws", "us-east-1", "model");
   ir.has_top_k = 1;
   kb_bedrock_wire_request_t q;
   kb_bedrock_wire_request_init(&q);
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_ARGUMENT);
   t.endpoint[0] = 'x';
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_TARGET);

   memset(&ir, 0, sizeof(ir));
   t = target("aws", "us-east-1",
              "arn:aws:bedrock:us-east-1::foundation-model/anthropic.claude-v2");
   aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = "line one\nline two"};
   aimee_message_t message = {.role = "user", .blocks = &block, .n_blocks = 1};
   ir.messages = &message;
   ir.n_messages = 1;
   c.session_token = NULL;
   assert(kb_bedrock_wire_request_build(&t, &ir, 1, &c, &q) == KB_BEDROCK_OK);
   assert(strstr(q.encoded_path, "foundation-model/anthropic.claude-v2/converse-stream") != NULL);
   assert(strstr(q.sig.canonical_request, q.encoded_path) != NULL);
   assert(strstr(q.body, "line one\\nline two") != NULL);
   kb_bedrock_header_t headers[KB_BEDROCK_MAX_HEADERS];
   size_t n_headers = 0;
   assert(kb_bedrock_wire_request_headers(&q, headers, KB_BEDROCK_MAX_HEADERS, &n_headers) ==
          KB_BEDROCK_OK);
   assert(n_headers == 5);
   kb_bedrock_wire_request_clear(&q);

   c = credentials();
   c.date = "20260102";
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_ARGUMENT);
   c = credentials();
   snprintf(t.model_family, sizeof(t.model_family), "unknown-family");
   assert(kb_bedrock_wire_request_build(&t, &ir, 0, &c, &q) == KB_BEDROCK_INVALID_TARGET);

   char legacy[16] = "stale";
   int status = 999;
   assert(kb_bedrock_dispatch_https(NULL, NULL, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, legacy,
                                    sizeof(legacy), &status) == -1);
   assert(legacy[0] == 0 && status == 0);
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
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":[]}},"
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
   static const unsigned char ambiguous_content[] =
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":["
       "{\"text\":\"x\",\"toolUse\":{}}]}},\"stopReason\":\"end_turn\","
       "\"usage\":{\"inputTokens\":0,\"outputTokens\":0}}";
   static const unsigned char nul_escape[] =
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":["
       "{\"text\":\"x\\u0000y\"}]}},\"stopReason\":\"end_turn\","
       "\"usage\":{\"inputTokens\":0,\"outputTokens\":0}}";
   const unsigned char *invalid[] = {not_object,       duplicate_key,     negative_usage,
                                     fractional_usage, ambiguous_content, nul_escape};
   const size_t invalid_len[] = {sizeof(not_object) - 1,        sizeof(duplicate_key) - 1,
                                 sizeof(negative_usage) - 1,    sizeof(fractional_usage) - 1,
                                 sizeof(ambiguous_content) - 1, sizeof(nul_escape) - 1};
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
   char error[128];
} delta_log_t;

static int collect_delta(const aimee_delta_t *delta, void *ctx)
{
   delta_log_t *log = ctx;
   assert(log->count < (int)(sizeof(log->type) / sizeof(log->type[0])));
   log->type[log->count++] = delta->type;
   if (delta->type == AIMEE_DELTA_ERROR && delta->error_message)
      snprintf(log->error, sizeof(log->error), "%s", delta->error_message);
   return log->abort_after > 0 && log->count >= log->abort_after;
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
   assert(log.type[1] == AIMEE_DELTA_BLOCK_DELTA);
   assert(log.type[2] == AIMEE_DELTA_BLOCK_STOP);
   assert(log.type[3] == AIMEE_DELTA_TURN_STOP);
   assert(log.type[4] == AIMEE_DELTA_TURN_STOP);
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
}

static void stream_order_and_abort_tests(void)
{
   struct bad_event
   {
      const char *type;
      const char *json;
      int start_first;
   } cases[] = {{"contentBlockDelta", "{\"contentBlockIndex\":0,\"delta\":{\"text\":\"x\"}}", 0},
                {"contentBlockStop", "{\"contentBlockIndex\":0}", 1},
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

   kb_bedrock_stream_t *stream = NULL;
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

   assert(kb_bedrock_stream_init(&stream, NULL, NULL) == KB_BEDROCK_OK);
   feed_event(stream, "messageStart", "{\"role\":\"assistant\"}");
   feed_event(stream, "messageStop", "{\"stopReason\":\"end_turn\"}");
   assert(feed_event_result(stream, "contentBlockDelta",
                            "{\"contentBlockIndex\":0,\"delta\":{\"text\":\"late\"}}") ==
          KB_BEDROCK_MALFORMED_STREAM);
   assert_poisoned(stream);
   assert(kb_bedrock_stream_clear(&stream) == KB_BEDROCK_OK);
}

int main(void)
{
   request_tests();
   response_tests();
   stream_state_tests();
   stream_lifecycle_tests();
   stream_semantic_header_tests();
   stream_provider_error_tests();
   stream_order_and_abort_tests();
   puts("kb bedrock egress: pure request/response engine tests passed");
   return 0;
}
