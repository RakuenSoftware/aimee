#include "mock_agent_http.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static mock_agent_http_stream_handler_t g_stream_handler = NULL;
static mock_agent_http_post_handler_t g_post_handler = NULL;
static mock_agent_http_get_handler_t g_get_handler = NULL;

void mock_agent_http_reset(void)
{
   g_stream_handler = NULL;
   g_post_handler = NULL;
   g_get_handler = NULL;
}

void mock_agent_http_set_stream_handler(mock_agent_http_stream_handler_t handler)
{
   g_stream_handler = handler;
}

void mock_agent_http_set_post_handler(mock_agent_http_post_handler_t handler)
{
   g_post_handler = handler;
}

void mock_agent_http_set_get_handler(mock_agent_http_get_handler_t handler)
{
   g_get_handler = handler;
}

int agent_http_get_stream(const char *url, const char *extra_headers, agent_http_stream_cb callback,
                          void *userdata, int timeout_ms)
{
   if (!g_stream_handler)
      return -1;
   return g_stream_handler(url, extra_headers, callback, userdata, timeout_ms);
}

int agent_http_get(const char *url, const char *extra_headers, char **response_buf, int timeout_ms)
{
   if (response_buf)
      *response_buf = NULL;
   if (g_get_handler)
      return g_get_handler(url, extra_headers, response_buf, timeout_ms);
   if (url && (strstr(url, "/health") != NULL || strstr(url, "/collections/") != NULL))
   {
      if (response_buf)
         *response_buf = strdup("{\"status\":\"ok\"}");
      return 200;
   }
   (void)url;
   return -1;
}

int agent_http_put(const char *url, const char *auth_header, const char *body, char **response_buf,
                   int timeout_ms, const char *extra_headers)
{
   (void)auth_header;
   (void)body;
   (void)timeout_ms;
   (void)extra_headers;
   if (response_buf)
      *response_buf = NULL;
   /* Legacy stub for HTTP /collections/ vector calls (pre-pgvector cutover);
    * kept harmless so any retrieval test that still exercises that path
    * doesn't fail on a missing handler. */
   if (url && strstr(url, "/collections/") != NULL)
   {
      if (response_buf)
         *response_buf = strdup("{\"result\":{\"status\":\"ok\"},\"status\":\"ok\",\"time\":0.0}");
      return 200;
   }
   return -1;
}

int agent_http_patch(const char *url, const char *auth_header, const char *body,
                     char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)timeout_ms;
   (void)extra_headers;
   if (response_buf)
      *response_buf = NULL;
   return -1;
}

int agent_http_post(const char *url, const char *auth_header, const char *body, char **response_buf,
                    int timeout_ms, const char *extra_headers)
{
   if (response_buf)
      *response_buf = NULL;
   if (g_post_handler)
      return g_post_handler(url, auth_header, body, response_buf, timeout_ms, extra_headers);
   /* Default stub: return an empty-but-well-formed vector response for search
    * and upsert endpoints so tests that don't register a post handler can still
    * exercise the retrieval codepath (semantic returns 0 hits → code falls
    * back to LIKE/FTS matches backed by SQLite). */
   if (url && strstr(url, "/collections/") != NULL)
   {
      if (response_buf)
         *response_buf = strdup("{\"result\":[],\"status\":\"ok\",\"time\":0.0}");
      return 200;
   }
   return -1;
}

/* Legacy fixtures provide text JSON handlers. Preserve their exact bytes when
 * an adapter uses the explicit-length transport; binary fixtures supply their
 * own byte handler rather than silently losing NUL-delimited content here. */
int agent_http_post_content_type_bytes(const char *url, const char *auth_header,
                                       const char *content_type, const void *body, size_t length,
                                       char **response_buf, size_t *response_length, int timeout_ms,
                                       const char *extra_headers)
{
   (void)content_type;
   if (response_buf)
      *response_buf = NULL;
   if (response_length)
      *response_length = 0;
   if (!response_buf || !response_length || (!body && length) || length == SIZE_MAX ||
       (length && memchr(body, '\0', length)))
      return -1;
   char *text = malloc(length + 1);
   if (!text)
      return -1;
   if (length)
      memcpy(text, body, length);
   text[length] = '\0';
   int status = agent_http_post(url, auth_header, text, response_buf, timeout_ms, extra_headers);
   free(text);
   if (status < 0)
   {
      free(*response_buf);
      *response_buf = NULL;
   }
   else if (*response_buf)
      *response_length = strlen(*response_buf);
   return status;
}

int agent_http_delete(const char *url, const char *auth_header, int timeout_ms)
{
   (void)url;
   (void)auth_header;
   (void)timeout_ms;
   return -1;
}
