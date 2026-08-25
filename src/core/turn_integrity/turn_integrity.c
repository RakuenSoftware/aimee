#include <aimee/core/turn_integrity.h>

#include "cJSON.h"
#include "aimee_sha256.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

static ti_event_callback_t g_event_callback;
static void *g_event_userdata;

#define TI_KNOWLEDGE_SLOTS 256
#define TI_SESSION_SLOTS   256

typedef struct
{
   char domain[TI_DOMAIN_MAX];
   char scope_id[TI_SCOPE_MAX];
   uint64_t epoch;
   int in_use;
} ti_knowledge_slot_t;

typedef struct
{
   char session_id[TI_ID_MAX];
   uint64_t epoch;
   int in_use;
} ti_session_slot_t;

static ti_knowledge_slot_t g_knowledge[TI_KNOWLEDGE_SLOTS];
static ti_session_slot_t g_sessions[TI_SESSION_SLOTS];
static pthread_mutex_t g_knowledge_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_effect_sequence = 1;

static void copy_bounded(char *dst, size_t cap, const char *src)
{
   if (!dst || cap == 0)
      return;
   snprintf(dst, cap, "%s", src ? src : "");
}

void ti_set_event_callback(ti_event_callback_t callback, void *userdata)
{
   g_event_callback = callback;
   g_event_userdata = userdata;
}

const char *ti_turn_state_name(ti_turn_state_t state)
{
   switch (state)
   {
   case TI_TURN_RECEIVED:
      return "received";
   case TI_TURN_CONTEXTUALIZED:
      return "contextualized";
   case TI_TURN_CONTRACTED:
      return "contracted";
   case TI_TURN_AUTHORIZED:
      return "authorized";
   case TI_TURN_EXECUTING:
      return "executing";
   case TI_TURN_VERIFYING:
      return "verifying";
   case TI_TURN_REVIEWING:
      return "reviewing";
   case TI_TURN_COMPLETED:
      return "completed";
   case TI_TURN_BLOCKED:
      return "blocked";
   case TI_TURN_FAILED:
      return "failed";
   case TI_TURN_CANCELLED:
      return "cancelled";
   default:
      return "invalid";
   }
}

int ti_turn_state_terminal(ti_turn_state_t state)
{
   return state == TI_TURN_COMPLETED || state == TI_TURN_BLOCKED || state == TI_TURN_FAILED ||
          state == TI_TURN_CANCELLED;
}

static void emit_event(const ti_turn_manifest_t *manifest, const char *name, const char *detail)
{
   if (!manifest || !g_event_callback)
      return;
   ti_event_t event;
   memset(&event, 0, sizeof event);
   copy_bounded(event.event, sizeof event.event, name);
   copy_bounded(event.turn_id, sizeof event.turn_id, manifest->turn_id);
   copy_bounded(event.session_id, sizeof event.session_id, manifest->session_id);
   copy_bounded(event.principal, sizeof event.principal, manifest->principal);
   copy_bounded(event.detail, sizeof event.detail, detail);
   event.state = manifest->state;
   event.sequence = manifest->sequence;
   g_event_callback(&event, g_event_userdata);
}

int ti_turn_manifest_init(ti_turn_manifest_t *manifest, const char *turn_id, const char *session_id,
                          const char *principal)
{
   if (!manifest || !turn_id || !turn_id[0])
      return -1;
   memset(manifest, 0, sizeof *manifest);
   copy_bounded(manifest->turn_id, sizeof manifest->turn_id, turn_id);
   copy_bounded(manifest->session_id, sizeof manifest->session_id, session_id);
   copy_bounded(manifest->principal, sizeof manifest->principal, principal);
   manifest->state = TI_TURN_RECEIVED;
   manifest->sequence = 1;
   emit_event(manifest, "turn.created", "");
   return 0;
}

int ti_turn_bind_snapshots(ti_turn_manifest_t *manifest, const ti_turn_snapshots_t *snapshots)
{
   if (!manifest || !snapshots || manifest->state != TI_TURN_RECEIVED ||
       manifest->snapshots.configuration_id[0] || manifest->snapshots.toolset_id[0] ||
       manifest->snapshots.model_routing_id[0] || manifest->snapshots.policy_revision[0] ||
       manifest->snapshots.context_manifest_id[0])
      return -1;
   manifest->snapshots = *snapshots;
   manifest->sequence++;
   emit_event(manifest, "turn.snapshot_bound", "");
   return 0;
}

