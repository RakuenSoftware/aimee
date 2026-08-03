/* Shared core-side client for calls into local process modules.
 *
 * A daemon owns one of these over a dedicated event-bus client. The core, not
 * feature modules, owns correlation ids, AMOD envelopes, monotonic deadlines,
 * cancellation frames, stale-reply draining, and response validation.
 */
#ifndef AIMEE_CORE_EVENT_BUS_MODULE_CLIENT_H
#define AIMEE_CORE_EVENT_BUS_MODULE_CLIENT_H 1

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <aimee/core/event_bus/bus_client.h>

typedef enum
{
   AIMEE_MODULE_CALL_OK = 0,
   AIMEE_MODULE_CALL_CAPABILITY_ABSENT,
   AIMEE_MODULE_CALL_CAPABILITY_DENIED,
   AIMEE_MODULE_CALL_CANCELLED,
   AIMEE_MODULE_CALL_DEADLINE_EXCEEDED,
   AIMEE_MODULE_CALL_INVALID_REQUEST,
   AIMEE_MODULE_CALL_INTERNAL,
   AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE,
   AIMEE_MODULE_CALL_TRANSPORT,
   AIMEE_MODULE_CALL_PROTOCOL,
   AIMEE_MODULE_CALL_INVALID_ARGUMENT
} aimee_module_call_result_t;

typedef int (*aimee_module_cancelled_fn)(void *context);

typedef struct
{
   bus_client_t *bus;
   pthread_mutex_t lock;
   uint64_t next_correlation;
   int initialized;
} aimee_module_client_t;

/* The bus client is borrowed and must remain attached until destroy returns. */
int aimee_module_client_init(aimee_module_client_t *client, bus_client_t *bus);
void aimee_module_client_destroy(aimee_module_client_t *client);

/* Synchronously invoke one module stage. deadline_ns is an absolute
 * CLOCK_MONOTONIC deadline; zero disables the deadline. When cancellation or a
 * deadline fires, core publishes F_CANCEL and returns the typed outcome. A late
 * terminal reply is ignored by the next call through this dedicated client.
 *
 * response_len receives the complete returned body length. If it is greater
 * than response_capacity, no partial response is copied and the result is
 * RESPONSE_TOO_LARGE. */
aimee_module_call_result_t aimee_module_client_call(
    aimee_module_client_t *client, uint32_t event_kind, uint32_t stage_id, uint64_t trace_id,
    uint64_t deadline_ns, const void *request_body, uint32_t request_len, void *response_body,
    uint32_t response_capacity, uint32_t *response_len, aimee_module_cancelled_fn cancelled,
    void *cancel_context);

const char *aimee_module_call_result_name(aimee_module_call_result_t result);

#endif /* AIMEE_CORE_EVENT_BUS_MODULE_CLIENT_H */
