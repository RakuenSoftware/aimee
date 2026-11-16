#ifndef DB1_ORIGINAL_INPUT_PLAN_H
#define DB1_ORIGINAL_INPUT_PLAN_H
#include "primary_sessions.h"
#include <stddef.h>
typedef enum
{
   DB1_INPUT_PLAN_UNKNOWN = 0,
   DB1_INPUT_PLAN_RESERVED = 1,
   DB1_INPUT_PLAN_COMMITTED = 2,
   DB1_INPUT_PLAN_TERMINAL = 3
} db1_original_input_plan_state_t;
typedef struct
{
   unsigned contract_version;
   uint64_t plan_id;
   db1_original_input_plan_state_t state;
   char session[1025], operation_uuid[37], actor[1025], receiver_tag[129];
   char safe_text_sha256[65], plan_sha256[65];
} db1_original_input_plan_receipt_t;
typedef enum
{
   DB1_INPUT_PROBE_UNKNOWN = 0,
   DB1_INPUT_PROBE_OWNED_ABSENT = 1,
   DB1_INPUT_PROBE_LEGACY = 2,
   DB1_INPUT_PROBE_DELIVERY = 3,
   DB1_INPUT_PROBE_RESERVED = 4,
   DB1_INPUT_PROBE_COMMITTED = 5,
   DB1_INPUT_PROBE_TERMINAL = 6
} db1_original_input_presence_t;
typedef struct
{
   unsigned contract_version;
   db1_original_input_presence_t presence;
   db1_original_input_receipt_t canonical;
   uint64_t delivery_id;
   db1_original_input_plan_receipt_t plan;
   char *opaque_plan, *terminal_result;
   size_t opaque_plan_len, terminal_result_len;
} db1_original_input_probe_t;
void db1_original_input_probe_destroy(db1_original_input_probe_t *);
#define DB1_INPUT_PLAN_CONFLICT (-2)
/* Probe output must be an empty slot or previously destroyed.
 * Actual Host-only DB1 operations; query fields never confer authorization.
 * Probe0 means a validated state, not necessarily absence. Missing session,
 * wrong owner, malformed, transport unknown return -1 with UNKNOWN output. */
int db1_session_original_input_probe(const char *, const char *, const char *,
                                     db1_original_input_probe_t *);
int db1_session_original_input_plan_reserve(const char *, const char *, const char *, const char *,
                                            const char *, const char *, const char *,
                                            db1_original_input_plan_receipt_t *);
int db1_session_original_input_begin_planned_delivery(const char *, const char *, const char *,
                                                      const char *, const char *, const char *,
                                                      const char *, const char *,
                                                      db1_original_input_receipt_t *, uint64_t *);
int db1_session_original_input_plan_terminal(const char *, const char *, const char *, const char *,
                                             const char *, const char *, const char *);
extern const unsigned aimee_original_input_safe_plan_client_contract_v1;
#endif
