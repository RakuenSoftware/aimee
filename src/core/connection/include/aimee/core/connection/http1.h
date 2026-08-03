#ifndef AIMEE_CORE_CONNECTION_HTTP1_H
#define AIMEE_CORE_CONNECTION_HTTP1_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef long (*aimee_core_http1_read_fn)(void *context, void *buffer, size_t length);
   typedef int (*aimee_core_http1_write_all_fn)(void *context, const void *buffer, size_t length);

   typedef struct
   {
      void *context;
      aimee_core_http1_read_fn read;
      aimee_core_http1_write_all_fn write_all;
   } aimee_core_http1_io_t;

   typedef struct
   {
      char *data;
      size_t length;
      size_t header_length;
      size_t content_length;
      int has_content_length;
      int connection_close;
      int status;
   } aimee_core_http1_response_t;

   /* Read one HTTP/1.x response into a bounded NUL-terminated allocation.
    * Content-Length responses stop exactly at the declared body end, making
    * persistent connections safe. A close-delimited response is accepted only
    * when require_content_length is zero. Transfer-Encoding is rejected; the
    * Aimee C service protocol uses explicit Content-Length framing. */
   int aimee_core_http1_response_read(const aimee_core_http1_io_t *io, size_t header_max,
                                      size_t response_max, int require_content_length,
                                      aimee_core_http1_response_t *response);

   /* Send request bytes and read one response through the same transport. */
   int aimee_core_http1_exchange(const aimee_core_http1_io_t *io, const void *request,
                                 size_t request_length, size_t header_max, size_t response_max,
                                 int require_content_length, aimee_core_http1_response_t *response);

   void aimee_core_http1_response_free(aimee_core_http1_response_t *response);

#ifdef __cplusplus
}
#endif

#endif