static int normal_transition(ti_turn_state_t from, ti_turn_state_t to)
{
   switch (from)
   {
   case TI_TURN_RECEIVED:
      return to == TI_TURN_CONTEXTUALIZED;
   case TI_TURN_CONTEXTUALIZED:
      return to == TI_TURN_CONTRACTED || to == TI_TURN_REVIEWING || to == TI_TURN_COMPLETED;
   case TI_TURN_CONTRACTED:
      return to == TI_TURN_AUTHORIZED;
   case TI_TURN_AUTHORIZED:
      return to == TI_TURN_EXECUTING;
   case TI_TURN_EXECUTING:
      return to == TI_TURN_VERIFYING;
   case TI_TURN_VERIFYING:
      return to == TI_TURN_REVIEWING || to == TI_TURN_COMPLETED;
   case TI_TURN_REVIEWING:
      return to == TI_TURN_COMPLETED;
   default:
      return 0;
   }
}

int ti_turn_transition(ti_turn_manifest_t *manifest, ti_turn_state_t next, const char *detail)
{
   if (!manifest || ti_turn_state_terminal(manifest->state) ||
       (next != TI_TURN_BLOCKED && next != TI_TURN_FAILED && next != TI_TURN_CANCELLED &&
        !normal_transition(manifest->state, next)))
      return -1;
   manifest->state = next;
   manifest->sequence++;
   char event_name[TI_EVENT_MAX];
   snprintf(event_name, sizeof event_name, "turn.%s", ti_turn_state_name(next));
   emit_event(manifest, event_name, detail);
   return 0;
}

struct cJSON *ti_turn_manifest_json(const ti_turn_manifest_t *manifest)
{
   if (!manifest)
      return NULL;
   cJSON *root = cJSON_CreateObject();
   cJSON *snapshots = cJSON_CreateObject();
   if (!root || !snapshots)
   {
      cJSON_Delete(root);
      cJSON_Delete(snapshots);
      return NULL;
   }
   cJSON_AddStringToObject(root, "turn_id", manifest->turn_id);
   cJSON_AddStringToObject(root, "session_id", manifest->session_id);
   cJSON_AddStringToObject(root, "principal", manifest->principal);
   cJSON_AddStringToObject(root, "state", ti_turn_state_name(manifest->state));
   cJSON_AddNumberToObject(root, "sequence", (double)manifest->sequence);
   cJSON_AddStringToObject(snapshots, "configuration_id", manifest->snapshots.configuration_id);
   cJSON_AddStringToObject(snapshots, "toolset_id", manifest->snapshots.toolset_id);
   cJSON_AddStringToObject(snapshots, "model_routing_id", manifest->snapshots.model_routing_id);
   cJSON_AddStringToObject(snapshots, "policy_revision", manifest->snapshots.policy_revision);
   cJSON_AddStringToObject(snapshots, "context_manifest_id",
                           manifest->snapshots.context_manifest_id);
   cJSON_AddItemToObject(root, "snapshots", snapshots);
   return root;
}

static ti_knowledge_slot_t *knowledge_find_locked(const char *domain, const char *scope_id,
                                                  int create)
{
   ti_knowledge_slot_t *free_slot = NULL;
   for (int i = 0; i < TI_KNOWLEDGE_SLOTS; i++)
   {
      ti_knowledge_slot_t *slot = &g_knowledge[i];
      if (!slot->in_use)
      {
         if (!free_slot)
            free_slot = slot;
         continue;
      }
      if (strcmp(slot->domain, domain) == 0 && strcmp(slot->scope_id, scope_id) == 0)
         return slot;
   }
   if (!create || !free_slot)
      return NULL;
   memset(free_slot, 0, sizeof *free_slot);
   copy_bounded(free_slot->domain, sizeof free_slot->domain, domain);
   copy_bounded(free_slot->scope_id, sizeof free_slot->scope_id, scope_id);
   free_slot->in_use = 1;
   return free_slot;
}

