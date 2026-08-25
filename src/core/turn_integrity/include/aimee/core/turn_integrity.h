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
#define TI_DETAIL_MAX    256

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

#endif /* AIMEE_CORE_TURN_INTEGRITY_H */
