#ifndef AIMEE_KB_MANAGEMENT_RUNTIME_H
#define AIMEE_KB_MANAGEMENT_RUNTIME_H

#include "kb_management_health_exchange.h"

#include <stdint.h>

typedef enum
{
   KB_MANAGEMENT_RUNTIME_DISABLED = 0,
   KB_MANAGEMENT_RUNTIME_RECONCILING,
   KB_MANAGEMENT_RUNTIME_READY,
   KB_MANAGEMENT_RUNTIME_RETRY_WAIT,
   KB_MANAGEMENT_RUNTIME_READY_DEGRADED,
   KB_MANAGEMENT_RUNTIME_TERMINAL,
   KB_MANAGEMENT_RUNTIME_STOPPING
} kb_management_runtime_state_t;

/* Parse the process environment, register the live health callback when the
 * complete packet is present, and perform the first bounded reconciliation.
 * An absent packet is a successful, explicitly disabled configuration. */
int kb_management_runtime_start(void);

/* Called from the kb service loop. It returns immediately when no reconcile is
 * due; a due reconcile has one absolute wall-clock deadline of now + 30. */
void kb_management_runtime_tick(int64_t now_epoch);

/* Must be called after kb_http_stop() has joined request workers. */
void kb_management_runtime_stop(void);

kb_management_runtime_state_t kb_management_runtime_state(void);

/* Pure schedule helper kept public so the capped sequence cannot drift from
 * its focused test. */
unsigned kb_management_runtime_retry_seconds(unsigned retry_index);

#endif
