/* turn_integrity.h -- protocol-neutral contracts for one Aimee turn.
 *
 * This module owns data and transition rules only. It deliberately has no
 * dependency on the server, DB1/DB2, the event bus, or a provider. Shipping
 * daemons install the optional event callback at startup; other binaries get a
 * deterministic, dependency-free contract core. */
#ifndef AIMEE_CORE_TURN_INTEGRITY_H
#define AIMEE_CORE_TURN_INTEGRITY_H 1

#include <stddef.h>
#include <stdint.h>

struct cJSON;

#define TI_ID_MAX        128
#define TI_PRINCIPAL_MAX 128
#define TI_REVISION_MAX  96
#define TI_EVENT_MAX     48
#define TI_DETAIL_MAX    384
#define TI_DOMAIN_MAX    48
#define TI_SCOPE_MAX     128
#define TI_TOOL_MAX      96
#define TI_DIGEST_MAX    65

typedef enum
{
   TI_TURN_RECEIVED = 0,
   TI_TURN_CONTEXTUALIZED,
   TI_TURN_CONTRACTED,
   TI_TURN_AUTHORIZED,
   TI_TURN_EXECUTING,
   TI_TURN_VERIFYING,
   TI_TURN_REVIEWING,
   TI_TURN_COMPLETED,
   TI_TURN_BLOCKED,
   TI_TURN_FAILED,
   TI_TURN_CANCELLED
} ti_turn_state_t;

typedef struct
{
   char configuration_id[TI_REVISION_MAX];
   char toolset_id[TI_REVISION_MAX];
   char model_routing_id[TI_REVISION_MAX];
   char policy_revision[TI_REVISION_MAX];
   char context_manifest_id[TI_REVISION_MAX];
} ti_turn_snapshots_t;

typedef struct
{
   char turn_id[TI_ID_MAX];
   char session_id[TI_ID_MAX];
   char principal[TI_PRINCIPAL_MAX];
   ti_turn_snapshots_t snapshots;
   ti_turn_state_t state;
   uint64_t sequence;
} ti_turn_manifest_t;

typedef struct
{
   char event[TI_EVENT_MAX];
   char turn_id[TI_ID_MAX];
   char session_id[TI_ID_MAX];
   char principal[TI_PRINCIPAL_MAX];
   char detail[TI_DETAIL_MAX];
   ti_turn_state_t state;
   uint64_t sequence;
} ti_event_t;

typedef void (*ti_event_callback_t)(const ti_event_t *event, void *userdata);

typedef enum
{
   TI_FRESHNESS_UNKNOWN = 0,
   TI_FRESHNESS_CURRENT,
   TI_FRESHNESS_STALE
} ti_freshness_t;

typedef struct
{
   char domain[TI_DOMAIN_MAX];
   char scope_id[TI_SCOPE_MAX];
   uint64_t epoch;
} ti_knowledge_basis_t;

typedef enum
{
   TI_EFFECT_READ_ONLY = 0,
   TI_EFFECT_REVERSIBLE,
   TI_EFFECT_CONDITIONALLY_REVERSIBLE,
   TI_EFFECT_IRREVERSIBLE,
   TI_EFFECT_EXTERNAL_COMMUNICATION,
   TI_EFFECT_UNCLASSIFIED
} ti_effect_class_t;

typedef enum
{
   TI_EFFECT_MODE_OFF = 0,
   TI_EFFECT_MODE_SHADOW,
   TI_EFFECT_MODE_ENFORCE
} ti_effect_mode_t;

typedef enum
{
   TI_EFFECT_PROPOSED = 0,
   TI_EFFECT_VALIDATED,
   TI_EFFECT_EXECUTING,
   TI_EFFECT_SUCCEEDED,
   TI_EFFECT_FAILED,
   TI_EFFECT_UNKNOWN_OUTCOME,
   TI_EFFECT_REFUSED
} ti_effect_state_t;

typedef enum
{
   TI_POSTCONDITION_NONE = 0,
   TI_POSTCONDITION_PENDING,
   TI_POSTCONDITION_PASSED,
   TI_POSTCONDITION_FAILED
} ti_postcondition_state_t;

typedef enum
{
   TI_IDEMPOTENCY_UNKNOWN = 0,
   TI_IDEMPOTENT,
   TI_NON_IDEMPOTENT
} ti_idempotency_t;

