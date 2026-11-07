#ifndef ORIGINAL_INPUT_DELIVERY_H
#define ORIGINAL_INPUT_DELIVERY_H
#include "original_input_created.h"
#include <stdint.h>
/* Distinct thread-local capabilities: prepared is not committed, and recovered
 * delivery is never a fresh Created capability or execution permission. */
typedef struct server_input_prepared_cap server_input_prepared_cap_t;
typedef struct server_input_committed_delivery_cap server_input_committed_delivery_cap_t;
typedef struct
{
   unsigned schema_version;
   const char *session, *principal, *execution_session, *operation_uuid, *proposed_turn;
   const char *text, *opaque, *text_sha256, *opaque_sha256;
   size_t text_len, opaque_len;
   const void *owner_cookie;
} server_input_prepared_view_t;
typedef struct
{
   unsigned schema_version;
   uint64_t delivery_id;
   const char *receiver_tag, *intent_key, *opaque_intent;
   size_t opaque_intent_len;
   db1_original_input_receipt_t receipt;
} server_input_created_delivery_view_t;
typedef struct
{
   unsigned schema_version;
   uint64_t delivery_id;
   const char *session, *principal, *operation_uuid, *original_turn;
   const char *receiver_tag, *intent_key, *opaque_intent;
   size_t opaque_intent_len;
   const char *text, *opaque, *text_sha256, *opaque_sha256;
   size_t text_len, opaque_len;
   db1_original_input_receipt_t receipt;
} server_input_committed_delivery_view_t;
/* Callbacks are synchronous and must return normally: no retained cap/view,
 * longjmp, pthread_exit, or cancellation-state/type changes. Prepare runs under
 * the capture writer mutex after owner-admit, and must remain in-memory/lock-free:
 * no capture-locking APIs, file/network I/O or Native calls. Created and recovery
 * callbacks run outside that mutex; cancellation is deferred until cleanup.
 * Prepare returns owned closed delivery JSON. Recover returns an owned opaque
 * documentary result only when the target operation is durably delivered or
 * terminal. A negative result is unknown: no generic acknowledgment occurs. */
typedef int (*server_input_prepare_delivery_fn)(const server_input_prepared_cap_t *, char **,
                                                void *);
typedef enum
{
   SERVER_INPUT_DELIVERY_UNKNOWN = -1,
   SERVER_INPUT_DELIVERY_DELIVERED = 0,
   SERVER_INPUT_DELIVERY_TERMINAL = 1
} server_input_delivery_outcome_t;
typedef server_input_delivery_outcome_t (*server_input_recover_delivery_fn)(
    const server_input_committed_delivery_cap_t *, char **, void *);
/* Trusted startup installation only, one immutable receiver/actor binding.
 * No callback, actor, receiver or subject can be selected by Model arguments. */
int server_input_delivery_install_owner_v1(const char *, const char *,
                                           server_input_prepare_delivery_fn,
                                           server_input_recover_delivery_fn, void *);
int server_input_prepared_view(const server_input_prepared_cap_t *, server_input_prepared_view_t *);
int server_input_created_delivery_view(const server_input_created_cap_t *,
                                       server_input_created_delivery_view_t *);
int server_input_created_delivery_ack_v1(const server_input_created_cap_t *, const char *);
int server_input_committed_delivery_view(const server_input_committed_delivery_cap_t *,
                                         server_input_committed_delivery_view_t *);
/* Immutable cached INPUT result only for the actual current callback cap/thread.
 * Never new source/actor authority; NULL for pending/background recovery. */
const char *
server_input_committed_delivery_completed_receipt_v1(const server_input_committed_delivery_cap_t *);

/* Explicit modification/recovery entry points. Zero means missing/no callback;
 * positive means a documentary terminal/delivered result, never model execution.
 * Unknown/refusal remains negative and leaves the obligation untouched. */
int server_input_delivery_reconcile_one_v1(const char *, const char *);
int server_input_delivery_reconcile_page_v1(uint64_t, unsigned, size_t, uint64_t *);
extern const unsigned aimee_original_input_durable_delivery_contract_v1;
#endif
