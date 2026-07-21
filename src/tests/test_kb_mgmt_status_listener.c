#include "kb_mgmt_status_listener.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static kb_mgmt_status_http_result_t parse(const char *request)
{
   const char *body = (const char *)1;
   size_t body_len = 99;
   return kb_mgmt_status_http_parse((const unsigned char *)request, strlen(request), &body,
                                    &body_len);
}

int main(void)
{
   static const char good[] = "POST /v1/management/status HTTP/1.1\r\nHost: authority\r\n"
                              "Content-Type: application/json\r\nContent-Length: 2\r\n\r\n{}";
   const char *body = NULL;
   size_t body_len = 0;
   assert(kb_mgmt_status_http_parse((const unsigned char *)good, strlen(good), &body, &body_len) ==
          KB_MGMT_STATUS_HTTP_COMPLETE);
   assert(body_len == 2 && !memcmp(body, "{}", 2));
   for (size_t i = 0; i < strlen(good); ++i)
   {
      body = (const char *)1;
      body_len = 99;
      assert(kb_mgmt_status_http_parse((const unsigned char *)good, i, &body, &body_len) ==
             KB_MGMT_STATUS_HTTP_MORE);
      assert(body == NULL && body_len == 0);
   }

   assert(parse("GET /v1/management/status HTTP/1.1\r\nHost:x\r\nContent-Type: "
                "application/json\r\nContent-Length:0\r\n\r\n") == KB_MGMT_STATUS_HTTP_BAD);
   assert(parse("POST /v1/management/status HTTP/1.1\nHost:x\n\n") == KB_MGMT_STATUS_HTTP_BAD);
   assert(parse("POST /v1/management/status HTTP/1.1\r\nHost:x\r\nHost:y\r\nContent-Type: "
                "application/json\r\nContent-Length:0\r\n\r\n") == KB_MGMT_STATUS_HTTP_BAD);
   assert(parse("POST /v1/management/status HTTP/1.1\r\nHost:x\r\nContent-Type: "
                "application/json\r\nContent-Length:00\r\n\r\n") == KB_MGMT_STATUS_HTTP_BAD);
   assert(parse("POST /v1/management/status HTTP/1.1\r\nHost:x\r\nContent-Type: "
                "application/json\r\nContent-Length:0\r\nTransfer-Encoding: chunked\r\n\r\n") ==
          KB_MGMT_STATUS_HTTP_BAD);
   assert(parse("POST /v1/management/status HTTP/1.1\r\nHost:x\r\nContent-Type: "
                "application/json\r\nContent-Length:0\r\nConnection: keep-alive\r\n\r\n") ==
          KB_MGMT_STATUS_HTTP_BAD);
   assert(parse("POST /v1/management/status HTTP/1.1\r\nHost:x\r\nContent-Type: "
                "application/json\r\nContent-Length:0\r\nUpgrade: h2c\r\n\r\n") ==
          KB_MGMT_STATUS_HTTP_BAD);
   assert(parse("POST /v1/management/status HTTP/1.1\r\n Host:x\r\nContent-Type: "
                "application/json\r\nContent-Length:0\r\n\r\n") == KB_MGMT_STATUS_HTTP_BAD);
   assert(parse("POST /v1/management/status HTTP/1.1\r\nHost:x\r\nContent-Type: "
                "application/json\r\nContent-Length:1\r\n\r\n{}") == KB_MGMT_STATUS_HTTP_BAD);

   char large[KB_MGMT_STATUS_HTTP_HEADER_MAX + 16];
   memset(large, 'A', sizeof(large));
   body = NULL;
   body_len = 0;
   assert(kb_mgmt_status_http_parse((const unsigned char *)large, sizeof(large), &body,
                                    &body_len) == KB_MGMT_STATUS_HTTP_TOO_LARGE);
   puts("kb_mgmt_status_listener: all tests passed");
   return 0;
}
