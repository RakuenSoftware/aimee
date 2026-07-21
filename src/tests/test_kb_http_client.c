#include "kb/http/kb_http_client.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
   int headers_called, body_called, abort_body;
   kb_http_gate_t gate;
   kb_http_response_t response;
   unsigned char body[1024];
   size_t body_len;
} capture_t;

static kb_http_gate_t capture_headers(const kb_http_response_t *response, void *context)
{
   capture_t *capture = context;
   assert(capture->headers_called == 0);
   assert(capture->body_called == 0);
   capture->headers_called = 1;
   capture->response = *response;
   return capture->gate;
}

static kb_http_body_action_t capture_body(const unsigned char *bytes, size_t length, void *context)
{
   capture_t *capture = context;
   assert(capture->headers_called == 1);
   assert(length > 0);
   capture->body_called++;
   if (capture->abort_body)
      return KB_HTTP_BODY_CALLER_ABORT;
   assert(capture->body_len + length <= sizeof(capture->body));
   memcpy(capture->body + capture->body_len, bytes, length);
   capture->body_len += length;
   return KB_HTTP_BODY_CONTINUE;
}

static kb_http_result_t parse_parts(const unsigned char *wire, size_t length, size_t split,
                                    size_t body_max, capture_t *capture)
{
   kb_http_response_parser_t *parser = NULL;
   assert(kb_http_response_parser_init(&parser, body_max, capture_headers, capture_body, capture) ==
          KB_HTTP_OK);
   kb_http_result_t result = kb_http_response_parser_feed(parser, wire, split);
   if (result == KB_HTTP_MORE)
      result = kb_http_response_parser_feed(parser, wire + split, length - split);
   if (result == KB_HTTP_MORE)
      result = kb_http_response_parser_finish_eof(parser);
   kb_http_response_parser_free(&parser);
   assert(parser == NULL);
   return result;
}

static void content_length_boundaries(void)
{
   static const unsigned char response[] =
       "HTTP/1.1 200 OK\r\nContent-Type:\t application/json \t\r\nContent-Length: 5\r\n"
       "X-Request-Id: safe\r\n\r\nhello";
   const size_t length = sizeof(response) - 1;
   for (size_t split = 0; split <= length; split++)
   {
      capture_t capture = {.gate = KB_HTTP_GATE_DELIVER};
      assert(parse_parts(response, length, split, 64, &capture) == KB_HTTP_OK);
      assert(capture.headers_called == 1 && capture.response.status == 200);
      assert(capture.response.framing == KB_HTTP_FRAMING_CONTENT_LENGTH);
      assert(capture.response.content_length == 5);
      assert(strcmp(capture.response.content_type, "application/json") == 0);
      assert(capture.body_len == 5 && memcmp(capture.body, "hello", 5) == 0);
   }

   capture_t bytewise = {.gate = KB_HTTP_GATE_DELIVER};
   kb_http_response_parser_t *parser = NULL;
   assert(kb_http_response_parser_init(&parser, 64, capture_headers, capture_body, &bytewise) ==
          KB_HTTP_OK);
   for (size_t i = 0; i < length; i++)
      assert(kb_http_response_parser_feed(parser, response + i, 1) == KB_HTTP_MORE);
   assert(kb_http_response_parser_finish_eof(parser) == KB_HTTP_OK);
   assert(bytewise.body_len == 5 && bytewise.body_called == 5);
   kb_http_response_parser_free(&parser);
}

static void chunked_boundaries(void)
{
   static const unsigned char response[] =
       "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.amazon.eventstream\r\n"
       "Transfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
   const size_t length = sizeof(response) - 1;
   for (size_t split = 0; split <= length; split++)
   {
      capture_t capture = {.gate = KB_HTTP_GATE_DELIVER};
      assert(parse_parts(response, length, split, 64, &capture) == KB_HTTP_OK);
      assert(capture.response.framing == KB_HTTP_FRAMING_CHUNKED);
      assert(capture.response.content_length == 0);
      assert(capture.body_len == 9 && memcmp(capture.body, "Wikipedia", 9) == 0);
   }
}