typedef struct
{
   char contract_id[TI_ID_MAX];
   char session_id[TI_ID_MAX];
   char tool[TI_TOOL_MAX];
   char target_digest[TI_DIGEST_MAX];
   char arguments_digest[TI_DIGEST_MAX];
   ti_effect_class_t effect_class;
   ti_effect_mode_t mode;
   ti_effect_state_t state;
   ti_postcondition_state_t postcondition;
   ti_idempotency_t idempotency;
   int matched;
   int authorization_required;
   int authorized;
   uint64_t sequence;
} ti_effect_contract_t;

/* Installation must complete before worker threads start. NULL disables the
 * callback. Events contain bounded identity/enum metadata, never prompt, tool
 * argument, result, or model-response content. */
void ti_set_event_callback(ti_event_callback_t callback, void *userdata);

/* Initialize a caller-owned manifest in RECEIVED state and emit turn.created.
 * turn_id is required; session/principal may be empty. */
int ti_turn_manifest_init(ti_turn_manifest_t *manifest, const char *turn_id, const char *session_id,
                          const char *principal);

/* Bind immutable per-turn snapshot identities. May be called once while the
 * turn is RECEIVED; a second call or a late call fails without mutation. */
int ti_turn_bind_snapshots(ti_turn_manifest_t *manifest, const ti_turn_snapshots_t *snapshots);

/* Advance through the turn state machine. Terminal states cannot transition.
 * Read-only turns may complete from CONTEXTUALIZED; failure/block/cancel may be
 * entered from any non-terminal state. */
int ti_turn_transition(ti_turn_manifest_t *manifest, ti_turn_state_t next, const char *detail);

int ti_turn_state_terminal(ti_turn_state_t state);
const char *ti_turn_state_name(ti_turn_state_t state);

/* Return a newly allocated JSON object containing only bounded contract
 * metadata. Caller owns it. */
struct cJSON *ti_turn_manifest_json(const ti_turn_manifest_t *manifest);

/* Scoped knowledge epochs. The registry is process-local and thread-safe; the
 * durable curator feed remains the authority that replays invalidations after a
 * restart. An absent scope has epoch zero. */
uint64_t ti_knowledge_epoch_current(const char *domain, const char *scope_id);
uint64_t ti_knowledge_epoch_advance(const char *domain, const char *scope_id, const char *reason);
ti_freshness_t ti_knowledge_basis_freshness(const ti_knowledge_basis_t *basis);

/* Compare and atomically update a session's last observed epoch. Returns STALE
 * only when this session observed an earlier epoch; a first observation is
 * UNKNOWN because there is no prior answer to invalidate. */
ti_freshness_t ti_session_knowledge_observe(const char *session_id, uint64_t current_epoch,
                                            uint64_t *previous_epoch_out);

/* Clears only the knowledge/session registries. Intended for process teardown
 * tests; production lifetime is the daemon lifetime. */
void ti_knowledge_reset_for_test(void);

/* Bind an effect proposal to the mechanical tool, target identity and effective
 * normalized arguments. Raw target/argument content is hashed and never retained
 * or emitted. Shadow mode records drift without authorizing or blocking it. */
int ti_effect_contract_init(ti_effect_contract_t *contract, const char *session_id,
                            const char *tool, const char *target, const char *arguments_json,
                            ti_effect_class_t effect_class, ti_effect_mode_t mode);

/* Compare the execution about to happen with the proposal. Returns 1 for an
 * exact match, 0 for drift, and -1 for an invalid contract. The caller decides
 * whether a mismatch blocks according to the declared mode and policy. */
int ti_effect_contract_validate(ti_effect_contract_t *contract, const char *tool,
                                const char *target, const char *arguments_json,
                                ti_effect_class_t effect_class);
int ti_effect_contract_mark_executing(ti_effect_contract_t *contract);
int ti_effect_contract_set_authorization(ti_effect_contract_t *contract, int required,
                                         int authorized);
int ti_effect_contract_set_idempotency(ti_effect_contract_t *contract,
                                       ti_idempotency_t idempotency);
int ti_effect_contract_require_postcondition(ti_effect_contract_t *contract);
int ti_effect_contract_record_postcondition(ti_effect_contract_t *contract, int passed,
                                            const char *descriptor);
int ti_effect_contract_finish(ti_effect_contract_t *contract, ti_effect_state_t outcome,
                              const char *reason_code);
const char *ti_effect_class_name(ti_effect_class_t effect_class);
const char *ti_effect_state_name(ti_effect_state_t state);

#endif /* AIMEE_CORE_TURN_INTEGRITY_H */
