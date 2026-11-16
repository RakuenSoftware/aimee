#ifndef ORIGINAL_INPUT_PLAN_H
#define ORIGINAL_INPUT_PLAN_H
#include "original_input_delivery.h"
#include "db1_client/original_input_plan.h"
/* Draft additive contract. A plan is operational state, never Human/Created
 * authority or model execution. The installed verifier must recognize only the
 * actual live private CONSUMING origin and restored authenticated context. */
typedef struct
{
   unsigned schema_version;
   const char *session, *operation_uuid, *principal;
} server_input_plan_owner_view_t;
typedef int (*server_input_plan_owner_check_fn)(const void *, server_input_plan_owner_view_t *,
                                                void *);
typedef struct server_input_pending_plan_cap server_input_pending_plan_cap_t;
typedef struct
{
   unsigned schema_version;
   db1_original_input_plan_receipt_t receipt;
   const char *opaque_plan;
   size_t opaque_plan_len;
} server_input_pending_plan_view_t;
/* A fixed private callback must verify an irreversible target-operation fence
 * (and all custody retired when acquired). Observed absence is NOT terminal.
 * The cap is documentary pending-plan state, never a live Human/Created cap. */
typedef int (*server_input_plan_terminal_fn)(const server_input_pending_plan_cap_t *, char **,
                                             void *);
int server_input_pending_plan_view(const server_input_pending_plan_cap_t *,
                                   server_input_pending_plan_view_t *);
int server_input_plan_conclude_v1(const char *, const char *, const char *);
/* All roles/callbacks fixed at trusted startup before capture. */
int server_input_delivery_install_owner_v2(const char *, const char *,
                                           server_input_prepare_delivery_fn,
                                           server_input_recover_delivery_fn,
                                           server_input_plan_owner_check_fn,
                                           server_input_plan_terminal_fn, void *);
/* No per-call session/actor/receiver selectors: verifier supplies actual origin,
 * installation supplies fixed actor/receiver. Probe is diagnostic, not custody
 * permission. Unknown/refusal clears outputs and must prevent Raw2. */
int server_input_plan_probe_v1(const void *, db1_original_input_probe_t *);
/* Actual consuming-owner cookie supplies session/actor. Fresh fenced recovery
 * must confirm the existing delivered input; output is cached receipt DATA.
 * No fresh canonical append or input-origin authority is granted. */
int server_input_plan_replay_receipt_v1(const void *, char **);

int server_input_plan_reserve_v1(const void *, const char *, const char *, const char *,
                                 db1_original_input_plan_receipt_t *);
/* Only within a genuine active prepared-cap callback;64 independently rechecks
 * the real row/owner and canonical TEXT commitment under one transaction. */
int server_input_prepared_bind_plan_v1(const server_input_prepared_cap_t *,
                                       const db1_original_input_plan_receipt_t *);
extern const unsigned aimee_original_input_safe_plan_contract_v1;
#endif
