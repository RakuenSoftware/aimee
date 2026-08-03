/* Shared process-module runtime.
 *
 * Modules provide only a stage table and a handler. The core owns attach,
 * envelope validation, deadlines, cancellation, correlation-preserving replies,
 * heartbeats, and shutdown.
 */
#ifndef AIMEE_CORE_EVENT_BUS_MODULE_RUNTIME_H
#define AIMEE_CORE_EVENT_BUS_MODULE_RUNTIME_H 1

#include <stddef.h>
#include <stdint.h>

#include <aimee/core/event_bus/module_protocol.h>

typedef struct
{
   uint32_t event_kind;
   uint32_t stage_id;
} aimee_module_stage_t;

typedef struct
{
   uint32_t stage_id;
   uint64_t deadline_ns;
   uint64_t trace_id;
   const void *runtime_state;
} aimee_module_invocation_t;

/* A handler writes at most response_capacity bytes and sets response_len. It
 * should call aimee_module_invocation_cancelled() around bounded units of work.
 * The runtime checks cancellation and deadline again before publishing. */
typedef aimee_module_status_t (*aimee_module_handler_fn)(
    const aimee_module_invocation_t *invocation, const uint8_t *request_body, uint32_t request_len,
    uint8_t *response_body, uint32_t response_capacity, uint32_t *response_len, void *user_data);

typedef struct
{
   const char *socket_path;
   const char *module_name;
   uint32_t principal_class;
   uint32_t principal_ref;
   const aimee_module_stage_t *stages;
   size_t stage_count;
   aimee_module_handler_fn handler;
   void *user_data;
} aimee_module_process_config_t;

/* True after a bus cancel for this correlation or once its absolute monotonic
 * deadline expires. Module code never owns clocks or transport cancellation. */
int aimee_module_invocation_cancelled(const aimee_module_invocation_t *invocation);

/* Run until SIGINT/SIGTERM, host epoch change, or an attach error. */
int aimee_module_process_run(const aimee_module_process_config_t *config);

/* Request an orderly stop without a process signal (also useful in tests). */
void aimee_module_process_stop(void);

#endif /* AIMEE_CORE_EVENT_BUS_MODULE_RUNTIME_H */
