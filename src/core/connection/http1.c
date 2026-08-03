#include <aimee/core/connection/http1.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int ascii_equal_ci(const char *left, const char *right, size_t length)
{
   for (size_t i = 0; i < length; i++)
   {
      unsigned char a = (unsigned char)left[i], b = (unsigned char)right[i];
      if (a >= 'A' && a <= 'Z')
         a = (unsigned char)(a + ('a' - 'A'));
      if (b >= 'A' && b <= 'Z')
         b = (unsigned char)(b + ('a' - 'A'));
      if (a != b)
         return 0;
   }
   return 1;
}

static int header_token(const char *start, const char *end, const char *token)
{
   size_t token_length = strlen(token);
   while (start < end)
   {
      while (start < end && (*start == ' ' || *start == '\t' || *start == ','))
         start++;
      const char *item_end = start;
      while (item_end < end && *item_end != ',')
         item_end++;
      const char *trimmed_end = item_end;
      while (trimmed_end > start && (trimmed_end[-1] == ' ' || trimmed_end[-1] == '\t'))
         trimmed_end--;
      if ((size_t)(trimmed_end - start) == token_length &&
          ascii_equal_ci(start, token, token_length))
         return 1;
      start = item_end < end ? item_end + 1 : end;
   }
   return 0;
}

static int response_headers_parse(aimee_core_http1_response_t *response)
{
   const char *data = response->data;
   const char *headers_end = data + response->header_length - 2;
   const char *line_end = strstr(data, "\r\n");
   if (!line_end || line_end >= headers_end || line_end - data < 12 ||
       strncmp(data, "HTTP/1.", 7) != 0 || data[7] < '0' || data[7] > '9' || data[8] != ' ' ||
       data[9] < '1' || data[9] > '5' || data[10] < '0' || data[10] > '9' || data[11] < '0' ||
       data[11] > '9')
      return -1;
   response->status = (data[9] - '0') * 100 + (data[10] - '0') * 10 + data[11] - '0';

   int content_length_seen = 0;
   const char *line = line_end + 2;
   while (line < headers_end)
   {
      const char *next = strstr(line, "\r\n");
      if (!next || next > headers_end || next == line || line[0] == ' ' || line[0] == '\t')
         return -1;
      const char *colon = memchr(line, ':', (size_t)(next - line));
      if (!colon || colon == line)
         return -1;
      for (const char *p = line; p < colon; p++)
      {
         unsigned char c = (unsigned char)*p;
         if (c <= 0x20 || c >= 0x7f || c == ':')
            return -1;
      }
      for (const char *p = colon + 1; p < next; p++)
         if (((unsigned char)*p < 0x20 && *p != '\t') || (unsigned char)*p == 0x7f)
            return -1;
      size_t name_length = (size_t)(colon - line);
      const char *value = colon + 1;
      while (value < next && (*value == ' ' || *value == '\t'))
         value++;
      if (name_length == sizeof("transfer-encoding") - 1 &&
          ascii_equal_ci(line, "transfer-encoding", name_length))
         return -1;
      if (name_length == sizeof("content-length") - 1 &&
          ascii_equal_ci(line, "content-length", name_length))
      {
         if (content_length_seen++ || value == next)
            return -1;
         size_t parsed = 0;
         for (const char *p = value; p < next; p++)
         {
            if (*p < '0' || *p > '9' || parsed > (SIZE_MAX - (size_t)(*p - '0')) / (size_t)10)
               return -1;
            parsed = parsed * 10 + (size_t)(*p - '0');
         }
         response->content_length = parsed;
         response->has_content_length = 1;
      }
      if (name_length == sizeof("connection") - 1 &&
          ascii_equal_ci(line, "connection", name_length) && header_token(value, next, "close"))
         response->connection_close = 1;
      line = next + 2;
   }
   return 0;
}

static int buffer_grow(char **buffer, size_t *capacity, size_t required, size_t maximum)
{
   if (required > maximum + 1)
      return -1;
   size_t grown_capacity = *capacity;
   while (grown_capacity < required)
   {
      if (grown_capacity > (maximum + 1) / 2)
      {
         grown_capacity = maximum + 1;
         break;
      }
      grown_capacity *= 2;
   }
   char *grown = realloc(*buffer, grown_capacity);
   if (!grown)
      return -1;
   *buffer = grown;
   *capacity = grown_capacity;
   return 0;
}

int aimee_core_http1_response_read(const aimee_core_http1_io_t *io, size_t header_max,
                                   size_t response_max, int require_content_length,
                                   aimee_core_http1_response_t *response)
{
   if (response)
      memset(response, 0, sizeof(*response));
   if (!io || !io->read || !response || header_max < 16 || response_max < header_max ||
       response_max == SIZE_MAX)
      return -1;
   size_t capacity = response_max < 8191 ? response_max + 1 : 8192;
   char *buffer = malloc(capacity);
   if (!buffer)
      return -1;
   size_t length = 0, expected = 0;
   for (;;)
   {
      if (response->has_content_length && length == expected)
         break;
      if (length >= response_max)
         goto fail;
      size_t wanted = response->has_content_length ? expected - length : 4096;
      if (wanted > response_max - length)
         wanted = response_max - length;
      if (buffer_grow(&buffer, &capacity, length + wanted + 1, response_max) != 0)
         goto fail;
      long received = io->read(io->context, buffer + length, wanted);
      if (received < 0 || (size_t)received > wanted)
         goto fail;
      if (received == 0)
         break;
      length += (size_t)received;
      buffer[length] = '\0';
      if (!response->header_length)
      {
         char *terminator = strstr(buffer, "\r\n\r\n");
         if (terminator)
         {
            response->data = buffer;
            response->length = length;
            response->header_length = (size_t)(terminator + 4 - buffer);
            if (response->header_length > header_max || response_headers_parse(response) != 0 ||
                (require_content_length && !response->has_content_length) ||
                (response->has_content_length &&
                 response->content_length > response_max - response->header_length))
               goto fail;
            if (response->has_content_length)
            {
               expected = response->header_length + response->content_length;
               if (length > expected)
                  goto fail;
            }
         }
         else if (length >= header_max)
            goto fail;
      }
   }
   if (!response->header_length || (response->has_content_length &&
                                    length != response->header_length + response->content_length))
      goto fail;
   response->data = buffer;
   response->length = length;
   buffer[length] = '\0';
   return 0;

fail:
   free(buffer);
   memset(response, 0, sizeof(*response));
   return -1;
}

int aimee_core_http1_exchange(const aimee_core_http1_io_t *io, const void *request,
                              size_t request_length, size_t header_max, size_t response_max,
                              int require_content_length, aimee_core_http1_response_t *response)
{
   if (!io || !io->write_all || (!request && request_length) ||
       io->write_all(io->context, request, request_length) != 0)
   {
      if (response)
         memset(response, 0, sizeof(*response));
      return -1;
   }
   return aimee_core_http1_response_read(io, header_max, response_max, require_content_length,
                                         response);
}

void aimee_core_http1_response_free(aimee_core_http1_response_t *response)
{
   if (!response)
      return;
   free(response->data);
   memset(response, 0, sizeof(*response));
}
