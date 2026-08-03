#ifndef AIMEE_CORE_CONNECTION_CONTROL_H
#define AIMEE_CORE_CONNECTION_CONTROL_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef int (*aimee_core_cancel_fn)(void *context);

   typedef enum
   {
      AIMEE_CORE_OK = 0,
      AIMEE_CORE_EOF = 1,
      AIMEE_CORE_INVALID = -1,
      AIMEE_CORE_TIMEOUT = -2,
      AIMEE_CORE_CANCELLED = -3,
      AIMEE_CORE_IO_ERROR = -4,
      AIMEE_CORE_TLS_ERROR = -5
   } aimee_core_result_t;

   typedef enum
   {
      AIMEE_CORE_WAIT_READ = 1,
      AIMEE_CORE_WAIT_WRITE = 2
   } aimee_core_wait_t;

   /* One absolute monotonic deadline and optional cancellation source shared by
    * connect, TLS handshake, read, and write. deadline_ns == 0 means no deadline.
    * cancel_poll_ms bounds cancellation latency while an fd is idle. */
   typedef struct
   {
      int64_t deadline_ns;
      int cancel_poll_ms;
      aimee_core_cancel_fn cancelled;
      void *cancel_context;
   } aimee_core_control_t;

   int64_t aimee_core_now_ns(void);
   aimee_core_result_t aimee_core_control_init(aimee_core_control_t *control,
                                                int64_t deadline_ns, int cancel_poll_ms,
                                                aimee_core_cancel_fn cancelled,
                                                void *cancel_context);
   aimee_core_result_t aimee_core_control_init_timeout(aimee_core_control_t *control,
                                                        int timeout_ms, int cancel_poll_ms,
                                                        aimee_core_cancel_fn cancelled,
                                                        void *cancel_context);
   aimee_core_result_t aimee_core_control_check(const aimee_core_control_t *control);
   int aimee_core_control_remaining_ms(const aimee_core_control_t *control);
   aimee_core_result_t aimee_core_wait_fd(int fd, aimee_core_wait_t events,
                                           const aimee_core_control_t *control);

#ifdef __cplusplus
}
#endif

#endif
