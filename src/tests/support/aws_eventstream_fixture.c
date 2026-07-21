#include "tests/support/aws_eventstream_fixture.h"

#include "modules/aws/aws_eventstream.h"

#include <stdlib.h>
#include <string.h>

static void put_u16(uint8_t *p, uint16_t v)
{
   p[0] = (uint8_t)(v >> 8);
   p[1] = (uint8_t)v;
}

static void put_u32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)(v >> 24);
   p[1] = (uint8_t)(v >> 16);
   p[2] = (uint8_t)(v >> 8);
   p[3] = (uint8_t)v;
}

static void put_u64(uint8_t *p, uint64_t v)
{
   for (int i = 0; i < 8; i++)
      p[i] = (uint8_t)(v >> (56 - 8 * i));
}

static int header_value_size(const test_aws_es_header_t *h, size_t *size)
{
   if (!h || !h->name || !h->name[0] || strlen(h->name) > UINT8_MAX)
      return -1;
   size_t value = 0;
   switch (h->value_type)
   {
   case AWS_ES_HDR_BOOL_TRUE:
   case AWS_ES_HDR_BOOL_FALSE:
      break;
   case AWS_ES_HDR_INT8:
      value = 1;
      break;
   case AWS_ES_HDR_INT16:
      value = 2;
      break;
   case AWS_ES_HDR_INT32:
      value = 4;
      break;
   case AWS_ES_HDR_INT64:
   case AWS_ES_HDR_TIMESTAMP:
      value = 8;
      break;
   case AWS_ES_HDR_BYTES:
   case AWS_ES_HDR_STRING:
      if (h->value_len > UINT16_MAX || (h->value_len && !h->value))
         return -1;
      value = 2U + h->value_len;
      break;
   case AWS_ES_HDR_UUID:
      if (!h->value || h->value_len != 16)
         return -1;
      value = 16;
      break;
   default:
      return -1;
   }
   *size = 1U + strlen(h->name) + 1U + value;
   return 0;
}

static uint8_t *put_header(uint8_t *p, const test_aws_es_header_t *h)
{
   size_t name_len = strlen(h->name);
   *p++ = (uint8_t)name_len;
   memcpy(p, h->name, name_len);
   p += name_len;
   *p++ = h->value_type;
   switch (h->value_type)
   {
   case AWS_ES_HDR_BOOL_TRUE:
   case AWS_ES_HDR_BOOL_FALSE:
      break;
   case AWS_ES_HDR_INT8:
      *p++ = (uint8_t)h->int_value;
      break;
   case AWS_ES_HDR_INT16:
      put_u16(p, (uint16_t)h->int_value);
      p += 2;
      break;
   case AWS_ES_HDR_INT32:
      put_u32(p, (uint32_t)h->int_value);
      p += 4;
      break;
   case AWS_ES_HDR_INT64:
   case AWS_ES_HDR_TIMESTAMP:
      put_u64(p, (uint64_t)h->int_value);
      p += 8;
      break;
   case AWS_ES_HDR_BYTES:
   case AWS_ES_HDR_STRING:
      put_u16(p, (uint16_t)h->value_len);
      p += 2;
      if (h->value_len)
      {
         memcpy(p, h->value, h->value_len);
         p += h->value_len;
      }
      break;
   case AWS_ES_HDR_UUID:
      memcpy(p, h->value, 16);
      p += 16;
      break;
   }
   return p;
}

int test_aws_es_message(const test_aws_es_header_t *headers, size_t n_headers, const void *payload,
                        size_t payload_len, uint8_t **out, size_t *out_len)
{
   if ((!headers && n_headers) || (payload_len && !payload) || !out || *out || !out_len)
      return -1;
   size_t header_len = 0;
   for (size_t i = 0; i < n_headers; i++)
   {
      size_t one = 0;
      if (header_value_size(&headers[i], &one) != 0 || header_len > UINT32_MAX - one)
         return -1;
      header_len += one;
   }
   if (payload_len > UINT32_MAX || header_len + payload_len > UINT32_MAX - 16U)
      return -1;
   size_t total = 16U + header_len + payload_len;
   uint8_t *frame = malloc(total);
   if (!frame)
      return -1;
   put_u32(frame, (uint32_t)total);
   put_u32(frame + 4, (uint32_t)header_len);
   put_u32(frame + 8, aws_es_crc32(frame, 8));
   uint8_t *p = frame + 12;
   for (size_t i = 0; i < n_headers; i++)
      p = put_header(p, &headers[i]);
   if (payload_len)
   {
      memcpy(p, payload, payload_len);
      p += payload_len;
   }
   put_u32(p, aws_es_crc32(frame, total - 4U));
   *out = frame;
   *out_len = total;
   return 0;
}

static test_aws_es_header_t string_header(const char *name, const char *value)
{
   test_aws_es_header_t h = {
       .name = name, .value_type = AWS_ES_HDR_STRING, .value = value, .value_len = strlen(value)};
   return h;
}

int test_aws_es_event(const char *event_type, const char *payload, uint8_t **out, size_t *out_len)
{
   if (!event_type || !event_type[0] || !payload)
      return -1;
   test_aws_es_header_t h[] = {string_header(":message-type", "event"),
                               string_header(":event-type", event_type),
                               string_header(":content-type", "application/json")};
   return test_aws_es_message(h, sizeof(h) / sizeof(h[0]), payload, strlen(payload), out, out_len);
}

int test_aws_es_exception(const char *exception_type, const char *payload, uint8_t **out,
                          size_t *out_len)
{
   if (!exception_type || !exception_type[0] || !payload)
      return -1;
   test_aws_es_header_t h[] = {string_header(":message-type", "exception"),
                               string_header(":exception-type", exception_type),
                               string_header(":content-type", "application/json")};
   return test_aws_es_message(h, sizeof(h) / sizeof(h[0]), payload, strlen(payload), out, out_len);
}

int test_aws_es_error(const char *code, const char *message, uint8_t **out, size_t *out_len)
{
   if (!code || !code[0] || !message || !message[0])
      return -1;
   test_aws_es_header_t h[] = {string_header(":message-type", "error"),
                               string_header(":error-code", code),
                               string_header(":error-message", message)};
   return test_aws_es_message(h, sizeof(h) / sizeof(h[0]), NULL, 0, out, out_len);
}

void test_aws_es_fixture_free(uint8_t **buffer, size_t length)
{
   if (!buffer || !*buffer)
      return;
   memset(*buffer, 0, length);
   free(*buffer);
   *buffer = NULL;
}