static void gate_and_abort(void)
{
   static const unsigned char denied[] =
       "HTTP/1.1 429 Slow Down\r\nContent-Type: application/json\r\nContent-Length: 6\r\n\r\nsecret";
   capture_t discard = {.gate = KB_HTTP_GATE_DELIVER};
   assert(parse_parts(denied, sizeof(denied) - 1, sizeof(denied) - 1, 64, &discard) == KB_HTTP_OK);
   assert(discard.headers_called == 1 && discard.body_called == 0 && discard.body_len == 0);

   static const unsigned char okay[] =
       "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc";
   capture_t abort_headers = {.gate = KB_HTTP_GATE_ABORT};
   assert(parse_parts(okay, sizeof(okay) - 1, sizeof(okay) - 1, 64, &abort_headers) ==
          KB_HTTP_CALLBACK_ABORT);
   assert(abort_headers.body_called == 0);

   capture_t abort_body = {.gate = KB_HTTP_GATE_DELIVER, .abort_body = 1};
   kb_http_response_parser_t *parser = NULL;
   assert(kb_http_response_parser_init(&parser, 64, capture_headers, capture_body, &abort_body) ==
          KB_HTTP_OK);
   assert(kb_http_response_parser_feed(parser, okay, sizeof(okay) - 1) ==
          KB_HTTP_CALLBACK_ABORT);
   assert(kb_http_response_parser_feed(parser, NULL, 0) == KB_HTTP_CALLBACK_ABORT);
   assert(kb_http_response_parser_finish_eof(parser) == KB_HTTP_CALLBACK_ABORT);
   kb_http_response_parser_free(&parser);
}

static void post_completion_surplus(void)
{
   static const char *const complete[] = {
       "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx",
       "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx\r\n0\r\n\r\n"};
   for (size_t i = 0; i < sizeof(complete) / sizeof(complete[0]); i++)
   {
      capture_t capture = {.gate = KB_HTTP_GATE_DELIVER};
      kb_http_response_parser_t *parser = NULL;
      assert(kb_http_response_parser_init(&parser, 64, capture_headers, capture_body, &capture) ==
             KB_HTTP_OK);
      assert(kb_http_response_parser_feed(parser, (const unsigned char *)complete[i],
                                          strlen(complete[i])) == KB_HTTP_MORE);
      assert(kb_http_response_parser_feed(parser, (const unsigned char *)"x", 1) ==
             KB_HTTP_MALFORMED_RESPONSE);
      assert(kb_http_response_parser_finish_eof(parser) == KB_HTTP_MALFORMED_RESPONSE);
      kb_http_response_parser_free(&parser);
   }
}

static void malformed_matrix(void)
{
   static const char *const malformed[] = {
       "HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n",
       "HTTP/1.1 100 Continue\r\nContent-Length: 0\r\n\r\n",
       "HTTP/1.1 302 Found\r\nContent-Length: 0\r\n\r\n",
       "HTTP/1.1 200 OK\nContent-Length: 0\n\n",
       "HTTP/1.1 200 OK\r\n folded: bad\r\nContent-Length: 0\r\n\r\n",
       "HTTP/1.1 200 OK\r\nBad Name: x\r\nContent-Length: 0\r\n\r\n",
       "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n",
       "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n",
       "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n",
       "HTTP/1.1 200 OK\r\nContent-Type: a\r\nContent-Type: b\r\nContent-Length: 0\r\n\r\n",
       "HTTP/1.1 200 OK\r\nX: y\r\n\r\n",
       "HTTP/1.1 200 OK\r\nContent-Length: +1\r\n\r\nx",
       "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\n",
       "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nxy",
       "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1;x=y\r\na\r\n0\r\n\r\n",
       "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\naX0\r\n\r\n",
       "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n0\r\nX: y\r\n\r\n",
       "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n0\r\n",
       "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFFFFFFFFFF\r\n"};
   for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++)
   {
      capture_t capture = {.gate = KB_HTTP_GATE_DELIVER};
      size_t length = strlen(malformed[i]);
      kb_http_result_t result = parse_parts((const unsigned char *)malformed[i], length, length / 2,
                                            64, &capture);
      assert(result == KB_HTTP_MALFORMED_RESPONSE || result == KB_HTTP_TOO_LARGE);
   }

   static const unsigned char too_large[] =
       "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nxxxx";
   capture_t capture = {.gate = KB_HTTP_GATE_DELIVER};
   assert(parse_parts(too_large, sizeof(too_large) - 1, 0, 3, &capture) == KB_HTTP_TOO_LARGE);
}