uint64_t ti_knowledge_epoch_current(const char *domain, const char *scope_id)
{
   if (!domain || !domain[0] || !scope_id || !scope_id[0])
      return 0;
   pthread_mutex_lock(&g_knowledge_lock);
   ti_knowledge_slot_t *slot = knowledge_find_locked(domain, scope_id, 0);
   uint64_t epoch = slot ? slot->epoch : 0;
   pthread_mutex_unlock(&g_knowledge_lock);
   return epoch;
}

uint64_t ti_knowledge_epoch_advance(const char *domain, const char *scope_id, const char *reason)
{
   if (!domain || !domain[0] || !scope_id || !scope_id[0])
      return 0;
   pthread_mutex_lock(&g_knowledge_lock);
   ti_knowledge_slot_t *slot = knowledge_find_locked(domain, scope_id, 1);
   uint64_t epoch = slot ? ++slot->epoch : 0;
   pthread_mutex_unlock(&g_knowledge_lock);

   if (epoch && g_event_callback)
   {
      ti_event_t event;
      memset(&event, 0, sizeof event);
      copy_bounded(event.event, sizeof event.event, "knowledge.invalidated");
      copy_bounded(event.turn_id, sizeof event.turn_id, scope_id);
      copy_bounded(event.session_id, sizeof event.session_id, domain);
      copy_bounded(event.detail, sizeof event.detail, reason);
      event.state = TI_TURN_RECEIVED;
      event.sequence = epoch;
      g_event_callback(&event, g_event_userdata);
   }
   return epoch;
}

ti_freshness_t ti_knowledge_basis_freshness(const ti_knowledge_basis_t *basis)
{
   if (!basis || !basis->domain[0] || !basis->scope_id[0])
      return TI_FRESHNESS_UNKNOWN;
   return ti_knowledge_epoch_current(basis->domain, basis->scope_id) == basis->epoch
              ? TI_FRESHNESS_CURRENT
              : TI_FRESHNESS_STALE;
}

ti_freshness_t ti_session_knowledge_observe(const char *session_id, uint64_t current_epoch,
                                            uint64_t *previous_epoch_out)
{
   if (previous_epoch_out)
      *previous_epoch_out = 0;
   if (!session_id || !session_id[0])
      return TI_FRESHNESS_UNKNOWN;
   pthread_mutex_lock(&g_knowledge_lock);
   ti_session_slot_t *slot = NULL;
   ti_session_slot_t *free_slot = NULL;
   for (int i = 0; i < TI_SESSION_SLOTS; i++)
   {
      if (!g_sessions[i].in_use)
      {
         if (!free_slot)
            free_slot = &g_sessions[i];
      }
      else if (strcmp(g_sessions[i].session_id, session_id) == 0)
      {
         slot = &g_sessions[i];
         break;
      }
   }
   if (!slot)
   {
      if (free_slot)
      {
         memset(free_slot, 0, sizeof *free_slot);
         copy_bounded(free_slot->session_id, sizeof free_slot->session_id, session_id);
         free_slot->epoch = current_epoch;
         free_slot->in_use = 1;
      }
      pthread_mutex_unlock(&g_knowledge_lock);
      return TI_FRESHNESS_UNKNOWN;
   }
   uint64_t previous = slot->epoch;
   slot->epoch = current_epoch;
   pthread_mutex_unlock(&g_knowledge_lock);
   if (previous_epoch_out)
      *previous_epoch_out = previous;
   return previous == current_epoch ? TI_FRESHNESS_CURRENT : TI_FRESHNESS_STALE;
}

void ti_knowledge_reset_for_test(void)
{
   pthread_mutex_lock(&g_knowledge_lock);
   memset(g_knowledge, 0, sizeof g_knowledge);
   memset(g_sessions, 0, sizeof g_sessions);
   pthread_mutex_unlock(&g_knowledge_lock);
}

