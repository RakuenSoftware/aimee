/* Versioned payload envelope for core <-> process-module calls.
 *
 * The event-bus frame carries routing, correlation, and cancellation.  This
 * envelope describes the operation inside each inline payload. Bodies larger
 * than the negotiated inline budget use ordered BUS_F_MORE fragments under one
 * correlation. It deliberately
 * contains no pointers, native structs, or host-endian fields, so C, Go, and
 * future module SDKs can implement the same contract independently.
 */
#ifndef AIMEE_CORE_EVENT_BUS_MODULE_PROTOCOL_H
#define AIMEE_CORE_EVENT_BUS_MODULE_PROTOCOL_H 1

#include <stddef.h>
#include <stdint.h>

#define AIMEE_MODULE_MESSAGE_MAGIC 0x444f4d41u /* "AMOD", little-endian */
#define AIMEE_MODULE_MESSAGE_VERSION 1u
#define AIMEE_MODULE_MESSAGE_HEADER_LEN 40u
#define AIMEE_MODULE_MESSAGE_MAX_BODY (16u * 1024u * 1024u)

typedef enum
{
   AIMEE_MODULE_OP_INVOKE = 1,
   AIMEE_MODULE_OP_RESULT = 2
} aimee_module_operation_t;

typedef enum
{
   AIMEE_MODULE_STATUS_OK = 0,
   AIMEE_MODULE_STATUS_CAPABILITY_ABSENT = 1,
   AIMEE_MODULE_STATUS_CANCELLED = 2,
   AIMEE_MODULE_STATUS_DEADLINE_EXCEEDED = 3,
   AIMEE_MODULE_STATUS_INVALID_REQUEST = 4,
   AIMEE_MODULE_STATUS_INTERNAL = 5
} aimee_module_status_t;

typedef enum
{
   AIMEE_MODULE_MESSAGE_OK = 0,
   AIMEE_MODULE_MESSAGE_ERR_SHORT,
   AIMEE_MODULE_MESSAGE_ERR_MAGIC,
   AIMEE_MODULE_MESSAGE_ERR_VERSION,
   AIMEE_MODULE_MESSAGE_ERR_HEADER,
   AIMEE_MODULE_MESSAGE_ERR_OPERATION,
   AIMEE_MODULE_MESSAGE_ERR_STATUS,
   AIMEE_MODULE_MESSAGE_ERR_STAGE,
   AIMEE_MODULE_MESSAGE_ERR_FLAGS,
   AIMEE_MODULE_MESSAGE_ERR_BODY
} aimee_module_message_result_t;

typedef struct
{
   uint16_t operation;
   uint16_t status;
   uint32_t stage_id;
   uint32_t flags;
   uint32_t body_len;
   uint64_t deadline_ns; /* absolute CLOCK_MONOTONIC time; zero means none */
   uint64_t trace_id;
} aimee_module_message_t;

/* Encode/decode the fixed header. The body immediately follows the returned
 * header bytes and is owned by the caller/event frame. Decode validates that
 * the complete declared body is present in input_len. */
size_t aimee_module_message_encode(const aimee_module_message_t *message, uint8_t *output,
                                   size_t output_len);
aimee_module_message_result_t aimee_module_message_decode(const uint8_t *input,
                                                          size_t input_len,
                                                          aimee_module_message_t *message);

/* Deadlines and cancellation are core transport concerns. A runtime checks an
 * invoke before dispatch and uses the bus frame's F_CANCEL for cancellation. */
int aimee_module_deadline_expired(uint64_t deadline_ns, uint64_t now_ns);
const char *aimee_module_message_result_name(aimee_module_message_result_t result);

#endif