static void request_validation(void)
{
   kb_http_header_t headers[] = {{"host", "bedrock-runtime.us-east-1.amazonaws.com"},
                                 {"content-type", "application/json"}};
   kb_http_request_t request = {.authority = "bedrock-runtime.us-east-1.amazonaws.com",
                                .method = "GET",
                                .target = "/model/x/converse",
                                .headers = headers,
                                .header_count = 2,
                                .body = (const unsigned char *)"{}",
                                .body_len = 2,
                                .response_body_max = 64,
                                .connect_timeout_ms = 1,
                                .total_timeout_ms = 1};
   kb_http_response_t response = {.status = 999};
   capture_t capture = {.gate = KB_HTTP_GATE_DELIVER};
   assert(kb_http_request_validate(&request) == KB_HTTP_INVALID_ARGUMENT);
   assert(kb_http_tls_exchange(&request, &response, capture_headers, capture_body, &capture) ==
          KB_HTTP_INVALID_ARGUMENT);
   assert(response.status == 0);
   request.method = "POST";
   request.target = "//authority-form";
   assert(kb_http_tls_exchange(&request, &response, capture_headers, capture_body, &capture) ==
          KB_HTTP_INVALID_ARGUMENT);
   request.target = "/x?query";
   assert(kb_http_tls_exchange(&request, &response, capture_headers, capture_body, &capture) ==
          KB_HTTP_INVALID_ARGUMENT);
   request.target = "/x";
   headers[1].name = "Content-Length";
   assert(kb_http_tls_exchange(&request, &response, capture_headers, capture_body, &capture) ==
          KB_HTTP_INVALID_ARGUMENT);
   headers[1] = headers[0];
   assert(kb_http_tls_exchange(&request, &response, capture_headers, capture_body, &capture) ==
          KB_HTTP_INVALID_ARGUMENT);
}

static void origin_path_validation(void)
{
   kb_http_header_t headers[] = {{"host", "bedrock-runtime.us-east-1.amazonaws.com"}};
   kb_http_request_t request = {.authority = headers[0].value,
                                .method = "POST",
                                .target = "/",
                                .headers = headers,
                                .header_count = 1,
                                .body = (const unsigned char *)"{}",
                                .body_len = 2,
                                .response_body_max = 64,
                                .connect_timeout_ms = 1,
                                .total_timeout_ms = 1};
   static const char *const valid[] = {"/", "/model/a%3Ab/converse",
                                       "/a/b:c@d!$&'()*+,;=-._~"};
   for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++)
   {
      request.target = valid[i];
      assert(kb_http_request_validate(&request) == KB_HTTP_OK);
   }
   char raw_non_ascii[] = {'/', 'x', (char)0xc3, (char)0xa9, 0};
   const char *invalid[] = {"//x", "/x\\y", "/x%", "/x%0", "/x%GG", "/x%0g",
                            "/x?y", "/x#y", "/x y", "/x\ty", raw_non_ascii};
   for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
   {
      request.target = invalid[i];
      assert(kb_http_request_validate(&request) == KB_HTTP_INVALID_ARGUMENT);
   }
}

int main(void)
{
   content_length_boundaries();
   chunked_boundaries();
   gate_and_abort();
   post_completion_surplus();
   malformed_matrix();
   request_validation();
   origin_path_validation();
   puts("kb http client: strict parser and request validation passed");
   return 0;
}