const char *ti_effect_class_name(ti_effect_class_t effect_class)
{
   switch (effect_class)
   {
   case TI_EFFECT_READ_ONLY:
      return "read_only";
   case TI_EFFECT_REVERSIBLE:
      return "reversible";
   case TI_EFFECT_CONDITIONALLY_REVERSIBLE:
      return "conditionally_reversible";
   case TI_EFFECT_IRREVERSIBLE:
      return "irreversible";
   case TI_EFFECT_EXTERNAL_COMMUNICATION:
      return "external_communication";
   default:
      return "unclassified";
   }
}

const char *ti_effect_state_name(ti_effect_state_t state)
{
   switch (state)
   {
   case TI_EFFECT_PROPOSED:
      return "proposed";
   case TI_EFFECT_VALIDATED:
      return "validated";
   case TI_EFFECT_EXECUTING:
      return "executing";
   case TI_EFFECT_SUCCEEDED:
      return "succeeded";
   case TI_EFFECT_FAILED:
      return "failed";
   case TI_EFFECT_UNKNOWN_OUTCOME:
      return "unknown_outcome";
   case TI_EFFECT_REFUSED:
      return "refused";
   default:
      return "invalid";
   }
}

static int effect_digest(const char *value, char out[TI_DIGEST_MAX])
{
   const char *safe = value ? value : "";
   return aimee_sha256_hex(safe, strlen(safe), out);
}

static void emit_effect(const ti_effect_contract_t *contract, const char *event_name,
                        const char *reason_code)
{
   if (!contract || !g_event_callback)
      return;
   ti_event_t event;
   memset(&event, 0, sizeof event);
   copy_bounded(event.event, sizeof event.event, event_name);
   copy_bounded(event.turn_id, sizeof event.turn_id, contract->contract_id);
   copy_bounded(event.session_id, sizeof event.session_id, contract->session_id);
   copy_bounded(event.principal, sizeof event.principal, contract->tool);
   snprintf(event.detail, sizeof event.detail,
            "class=%s state=%s matched=%d authorized=%d target=%s arguments=%s reason=%.24s",
            ti_effect_class_name(contract->effect_class), ti_effect_state_name(contract->state),
            contract->matched, contract->authorized, contract->target_digest,
            contract->arguments_digest, reason_code ? reason_code : "");
   switch (contract->state)
   {
   case TI_EFFECT_PROPOSED:
      event.state = TI_TURN_CONTRACTED;
      break;
   case TI_EFFECT_VALIDATED:
      event.state = TI_TURN_AUTHORIZED;
      break;
   case TI_EFFECT_EXECUTING:
      event.state = TI_TURN_EXECUTING;
      break;
   case TI_EFFECT_SUCCEEDED:
      event.state = TI_TURN_COMPLETED;
      break;
   case TI_EFFECT_REFUSED:
      event.state = TI_TURN_BLOCKED;
      break;
   default:
      event.state = TI_TURN_FAILED;
      break;
   }
   event.sequence = contract->sequence;
   g_event_callback(&event, g_event_userdata);
}

int ti_effect_contract_init(ti_effect_contract_t *contract, const char *session_id,
                            const char *tool, const char *target, const char *arguments_json,
                            ti_effect_class_t effect_class, ti_effect_mode_t mode)
{
   if (!contract || !tool || !tool[0] || mode < TI_EFFECT_MODE_OFF || mode > TI_EFFECT_MODE_ENFORCE)
      return -1;
   memset(contract, 0, sizeof *contract);
   contract->sequence = atomic_fetch_add(&g_effect_sequence, 1);
   snprintf(contract->contract_id, sizeof contract->contract_id, "effect-%llu",
            (unsigned long long)contract->sequence);
   copy_bounded(contract->session_id, sizeof contract->session_id, session_id);
   copy_bounded(contract->tool, sizeof contract->tool, tool);
   if (effect_digest(target, contract->target_digest) != 0 ||
       effect_digest(arguments_json, contract->arguments_digest) != 0)
      return -1;
   contract->effect_class = effect_class;
   contract->mode = mode;
   contract->state = TI_EFFECT_PROPOSED;
   contract->idempotency = TI_IDEMPOTENCY_UNKNOWN;
   contract->matched = 1;
   if (mode != TI_EFFECT_MODE_OFF)
      emit_effect(contract, "effect.proposed", "");
   return 0;
}

int ti_effect_contract_validate(ti_effect_contract_t *contract, const char *tool,
                                const char *target, const char *arguments_json,
                                ti_effect_class_t effect_class)
{
   if (!contract || contract->state != TI_EFFECT_PROPOSED || !tool)
      return -1;
   char target_digest[TI_DIGEST_MAX];
   char arguments_digest[TI_DIGEST_MAX];
   if (effect_digest(target, target_digest) != 0 ||
       effect_digest(arguments_json, arguments_digest) != 0)
      return -1;
   contract->matched = strcmp(contract->tool, tool) == 0 &&
                       strcmp(contract->target_digest, target_digest) == 0 &&
                       strcmp(contract->arguments_digest, arguments_digest) == 0 &&
                       contract->effect_class == effect_class &&
                       (!contract->authorization_required || contract->authorized);
   contract->state = TI_EFFECT_VALIDATED;
   contract->sequence++;
   if (contract->mode != TI_EFFECT_MODE_OFF)
      emit_effect(contract, contract->matched ? "effect.validated" : "effect.mismatch",
                  contract->matched ? "" : "proposal_drift");
   return contract->matched;
}

int ti_effect_contract_set_authorization(ti_effect_contract_t *contract, int required,
                                         int authorized)
{
   if (!contract || contract->state != TI_EFFECT_PROPOSED)
      return -1;
   contract->authorization_required = !!required;
   contract->authorized = !!authorized;
   return 0;
}

int ti_effect_contract_set_idempotency(ti_effect_contract_t *contract, ti_idempotency_t idempotency)
{
   if (!contract || contract->state != TI_EFFECT_PROPOSED || idempotency < TI_IDEMPOTENCY_UNKNOWN ||
       idempotency > TI_NON_IDEMPOTENT)
      return -1;
   contract->idempotency = idempotency;
   return 0;
}

int ti_effect_contract_mark_executing(ti_effect_contract_t *contract)
{
   if (!contract || contract->state != TI_EFFECT_VALIDATED)
      return -1;
   contract->state = TI_EFFECT_EXECUTING;
   contract->sequence++;
   if (contract->mode != TI_EFFECT_MODE_OFF)
      emit_effect(contract, "effect.executing", "");
   return 0;
}

int ti_effect_contract_require_postcondition(ti_effect_contract_t *contract)
{
   if (!contract ||
       (contract->state != TI_EFFECT_PROPOSED && contract->state != TI_EFFECT_VALIDATED))
      return -1;
   contract->postcondition = TI_POSTCONDITION_PENDING;
   return 0;
}

int ti_effect_contract_record_postcondition(ti_effect_contract_t *contract, int passed,
                                            const char *descriptor)
{
   if (!contract || contract->state != TI_EFFECT_EXECUTING ||
       contract->postcondition != TI_POSTCONDITION_PENDING)
      return -1;
   contract->postcondition = passed ? TI_POSTCONDITION_PASSED : TI_POSTCONDITION_FAILED;
   contract->sequence++;
   if (contract->mode != TI_EFFECT_MODE_OFF)
      emit_effect(contract, "effect.postcondition", descriptor);
   return 0;
}

int ti_effect_contract_finish(ti_effect_contract_t *contract, ti_effect_state_t outcome,
                              const char *reason_code)
{
   if (!contract ||
       (contract->state != TI_EFFECT_PROPOSED && contract->state != TI_EFFECT_VALIDATED &&
        contract->state != TI_EFFECT_EXECUTING) ||
       (outcome != TI_EFFECT_SUCCEEDED && outcome != TI_EFFECT_FAILED &&
        outcome != TI_EFFECT_UNKNOWN_OUTCOME && outcome != TI_EFFECT_REFUSED))
      return -1;
   if (outcome == TI_EFFECT_SUCCEEDED && contract->postcondition == TI_POSTCONDITION_PENDING)
      return -1;
   if (outcome == TI_EFFECT_SUCCEEDED && contract->postcondition == TI_POSTCONDITION_FAILED)
      return -1;
   contract->state = outcome;
   contract->sequence++;
   if (contract->mode != TI_EFFECT_MODE_OFF)
      emit_effect(contract, "effect.completed", reason_code);
   return 0;
}
