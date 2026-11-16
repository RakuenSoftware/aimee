#include "original_input_created.h"
#include "original_input_delivery.h"
#include "original_input_plan.h"
#include "host_original_submission.h"
#include "host_input_attachment.h"
#include "aimee_sha256.h"
#include "server_native_history.h"
#include "server_session_capture.h"
#include "server_session_control.h"
#include "server_learning_outcome.h"
#include "db1_client/primary_sessions.h"
#include "vault_principal.h"
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
   unsigned long long sequence;
   int block_index;
} source_claim_t;
#define TERMINAL_OBSERVERS_MAX 8
typedef struct
{
   server_session_terminal_action_fn action;
   server_session_verification_prepare_fn prepare;
   void *resource;
} terminal_observer_t;
struct server_session_capture
{
   char session[128], turn[128], principal[VAULT_PRINCIPAL_MAX], execution_session[128];
   pthread_mutex_t lock;
   atomic_uint references;
   int original_post_in_progress;
   unsigned long long original_input_sequence;
   host_original_documentary *original_documentary;
   atomic_int closed, failed, operator_cancelled;
   server_control_scope_t *controls;
   server_session_capture_counts_t counts;
   char committed_sequence[32];
   server_session_terminal_action_fn terminal_action;
   void *terminal_resource;
   terminal_observer_t terminal_observers[TERMINAL_OBSERVERS_MAX];
   int n_terminal_observers;
   unsigned long long model_generation, model_active;
   source_claim_t *source_claims;
   int n_claims, claims_capacity;
   struct server_session_capture *next;
};
/* Optional for standalone legacy capture users. A configured documentary lane
 * requires all strong generic lifetime functions before installation. */
extern int host_original_documentary_retain(const host_original_documentary *,
                                            host_original_documentary **) __attribute__((weak));
extern void host_original_documentary_release(host_original_documentary **) __attribute__((weak));
__attribute__((used, externally_visible, retain))
const unsigned aimee_original_documentary_capture_contract_v1 = 1;

/* Optional at link time only; source-bearing capture refuses without SDK54. */
extern int server_native_history_verify_message_tool_sources(const aimee_message_t *)
    __attribute__((weak));
extern int db1_session_tool_source_append(const char *, const char *, const char *, const char *,
                                          const char *, char[32]) __attribute__((weak));
static server_input_attachment_admit_fn input_admit;
static void *input_admit_state;
static int input_admit_installed;
static server_input_owner_admit_fn input_owner_admit;
static server_input_created_fn input_created;
struct server_input_created_cap
{
   server_input_created_view_t view;
   server_input_created_delivery_view_t delivery;
   server_session_capture_t *owner;
};
static _Thread_local const server_input_created_cap_t *active_created_cap;
__attribute__((used, externally_visible, retain))
const unsigned aimee_original_input_post_created_contract_v1 = 1;
__attribute__((used, externally_visible, retain)) int
server_input_attachment_install_owner(server_input_owner_admit_fn admit,
                                      server_input_created_fn created, void *state)
{
   if (input_admit_installed || !admit || !created)
      return -1;
   input_admit_installed = 1;
   input_owner_admit = admit;
   input_created = created;
   input_admit_state = state;
   return 0;
}
__attribute__((used, externally_visible, retain)) int server_input_created_owner_configured(void)
{
   return input_owner_admit && input_created;
}
__attribute__((used, externally_visible, retain)) int
server_input_created_view(const server_input_created_cap_t *cap, server_input_created_view_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   if (!cap || cap != active_created_cap)
      return -1;
   if (atomic_load(&cap->owner->closed) || server_session_capture_failed(cap->owner))
      return -1;
   *out = cap->view;
   return 0;
}

static int input_plan_required;
extern const unsigned aimee_original_input_delivery_client_contract_v1 __attribute__((weak));
extern int db1_session_original_input_begin_delivery(const char *, const char *, const char *,
                                                     const char *, const char *, const char *,
                                                     db1_original_input_receipt_t *, uint64_t *)
    __attribute__((weak));
extern int db1_session_original_input_delivery_get(const char *, const char *, const char *,
                                                   const char *, char **) __attribute__((weak));
extern int db1_session_original_input_delivery_page(const char *, const char *, const char *,
                                                    const char *, const char *, char **)
    __attribute__((weak));
extern int db1_session_original_input_delivery_ack(const char *, const char *, const char *,
                                                   const char *, const char *, const char *)
    __attribute__((weak));
/* Generic durable handoff. Only startup-owned callbacks interpret opaque intent. */
struct server_input_prepared_cap
{
   server_input_prepared_view_t view;
   server_session_capture_t *owner;
   db1_original_input_plan_receipt_t plan;
   int plan_bound;
};
struct server_input_committed_delivery_cap
{
   server_input_committed_delivery_view_t view;
   const char *completed_receipt; /* borrowed only by the current callback */
};
static _Thread_local const server_input_prepared_cap_t *active_prepared_cap;
static _Thread_local const server_input_committed_delivery_cap_t *active_delivery_cap;
static server_input_prepare_delivery_fn delivery_prepare;
static server_input_recover_delivery_fn delivery_recover;
static void *delivery_state;
static char delivery_receiver[129], delivery_actor[VAULT_PRINCIPAL_MAX];
static atomic_int delivery_capture_started;
__attribute__((used, externally_visible, retain))
const unsigned aimee_original_input_durable_delivery_contract_v1 = 1;
__attribute__((used, externally_visible, retain)) const char *
server_input_committed_delivery_completed_receipt_v1(
    const server_input_committed_delivery_cap_t *cap)
{
   return cap && cap == active_delivery_cap ? cap->completed_receipt : NULL;
}
static int delivery_u64(const char *s, uint64_t *out)
{
   if (!s || !*s || (s[0] == '0' && s[1]))
      return 0;
   uint64_t n = 0;
   for (; *s; s++)
   {
      if (*s < '0' || *s > '9' || n > (UINT64_MAX - (unsigned)(*s - '0')) / 10)
         return 0;
      n = n * 10 + (unsigned)(*s - '0');
   }
   *out = n;
   return n != 0 && n <= INT64_MAX;
}
static const char *delivery_string(const cJSON *o, const char *key)
{
   return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, key));
}
static int delivery_digest(const char *s)
{
   if (!s || strlen(s) != 64)
      return 0;
   for (const char *p = s; *p; p++)
      if (!(*p >= '0' && *p <= '9') && !(*p >= 'a' && *p <= 'f'))
         return 0;
   return 1;
}
static cJSON *delivery_parse(const char *wire, size_t limit)
{
   if (!wire || !wire[0] || strlen(wire) > limit)
      return NULL;
   cJSON *o = aimee_ir_json_parse_frame(wire, strlen(wire));
   if (!cJSON_IsObject(o))
   {
      cJSON_Delete(o);
      return NULL;
   }
   return o;
}
static int delivery_keys(const cJSON *o, const char *const *keys, size_t count)
{
   if (!cJSON_IsObject(o) || cJSON_GetArraySize(o) != (int)count)
      return 0;
   for (const cJSON *item = o->child; item; item = item->next)
   {
      size_t i = 0;
      for (; i < count; i++)
         if (!strcmp(item->string, keys[i]))
            break;
      if (i == count)
         return 0;
   }
   return 1;
}
static const char *const delivery_intent_keys[] = {"schema_version", "receiver_tag", "intent_key",
                                                   "opaque_intent"};
static const char *const delivery_record_keys[] = {
    "schema_version", "delivery_id",      "session_id",    "operation_uuid",
    "actor",          "receiver_tag",     "intent_key",    "opaque_intent",
    "begin_sequence", "input_sequence",   "original_turn", "begin_sha256",
    "input_sha256",   "input_event_json", "outcome",       "result_json"};
static const char *const delivery_page_keys[] = {"schema_version", "rows", "next_cursor",
                                                 "has_more"};
static int delivery_version(const cJSON *o)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "schema_version");
   return cJSON_IsNumber(v) && v->valuedouble == 1;
}
__attribute__((used, externally_visible, retain)) int
server_input_delivery_install_owner_v1(const char *receiver, const char *actor,
                                       server_input_prepare_delivery_fn prepare,
                                       server_input_recover_delivery_fn recover, void *state)
{
   if (!&aimee_original_input_delivery_client_contract_v1 ||
       aimee_original_input_delivery_client_contract_v1 != 1 ||
       !db1_session_original_input_begin_delivery || !db1_session_original_input_delivery_get ||
       !db1_session_original_input_delivery_page || !db1_session_original_input_delivery_ack ||
       delivery_prepare || atomic_load(&delivery_capture_started) || !input_owner_admit ||
       !input_created || !receiver || !*receiver || strlen(receiver) >= sizeof delivery_receiver ||
       !actor || !*actor || strlen(actor) >= sizeof delivery_actor || !prepare || !recover)
      return -1;
   memcpy(delivery_receiver, receiver, strlen(receiver) + 1);
   memcpy(delivery_actor, actor, strlen(actor) + 1);
   delivery_state = state;
   delivery_recover = recover;
   delivery_prepare = prepare;
   return 0;
}
__attribute__((used, externally_visible, retain)) int
server_input_prepared_view(const server_input_prepared_cap_t *cap,
                           server_input_prepared_view_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   if (!cap || cap != active_prepared_cap || atomic_load(&cap->owner->closed) ||
       server_session_capture_failed(cap->owner))
      return -1;
   *out = cap->view;
   return 0;
}
__attribute__((used, externally_visible, retain)) int
server_input_created_delivery_view(const server_input_created_cap_t *cap,
                                   server_input_created_delivery_view_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   server_input_created_view_t valid = {0};
   if (server_input_created_view(cap, &valid) || !cap->delivery.delivery_id)
      return -1;
   *out = cap->delivery;
   return 0;
}
__attribute__((used, externally_visible, retain)) int
server_input_committed_delivery_view(const server_input_committed_delivery_cap_t *cap,
                                     server_input_committed_delivery_view_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   if (!cap || cap != active_delivery_cap)
      return -1;
   *out = cap->view;
   return 0;
}
/* Called only after actual owner-admit, with an owned canonical message. */
static cJSON *delivery_prepare_original(server_session_capture_t *owner,
                                        const aimee_message_t *message, const void *cookie,
                                        char **owned_wire,
                                        db1_original_input_plan_receipt_t *out_plan)
{
   *owned_wire = NULL;
   memset(out_plan, 0, sizeof *out_plan);
   if (!delivery_prepare || strcmp(owner->principal, delivery_actor) || !cookie)
      return NULL;
   const aimee_block_t *b = &message->blocks[0];
   const cJSON *a = b->input_attachment;
   const char *opaque = delivery_string(a, "opaque");
   char opaque_hash[65];
   if (!opaque || aimee_sha256_hex(opaque, strlen(opaque), opaque_hash))
      return NULL;
   server_input_prepared_cap_t cap = {
       .owner = owner,
       .view = {.schema_version = 1,
                .session = owner->session,
                .principal = owner->principal,
                .execution_session = owner->execution_session,
                .operation_uuid = delivery_string(a, "operation_uuid"),
                .proposed_turn = owner->turn,
                .text = b->text,
                .opaque = opaque,
                .text_sha256 = delivery_string(a, "text_sha256"),
                .opaque_sha256 = opaque_hash,
                .text_len = strlen(b->text),
                .opaque_len = strlen(opaque),
                .owner_cookie = cookie}};
   active_prepared_cap = &cap;
   int rc = delivery_prepare(&cap, owned_wire, delivery_state);
   active_prepared_cap = NULL;
   if (input_plan_required && !cap.plan_bound)
      return NULL;
   *out_plan = cap.plan;
   cJSON *o = rc == 0 && !atomic_load(&owner->closed) && !server_session_capture_failed(owner)
                  ? delivery_parse(*owned_wire, 32768)
                  : NULL;
   const char *receiver = delivery_string(o, "receiver_tag"),
              *key = delivery_string(o, "intent_key"),
              *intent = delivery_string(o, "opaque_intent");
   if (!o || !delivery_keys(o, delivery_intent_keys, 4) || !delivery_version(o) || !receiver ||
       strcmp(receiver, delivery_receiver) || !delivery_digest(key) || !intent || !*intent ||
       strlen(intent) > 16384)
   {
      cJSON_Delete(o);
      return NULL;
   }
   return o;
}
static int delivery_reconcile_record(const char *wire, const char *expected_session,
                                     const char *expected_operation, const char *expected_id,
                                     char **completed_receipt)
{
   int result = -1;
   cJSON *o = delivery_parse(wire, 3u * 1024u * 1024u), *event = NULL;
   char *receipt = NULL;
   aimee_message_t message = {0};
   server_input_committed_delivery_cap_t cap = {0};
   const char *id = delivery_string(o, "delivery_id"), *session = delivery_string(o, "session_id"),
              *operation = delivery_string(o, "operation_uuid"),
              *actor = delivery_string(o, "actor"), *receiver = delivery_string(o, "receiver_tag"),
              *key = delivery_string(o, "intent_key"),
              *intent = delivery_string(o, "opaque_intent"),
              *turn = delivery_string(o, "original_turn"),
              *begin = delivery_string(o, "begin_sequence"),
              *input = delivery_string(o, "input_sequence"),
              *input_json = delivery_string(o, "input_event_json"),
              *input_hash = delivery_string(o, "input_sha256"),
              *outcome = delivery_string(o, "outcome"),
              *begin_hash = delivery_string(o, "begin_sha256"),
              *saved_result = delivery_string(o, "result_json");
   uint64_t di = 0, bs = 0, is = 0;
   char actual_hash[65], opaque_hash[65];
   if (!o || !delivery_version(o) || !delivery_keys(o, delivery_record_keys, 16) ||
       !delivery_u64(id, &di) || !session || !*session || strlen(session) >= 128 || !operation ||
       !*operation || strcmp(session, expected_session) || strcmp(operation, expected_operation) ||
       (expected_id && strcmp(id, expected_id)) || !actor || strcmp(actor, delivery_actor) ||
       !receiver || strcmp(receiver, delivery_receiver) || !delivery_digest(key) || !intent ||
       !*intent || strlen(intent) > 16384 || !turn || !*turn || strlen(turn) >= 128 ||
       !delivery_u64(begin, &bs) || bs == UINT64_MAX || !delivery_u64(input, &is) || is != bs + 1 ||
       !input_json || !delivery_digest(input_hash) ||
       aimee_sha256_hex(input_json, strlen(input_json), actual_hash) ||
       strcmp(actual_hash, input_hash) || !outcome || !delivery_digest(begin_hash) ||
       !saved_result || strlen(saved_result) > 32768)
      goto done;
   int pending = !strcmp(outcome, "pending"),
       completed = !strcmp(outcome, "delivered") || !strcmp(outcome, "terminal");
   if ((!pending && !completed) || (pending && saved_result[0]))
      goto done;
   /* An existing terminal result is not a delivered-input replay. Reject before
    * invoking recovery, so it cannot fall through the pending side-effect lane. */
   if (completed_receipt && completed && strcmp(outcome, "delivered"))
      goto done;
   if (completed)
   {
      cJSON *known = aimee_ir_json_parse_frame(saved_result, strlen(saved_result));
      if (!known)
         goto done;
      cJSON_Delete(known);
   }
   event = delivery_parse(input_json, 1048576u);
   if (!event || !delivery_string(event, "kind") ||
       strcmp(delivery_string(event, "kind"), "message") ||
       aimee_ir_message_parse(cJSON_GetObjectItemCaseSensitive(event, "payload"), &message) ||
       !message.role || strcmp(message.role, "user") || message.n_blocks != 1 ||
       !message.blocks[0].text ||
       !aimee_ir_input_attachment_shape_valid(message.blocks[0].input_attachment))
      goto done;
   const cJSON *a = message.blocks[0].input_attachment;
   const char *text = message.blocks[0].text, *opaque = delivery_string(a, "opaque"),
              *text_hash = delivery_string(a, "text_sha256");
   if (!opaque || strcmp(operation, delivery_string(a, "operation_uuid")) ||
       aimee_sha256_hex(text, strlen(text), actual_hash) || strcmp(actual_hash, text_hash) ||
       aimee_sha256_hex(opaque, strlen(opaque), opaque_hash))
      goto done;
   if (completed && !completed_receipt)
   {
      result = 1;
      goto done;
   }
   cap.view = (server_input_committed_delivery_view_t){.schema_version = 1,
                                                       .delivery_id = di,
                                                       .session = session,
                                                       .principal = actor,
                                                       .operation_uuid = operation,
                                                       .original_turn = turn,
                                                       .receiver_tag = receiver,
                                                       .intent_key = key,
                                                       .opaque_intent = intent,
                                                       .opaque_intent_len = strlen(intent),
                                                       .text = text,
                                                       .opaque = opaque,
                                                       .text_sha256 = text_hash,
                                                       .opaque_sha256 = opaque_hash,
                                                       .text_len = strlen(text),
                                                       .opaque_len = strlen(opaque)};
   cap.view.receipt.begin_sequence = bs;
   cap.view.receipt.input_sequence = is;
   cap.view.receipt.contract_version = 1;
   cap.view.receipt.outcome = DB1_ORIGINAL_INPUT_REPLAY;
   memcpy(cap.view.receipt.original_turn, turn, strlen(turn) + 1);
   cap.completed_receipt =
       completed && completed_receipt && !strcmp(outcome, "delivered") ? saved_result : NULL;
   active_delivery_cap = &cap;
   server_input_delivery_outcome_t status = delivery_recover(&cap, &receipt, delivery_state);
   active_delivery_cap = NULL;
   if ((completed_receipt && status != SERVER_INPUT_DELIVERY_DELIVERED) ||
       (status != SERVER_INPUT_DELIVERY_DELIVERED && status != SERVER_INPUT_DELIVERY_TERMINAL) ||
       !receipt || !*receipt || strlen(receipt) > 32768)
      goto done;
   cJSON *documentary = aimee_ir_json_parse_frame(receipt, strlen(receipt));
   if (!documentary)
      goto done;
   cJSON_Delete(documentary);
   if (completed)
   {
      cJSON *saved = aimee_ir_json_parse_frame(saved_result, strlen(saved_result));
      cJSON *recovered = aimee_ir_json_parse_frame(receipt, strlen(receipt));
      int same = status == SERVER_INPUT_DELIVERY_DELIVERED && !strcmp(outcome, "delivered") &&
                 saved && recovered && cJSON_Compare(saved, recovered, 1);
      cJSON_Delete(saved);
      cJSON_Delete(recovered);
      if (same && completed_receipt)
      {
         *completed_receipt = strdup(saved_result);
         result = *completed_receipt ? 1 : -1;
      }
      goto done;
   }
   if (db1_session_original_input_delivery_ack(
           id, receiver, actor, key,
           status == SERVER_INPUT_DELIVERY_DELIVERED ? "delivered" : "terminal", receipt))
      goto done;
   if (completed_receipt)
   {
      *completed_receipt = strdup(receipt);
      if (!*completed_receipt)
         goto done;
   }
   result = 1;
done:
   free(receipt);
   aimee_message_free(&message);
   cJSON_Delete(event);
   cJSON_Delete(o);
   return result;
}
__attribute__((used, externally_visible, retain)) int
server_input_delivery_reconcile_one_v1(const char *session, const char *operation)
{
   if (!delivery_prepare || active_prepared_cap || active_created_cap || active_delivery_cap ||
       !session || !operation)
      return -1;
   int previous;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous))
      return -1;
   char *wire = NULL;
   int rc = db1_session_original_input_delivery_get(session, operation, delivery_actor,
                                                    delivery_receiver, &wire);
   int result =
       rc == 1 ? 0
               : (rc == 0 ? delivery_reconcile_record(wire, session, operation, NULL, NULL) : -1);
   free(wire);
   pthread_setcancelstate(previous, NULL);
   if (previous == PTHREAD_CANCEL_ENABLE)
      pthread_testcancel();
   return result;
}
__attribute__((used, externally_visible, retain)) int
server_input_delivery_reconcile_page_v1(uint64_t after, unsigned limit, size_t max_bytes,
                                        uint64_t *next)
{
   if (!next)
      return -1;
   *next = after;
   if (!delivery_prepare || active_prepared_cap || active_created_cap || active_delivery_cap ||
       after > INT64_MAX || limit < 1 || limit > 32 || max_bytes < 1024 ||
       max_bytes > 4u * 1024u * 1024u)
      return -1;
   int previous;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous))
      return -1;
   char cursor[32], count[16], budget[32];
   snprintf(cursor, sizeof cursor, "%llu", (unsigned long long)after);
   snprintf(count, sizeof count, "%u", limit);
   snprintf(budget, sizeof budget, "%zu", max_bytes);
   char *wire = NULL;
   int result = -1;
   if (db1_session_original_input_delivery_page(delivery_receiver, delivery_actor, cursor, count,
                                                budget, &wire))
      goto done;
   cJSON *o = delivery_parse(wire, 4u * 1024u * 1024u);
   cJSON *rows = cJSON_GetObjectItemCaseSensitive(o, "rows");
   if (!o || !delivery_version(o) || !delivery_keys(o, delivery_page_keys, 4) ||
       !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(o, "has_more")) ||
       !delivery_string(o, "next_cursor") || !cJSON_IsArray(rows) ||
       cJSON_GetArraySize(rows) > (int)limit)
   {
      cJSON_Delete(o);
      goto done;
   }
   result = 0;
   for (const cJSON *row = rows->child; row; row = row->next)
   {
      const char *sid = delivery_string(row, "session_id"),
                 *op = delivery_string(row, "operation_uuid"),
                 *id = delivery_string(row, "delivery_id");
      uint64_t n = 0;
      if (!sid || !op || !delivery_u64(id, &n) || n <= *next)
      {
         result = -1;
         break;
      }
      char *fresh = NULL;
      int rc = db1_session_original_input_delivery_get(sid, op, delivery_actor, delivery_receiver,
                                                       &fresh);
      int handled = rc == 0 ? delivery_reconcile_record(fresh, sid, op, id, NULL) : -1;
      free(fresh);
      if (handled < 0)
      {
         result = -1;
         break;
      } /* completed prefix stops BEFORE unknown */
      *next = n;
      result++;
   }
   cJSON_Delete(o);
done:
   free(wire);
   pthread_setcancelstate(previous, NULL);
   if (previous == PTHREAD_CANCEL_ENABLE)
      pthread_testcancel();
   return result;
}

__attribute__((used, externally_visible, retain)) int
server_input_created_delivery_ack_v1(const server_input_created_cap_t *cap, const char *result_json)
{
   server_input_created_delivery_view_t v = {0};
   if (server_input_created_delivery_view(cap, &v) || !result_json || !*result_json ||
       strlen(result_json) > 32768)
      return -1;
   cJSON *result = aimee_ir_json_parse_frame(result_json, strlen(result_json));
   if (!result)
      return -1;
   cJSON_Delete(result);
   char id[32];
   snprintf(id, sizeof id, "%llu", (unsigned long long)v.delivery_id);
   return db1_session_original_input_delivery_ack(id, delivery_receiver, delivery_actor,
                                                  v.intent_key, "delivered", result_json);
}

/* Separate opt-in operational planning; no private custody semantics here. */
extern const unsigned aimee_original_input_safe_plan_client_contract_v1 __attribute__((weak));
#pragma weak db1_session_original_input_probe
#pragma weak db1_session_original_input_plan_reserve
#pragma weak db1_session_original_input_begin_planned_delivery
#pragma weak db1_session_original_input_plan_terminal
#pragma weak db1_original_input_probe_destroy
static server_input_plan_owner_check_fn plan_owner_check;
static server_input_plan_terminal_fn plan_terminal;
struct server_input_pending_plan_cap
{
   server_input_pending_plan_view_t view;
};
static _Thread_local const server_input_pending_plan_cap_t *active_pending_plan;
static _Thread_local int active_plan_request;
__attribute__((used, externally_visible, retain))
const unsigned aimee_original_input_safe_plan_contract_v1 = 1;
__attribute__((used, externally_visible, retain)) int server_input_delivery_install_owner_v2(
    const char *receiver, const char *actor, server_input_prepare_delivery_fn prepare,
    server_input_recover_delivery_fn recover, server_input_plan_owner_check_fn owner_check,
    server_input_plan_terminal_fn terminal, void *state)
{
   if (!owner_check || !terminal || !&aimee_original_input_safe_plan_client_contract_v1 ||
       aimee_original_input_safe_plan_client_contract_v1 != 1 ||
       !db1_session_original_input_probe || !db1_session_original_input_plan_reserve ||
       !db1_session_original_input_begin_planned_delivery ||
       !db1_session_original_input_plan_terminal || !db1_original_input_probe_destroy)
      return -1;
   if (server_input_delivery_install_owner_v1(receiver, actor, prepare, recover, state))
      return -1;
   plan_owner_check = owner_check;
   plan_terminal = terminal;
   input_plan_required = 1;
   return 0;
}
static int plan_owner(const void *cookie, server_input_plan_owner_view_t *view)
{
   memset(view, 0, sizeof *view);
   if (!input_plan_required || !cookie || !plan_owner_check || active_plan_request ||
       active_pending_plan || active_prepared_cap || active_created_cap || active_delivery_cap)
      return -1;
   active_plan_request = 1;
   int rc = plan_owner_check(cookie, view, delivery_state);
   active_plan_request = 0;
   if (rc || view->schema_version != 1 || !view->session || !view->operation_uuid ||
       !view->principal || !view->session[0] || strlen(view->session) >= 128 ||
       strlen(view->operation_uuid) != 36 || strcmp(view->principal, delivery_actor))
      return -1;
   return 0;
}
static void plan_probe_cancel_cleanup(void *value)
{
   db1_original_input_probe_destroy(value);
}
__attribute__((used, externally_visible, retain)) int
server_input_plan_probe_v1(const void *cookie, db1_original_input_probe_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   int previous;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous))
      return -1;
   server_input_plan_owner_view_t owner = {0};
   int rc = plan_owner(cookie, &owner);
   if (!rc)
      rc = db1_session_original_input_probe(owner.session, owner.operation_uuid, delivery_actor,
                                            out);
   pthread_cleanup_push(plan_probe_cancel_cleanup, out);
   pthread_setcancelstate(previous, NULL);
   if (previous == PTHREAD_CANCEL_ENABLE)
      pthread_testcancel();
   pthread_cleanup_pop(0);
   return rc;
}
static void replay_receipt_cancel(void *value)
{
   char **receipt = value;
   free(*receipt);
   *receipt = NULL;
}
__attribute__((used, externally_visible, retain)) int
server_input_plan_replay_receipt_v1(const void *cookie, char **out)
{
   if (!out || *out)
      return -1;
   int previous;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous))
      return -1;
   server_input_plan_owner_view_t owner = {0};
   char *wire = NULL;
   int result = -1;
   int rc = plan_owner(cookie, &owner);
   if (!rc)
      rc = db1_session_original_input_delivery_get(owner.session, owner.operation_uuid,
                                                   delivery_actor, delivery_receiver, &wire);
   if (!rc)
      result = delivery_reconcile_record(wire, owner.session, owner.operation_uuid, NULL, out);
   server_input_plan_owner_view_t after = {0};
   if (result == 1 && (plan_owner(cookie, &after) || strcmp(after.session, owner.session) ||
                       strcmp(after.operation_uuid, owner.operation_uuid) ||
                       strcmp(after.principal, owner.principal)))
   {
      free(*out);
      *out = NULL;
      result = -1;
   }
   free(wire);
   pthread_cleanup_push(replay_receipt_cancel, out);
   pthread_setcancelstate(previous, NULL);
   if (previous == PTHREAD_CANCEL_ENABLE)
      pthread_testcancel();
   pthread_cleanup_pop(0);
   return result;
}
__attribute__((used, externally_visible, retain)) int
server_input_plan_reserve_v1(const void *cookie, const char *text_hash, const char *plan_hash,
                             const char *opaque, db1_original_input_plan_receipt_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   if (!delivery_digest(text_hash) || !delivery_digest(plan_hash) || !opaque || !*opaque ||
       strlen(opaque) > 16384)
      return -1;
   int previous;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous))
      return -1;
   server_input_plan_owner_view_t owner = {0};
   int rc = plan_owner(cookie, &owner);
   if (!rc)
      rc = db1_session_original_input_plan_reserve(owner.session, owner.operation_uuid,
                                                   delivery_actor, delivery_receiver, text_hash,
                                                   plan_hash, opaque, out);
   pthread_setcancelstate(previous, NULL);
   if (previous == PTHREAD_CANCEL_ENABLE)
      pthread_testcancel();
   return rc;
}
__attribute__((used, externally_visible, retain)) int
server_input_prepared_bind_plan_v1(const server_input_prepared_cap_t *value,
                                   const db1_original_input_plan_receipt_t *receipt)
{
   if (!input_plan_required || !value || value != active_prepared_cap || !receipt ||
       receipt->contract_version != 1 || !receipt->plan_id || receipt->plan_id > INT64_MAX ||
       receipt->state != DB1_INPUT_PLAN_RESERVED || strcmp(receipt->session, value->view.session) ||
       strcmp(receipt->operation_uuid, value->view.operation_uuid) ||
       strcmp(receipt->actor, value->view.principal) ||
       strcmp(receipt->receiver_tag, delivery_receiver) ||
       strcmp(receipt->safe_text_sha256, value->view.text_sha256) ||
       !delivery_digest(receipt->plan_sha256))
      return -1;
   server_input_prepared_cap_t *cap = (server_input_prepared_cap_t *)value;
   if (cap->plan_bound)
      return -1;
   cap->plan = *receipt;
   cap->plan_bound = 1;
   return 0;
}
__attribute__((used, externally_visible, retain)) int
server_input_pending_plan_view(const server_input_pending_plan_cap_t *cap,
                               server_input_pending_plan_view_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   if (!cap || cap != active_pending_plan)
      return -1;
   *out = cap->view;
   return 0;
}
__attribute__((used, externally_visible, retain)) int
server_input_plan_conclude_v1(const char *session, const char *operation,
                              const char *expected_digest)
{
   if (!input_plan_required || !plan_terminal || !session || !operation ||
       !delivery_digest(expected_digest) || active_pending_plan || active_plan_request ||
       active_prepared_cap || active_created_cap || active_delivery_cap)
      return -1;
   int previous;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous))
      return -1;
   db1_original_input_probe_t probe = {0};
   char *result = NULL;
   int rc = db1_session_original_input_probe(session, operation, delivery_actor, &probe);
   if (rc)
      goto done;
   if (probe.plan.contract_version != 1 || strcmp(probe.plan.receiver_tag, delivery_receiver) ||
       strcmp(probe.plan.plan_sha256, expected_digest))
   {
      rc = DB1_INPUT_PLAN_CONFLICT;
      goto done;
   }
   if (probe.presence == DB1_INPUT_PROBE_TERMINAL)
   {
      rc = 1;
      goto done;
   }
   if (probe.presence != DB1_INPUT_PROBE_RESERVED || probe.plan.state != DB1_INPUT_PLAN_RESERVED)
   {
      rc = DB1_INPUT_PLAN_CONFLICT;
      goto done;
   }
   server_input_pending_plan_cap_t cap = {.view = {.schema_version = 1,
                                                   .receipt = probe.plan,
                                                   .opaque_plan = probe.opaque_plan,
                                                   .opaque_plan_len = probe.opaque_plan_len}};
   active_pending_plan = &cap;
   rc = plan_terminal(&cap, &result, delivery_state);
   active_pending_plan = NULL;
   if (rc || !result || !*result || strlen(result) > 32768)
   {
      rc = -1;
      goto done;
   }
   cJSON *proof = aimee_ir_json_parse_frame(result, strlen(result));
   if (!proof)
   {
      rc = -1;
      goto done;
   }
   cJSON_Delete(proof);
   char id[32];
   snprintf(id, sizeof id, "%llu", (unsigned long long)probe.plan.plan_id);
   rc = db1_session_original_input_plan_terminal(id, session, operation, delivery_actor,
                                                 delivery_receiver, expected_digest, result);
   if (!rc)
      rc = 1;
done:
   free(result);
   db1_original_input_probe_destroy(&probe);
   pthread_setcancelstate(previous, NULL);
   if (previous == PTHREAD_CANCEL_ENABLE)
      pthread_testcancel();
   return rc;
}

__attribute__((used, externally_visible, retain)) int
server_input_attachment_install(server_input_attachment_admit_fn admit, void *state)
{
   /* Trusted startup only; no update, request JSON or plugin registration. */
   if (input_admit_installed || !admit)
      return -1;
   input_admit_installed = 1;
   input_admit = admit;
   input_admit_state = state;
   return 0;
}
extern const unsigned aimee_original_input_attachment_contract_v1 __attribute__((weak));
extern int db1_session_original_input_begin(const char *, const char *, const char *, const char *,
                                            const char *, db1_original_input_receipt_t *)
    __attribute__((weak));
static _Thread_local server_session_capture_t *current;
/* Exact same-thread owner lock already held by the trusted verification call.
 * This is a read scope only, never a caller-provided flag or an authority. */
static _Thread_local server_session_capture_t *documentary_read_lock_owner;
static pthread_mutex_t owners_lock = PTHREAD_MUTEX_INITIALIZER;
static server_session_capture_t *owners;
int server_session_capture_has_owner(const char *session, const char *actor, const char *turn)
{
   if (!session || !session[0] || !actor || !actor[0] || !turn || !turn[0])
      return 0;
   int found = 0;
   pthread_mutex_lock(&owners_lock);
   for (const server_session_capture_t *owner = owners; owner; owner = owner->next)
      if (!strcmp(owner->session, session) && !strcmp(owner->principal, actor) &&
          !strcmp(owner->turn, turn))
      {
         found = 1;
         break;
      }
   pthread_mutex_unlock(&owners_lock);
   return found;
}
server_session_capture_t *server_session_capture_current(void)
{
   return current;
}
const char *server_session_capture_session(const server_session_capture_t *owner)
{
   return owner ? owner->session : NULL;
}
server_session_capture_t *server_session_capture_bind(server_session_capture_t *owner)
{
   server_session_capture_t *previous = current;
   current = owner;
   return previous;
}
server_session_capture_t *server_session_capture_open(const char *session, const char *turn,
                                                      const char *principal,
                                                      const char *execution_session)
{
   if (!session || !session[0] || strlen(session) >= 128 || !turn || !turn[0] ||
       strlen(turn) >= 128 || !principal || !principal[0] ||
       strlen(principal) >= VAULT_PRINCIPAL_MAX || !execution_session || !execution_session[0] ||
       strlen(execution_session) >= 128)
      return NULL;
   atomic_store(&delivery_capture_started, 1);
   server_session_capture_t *owner = calloc(1, sizeof *owner);
   if (!owner)
      return NULL;
   if (pthread_mutex_init(&owner->lock, NULL))
   {
      free(owner);
      return NULL;
   }
   atomic_init(&owner->references, 1);
   atomic_init(&owner->closed, 0);
   atomic_init(&owner->failed, 0);
   atomic_init(&owner->operator_cancelled, 0);
   snprintf(owner->session, sizeof owner->session, "%s", session);
   snprintf(owner->turn, sizeof owner->turn, "%s", turn);
   snprintf(owner->principal, sizeof owner->principal, "%s", principal);
   snprintf(owner->execution_session, sizeof owner->execution_session, "%s", execution_session);
   if (server_control_scope_capture(session, principal, &owner->controls))
   {
      server_session_capture_release(owner);
      return NULL;
   }
   pthread_mutex_lock(&owners_lock);
   for (server_session_capture_t *existing = owners; existing; existing = existing->next)
      if (!strcmp(existing->session, owner->session) && !strcmp(existing->turn, owner->turn))
      {
         pthread_mutex_unlock(&owners_lock);
         server_session_capture_release(owner);
         return NULL;
      }
   owner->next = owners;
   owners = owner;
   pthread_mutex_unlock(&owners_lock);
   return owner;
}
int server_session_capture_failed(const server_session_capture_t *owner)
{
   return owner && (atomic_load(&owner->failed) || server_control_scope_failed(owner->controls));
}
int server_session_capture_accepting(const server_session_capture_t *owner)
{
   return owner && !atomic_load(&owner->closed) && !server_session_capture_failed(owner);
}
void server_session_capture_fail(server_session_capture_t *owner)
{
   if (owner)
      atomic_store(&owner->failed, 1);
}
int server_session_capture_retain_current(const char *execution_session, const char *principal,
                                          server_session_capture_t **out)
{
   if (!out)
      return -1;
   *out = NULL;
   if (!current)
      return 0;
   if (!execution_session || !principal || strcmp(execution_session, current->execution_session) ||
       strcmp(principal, current->principal) || atomic_load(&current->closed) ||
       server_session_capture_failed(current))
      return -1;
   atomic_fetch_add(&current->references, 1);
   *out = current;
   return 0;
}
const char *server_session_capture_turn(const server_session_capture_t *owner)
{
   return owner ? owner->turn : NULL;
}
int server_session_capture_after_terminal(server_session_capture_t *owner,
                                          server_session_terminal_action_fn action, void *resource)
{
   if (!owner || !action)
      return -1;
   pthread_mutex_lock(&owner->lock);
   int refused = !server_session_capture_accepting(owner) || owner->terminal_action != NULL;
   for (int i = 0; !refused && i < owner->n_terminal_observers; i++)
      refused = resource && resource == owner->terminal_observers[i].resource;
   if (!refused)
   {
      owner->terminal_action = action;
      owner->terminal_resource = resource;
   }
   pthread_mutex_unlock(&owner->lock);
   return refused ? -1 : 0;
}
int server_session_capture_observe_verification(server_session_capture_t *owner,
                                                server_session_verification_prepare_fn prepare,
                                                server_session_terminal_action_fn action,
                                                void *resource)
{
   if (!owner || !action)
      return -1;
   pthread_mutex_lock(&owner->lock);
   int refused = !server_session_capture_accepting(owner) ||
                 owner->n_terminal_observers >= TERMINAL_OBSERVERS_MAX ||
                 (resource && resource == owner->terminal_resource);
   for (int i = 0; !refused && i < owner->n_terminal_observers; i++)
      refused = resource ? resource == owner->terminal_observers[i].resource
                         : !owner->terminal_observers[i].resource &&
                               action == owner->terminal_observers[i].action;
   if (!refused)
      owner->terminal_observers[owner->n_terminal_observers++] =
          (terminal_observer_t){.action = action, .prepare = prepare, .resource = resource};
   pthread_mutex_unlock(&owner->lock);
   return refused ? -1 : 0;
}
/* Caller holds the writer lock. Callbacks run only after that lock is released. */
static int terminal_observers_take(server_session_capture_t *owner,
                                   terminal_observer_t out[TERMINAL_OBSERVERS_MAX])
{
   int count = owner->n_terminal_observers;
   memcpy(out, owner->terminal_observers, (size_t)count * sizeof *out);
   memset(owner->terminal_observers, 0, sizeof owner->terminal_observers);
   owner->n_terminal_observers = 0;
   return count;
}
static void terminal_observers_finish(server_session_capture_t *owner,
                                      terminal_observer_t *observers, int count, int success)
{
   for (int i = 0; i < count; i++)
      observers[i].action(success && !server_session_capture_failed(owner), observers[i].resource);
}
void server_session_capture_release(server_session_capture_t *owner)
{
   if (owner && atomic_fetch_sub(&owner->references, 1) == 1)
   {
      pthread_mutex_lock(&owners_lock);
      server_session_capture_t **entry = &owners;
      while (*entry && *entry != owner)
         entry = &(*entry)->next;
      if (*entry)
         *entry = owner->next;
      pthread_mutex_unlock(&owners_lock);
      if (owner->terminal_action)
         owner->terminal_action(0, owner->terminal_resource);
      terminal_observers_finish(owner, owner->terminal_observers, owner->n_terminal_observers, 0);
      server_control_scope_release(owner->controls);
      pthread_mutex_destroy(&owner->lock);
      free(owner->source_claims);
      host_original_documentary *documentary = owner->original_documentary;
      free(owner);
      if (documentary && host_original_documentary_release)
         host_original_documentary_release(&documentary);
   }
}
int server_session_capture_operator_cancelled(const server_session_capture_t *owner)
{
   return owner && atomic_load(&owner->operator_cancelled);
}

int server_session_capture_cancel_owned(const char *actor, const char *session, const char *turn,
                                        char sequence[32], server_session_operator_cancel_fn commit)
{
   if (!commit || !actor || !actor[0] || !session || !session[0] || !turn || !turn[0] || !sequence)
      return 1;
   sequence[0] = 0;
   server_session_capture_t *owner = NULL;
   int foreign = 0;
   pthread_mutex_lock(&owners_lock);
   for (server_session_capture_t *entry = owners; entry; entry = entry->next)
      if (!strcmp(entry->session, session) && !strcmp(entry->turn, turn))
      {
         if (strcmp(entry->principal, actor))
            foreign = 1;
         else
         {
            /* release reaches zero before removing the list entry. Never
             * resurrect an owner whose destruction already started. */
            unsigned refs = atomic_load(&entry->references);
            while (refs && refs < UINT_MAX)
               if (atomic_compare_exchange_weak(&entry->references, &refs, refs + 1))
               {
                  owner = entry;
                  break;
               }
         }
         break;
      }
   pthread_mutex_unlock(&owners_lock);
   if (foreign)
      return 1;
   if (!owner)
      return commit(actor, session, turn, sequence);
   terminal_observer_t observers[TERMINAL_OBSERVERS_MAX];
   int count = 0;
   server_session_terminal_action_fn action = NULL;
   void *resource = NULL;
   pthread_mutex_lock(&owner->lock);
   int result = 1;
   if (atomic_load(&owner->operator_cancelled))
   {
      snprintf(sequence, 32, "%s", owner->committed_sequence);
      result = 0;
   }
   else if (!atomic_load(&owner->closed))
   {
      /* Serialize with producer appends. Go checks exact durable actor/turn,
       * native custody and atomically writes cancellation + interruptions. */
      result = commit(actor, session, turn, sequence);
      if (!result)
      {
         atomic_store(&owner->operator_cancelled, 1);
         atomic_store(&owner->closed, 1);
         snprintf(owner->committed_sequence, sizeof owner->committed_sequence, "%s", sequence);
         action = owner->terminal_action;
         resource = owner->terminal_resource;
         owner->terminal_action = NULL;
         owner->terminal_resource = NULL;
         count = terminal_observers_take(owner, observers);
      }
   }
   pthread_mutex_unlock(&owner->lock);
   if (action)
      action(0, resource);
   terminal_observers_finish(owner, observers, count, 0);
   server_session_capture_release(owner);
   return result;
}

void server_session_capture_close(server_session_capture_t *owner)
{
   if (!owner)
      return;
   pthread_mutex_lock(&owner->lock);
   atomic_store(&owner->closed, 1);
   server_session_terminal_action_fn action = owner->terminal_action;
   void *resource = owner->terminal_resource;
   owner->terminal_action = NULL;
   owner->terminal_resource = NULL;
   terminal_observer_t observers[TERMINAL_OBSERVERS_MAX];
   int count = terminal_observers_take(owner, observers);
   pthread_mutex_unlock(&owner->lock);
   if (action)
      action(0, resource);
   terminal_observers_finish(owner, observers, count, 0);
   if (current == owner)
      current = NULL;
   server_session_capture_release(owner);
}
int server_session_capture_counts(server_session_capture_t *owner,
                                  server_session_capture_counts_t *out)
{
   if (!owner || !out)
      return -1;
   pthread_mutex_lock(&owner->lock);
   *out = owner->counts;
   pthread_mutex_unlock(&owner->lock);
   return 0;
}
int server_session_capture_cursor(server_session_capture_t *owner, char sequence[32])
{
   if (!sequence)
      return -1;
   sequence[0] = 0;
   if (!owner)
      return -1;
   pthread_mutex_lock(&owner->lock);
   snprintf(sequence, 32, "%s", owner->committed_sequence[0] ? owner->committed_sequence : "0");
   pthread_mutex_unlock(&owner->lock);
   return 0;
}
int server_session_capture_model_enter(server_session_capture_t *owner, unsigned long long *lease)
{
   if (!lease)
      return -1;
   *lease = 0;
   if (!owner)
      return -1;
   pthread_mutex_lock(&owner->lock);
   int result = -1;
   if (server_session_capture_accepting(owner) && !owner->model_active &&
       owner->model_generation != ULLONG_MAX)
   {
      owner->model_active = ++owner->model_generation;
      *lease = owner->model_active;
      atomic_fetch_add(&owner->references, 1);
      result = 0;
   }
   pthread_mutex_unlock(&owner->lock);
   return result;
}
int server_session_capture_model_leave(server_session_capture_t *owner, unsigned long long lease)
{
   if (!owner || !lease)
      return -1;
   pthread_mutex_lock(&owner->lock);
   int matched = owner->model_active == lease;
   if (matched)
      owner->model_active = 0;
   pthread_mutex_unlock(&owner->lock);
   if (matched)
      server_session_capture_release(owner);
   return matched ? 0 : -1;
}
int server_session_capture_claim_call(server_session_capture_t *owner, const char *source_sequence,
                                      int block_index)
{
   if (!owner || !source_sequence || !source_sequence[0] || block_index < 0 ||
       block_index >= AIMEE_IR_MAX_BLOCKS)
      return -1;
   char *end, canonical[32];
   errno = 0;
   unsigned long long sequence = strtoull(source_sequence, &end, 10);
   int invalid = errno || *end || !sequence;
   snprintf(canonical, sizeof canonical, "%llu", sequence);
   if (invalid || strcmp(canonical, source_sequence))
      return -1;
   pthread_mutex_lock(&owner->lock);
   int result = -1;
   unsigned long long committed = strtoull(owner->committed_sequence, NULL, 10);
   if (!server_session_capture_accepting(owner) || sequence > committed)
      goto done;
   for (int i = 0; i < owner->n_claims; i++)
      if (owner->source_claims[i].sequence == sequence &&
          owner->source_claims[i].block_index == block_index)
      {
         result = 1;
         goto done;
      }
   if (owner->n_claims >= AIMEE_IR_MAX_BLOCKS)
      goto done;
   if (owner->n_claims == owner->claims_capacity)
   {
      int capacity = owner->claims_capacity ? owner->claims_capacity * 2 : 16;
      source_claim_t *claims = realloc(owner->source_claims, (size_t)capacity * sizeof *claims);
      if (!claims)
         goto done;
      owner->source_claims = claims;
      owner->claims_capacity = capacity;
   }
   owner->source_claims[owner->n_claims++] = (source_claim_t){sequence, block_index};
   result = 0;
done:
   pthread_mutex_unlock(&owner->lock);
   return result;
}
int server_session_capture_history(server_session_capture_t *owner, cJSON **out)
{
   if (!out)
      return -1;
   *out = NULL;
   if (!owner)
      return -1;
   pthread_mutex_lock(&owner->lock);
   if (!server_session_capture_accepting(owner))
   {
      pthread_mutex_unlock(&owner->lock);
      return -1;
   }
   char *wire = NULL;
   int status =
       db1_session_ir_running_history_load(owner->session, owner->principal, owner->turn, &wire);
   cJSON *history = !status && wire ? aimee_ir_json_parse_frame(wire, strlen(wire)) : NULL;
   free(wire);
   int valid = cJSON_IsArray(history) && cJSON_GetArraySize(history) <= AIMEE_IR_MAX_BLOCKS;
   const cJSON *item;
   cJSON_ArrayForEach(item, history)
   {
      aimee_message_t message = {0};
      int parsed = aimee_ir_message_parse(item, &message);
      aimee_request_t admitted = {.messages = &message, .n_messages = 1};
      valid = valid && !parsed && !aimee_ir_request_admit(&admitted);
      aimee_message_free(&message);
      if (!valid)
         break;
   }
   if (valid)
      *out = history;
   else
   {
      cJSON_Delete(history);
      server_session_capture_fail(owner);
   }
   pthread_mutex_unlock(&owner->lock);
   return valid ? 0 : -1;
}

static int qualify(cJSON *payload, const char *field, const char *turn, const char *native)
{
   if (!native)
      return 0;
   size_t length = strlen(turn) + strlen(native) + 2;
   char *id = malloc(length);
   if (!id)
      return -1;
   snprintf(id, length, "%s:%s", turn, native);
   cJSON *value = cJSON_CreateString(id);
   free(id);
   if (!value || !cJSON_ReplaceItemInObjectCaseSensitive(payload, field, value))
   {
      cJSON_Delete(value);
      return -1;
   }
   return 0;
}
static char *make_envelope(const server_session_capture_t *owner, const char *event_id,
                           const char *sequence, cJSON **payload)
{
   cJSON *envelope = cJSON_CreateObject();
   int valid = envelope && *payload && cJSON_AddNumberToObject(envelope, "version", 1) &&
               cJSON_AddStringToObject(envelope, "session_id", owner->session) &&
               cJSON_AddStringToObject(envelope, "sequence", sequence) &&
               cJSON_AddStringToObject(envelope, "event_id", event_id) &&
               cJSON_AddStringToObject(envelope, "turn_id", owner->turn) &&
               cJSON_AddStringToObject(envelope, "actor", owner->principal);
   if (valid && cJSON_AddItemToObject(envelope, "event", *payload))
      *payload = NULL;
   else
      valid = 0;
   char *wire = valid ? cJSON_PrintUnformatted(envelope) : NULL;
   cJSON_Delete(envelope);
   return wire;
}
struct capture_append_cancel_scope
{
   char **envelope;
   char *sequence;
};
static void capture_append_cancelled(void *opaque)
{
   struct capture_append_cancel_scope *scope = opaque;
   if (scope->envelope)
   {
      free(*scope->envelope);
      *scope->envelope = NULL;
   }
   if (scope->sequence)
      scope->sequence[0] = '\0';
}
static void capture_append_restore_cancel(int previous, char **envelope, char sequence[32])
{
   struct capture_append_cancel_scope scope = {envelope, sequence};
   pthread_cleanup_push(capture_append_cancelled, &scope);
   pthread_setcancelstate(previous, NULL);
   if (previous == PTHREAD_CANCEL_ENABLE)
      pthread_testcancel();
   pthread_cleanup_pop(0);
}
int server_session_capture_append(server_session_capture_t *owner,
                                  const aimee_session_event_t *event, char **envelope,
                                  char sequence[32])
{
   if (envelope)
      *envelope = NULL;
   if (sequence)
      sequence[0] = 0;
   if (!owner || !event)
      return -1;
   /* Match original_input_begin: callbacks must not deliver cancellation while
    * holding the writer lock or owned response allocations. */
   int previous_cancel_state;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous_cancel_state))
      return -1;
   server_session_terminal_action_fn action = NULL;
   void *resource = NULL;
   terminal_observer_t observers[TERMINAL_OBSERVERS_MAX];
   int n_observers = 0;
   int terminal_success = 0;
   pthread_mutex_lock(&owner->lock);
   if (owner->original_post_in_progress)
   {
      server_session_capture_fail(owner);
      pthread_mutex_unlock(&owner->lock);
      capture_append_restore_cancel(previous_cancel_state, envelope, sequence);
      return -1;
   }
   int failure_terminal = event->kind == AIMEE_SESSION_EXECUTION && event->execution_status &&
                          (!strcmp(event->execution_status, "failed") ||
                           !strcmp(event->execution_status, "cancelled"));
   if (atomic_load(&owner->closed) || (server_session_capture_failed(owner) && !failure_terminal))
   {
      pthread_mutex_unlock(&owner->lock);
      capture_append_restore_cancel(previous_cancel_state, envelope, sequence);
      return -1;
   }
   cJSON *proofs = NULL, *payload = NULL;
   int result = -1;
   /* Only the retained verification owner may supply privileged metadata.
    * Arbitrary producers cannot turn a JSON field into a grading receipt. */
   if (event->verification_proofs)
      goto finish;
   if (event->kind == AIMEE_SESSION_EXECUTION && event->execution_status &&
       !strcmp(event->execution_status, "completed") && !server_session_capture_failed(owner))
   {
      proofs = cJSON_CreateArray();
      for (int i = 0; proofs && i < owner->n_terminal_observers; ++i)
      {
         terminal_observer_t *observer = &owner->terminal_observers[i];
         const cJSON *proof = observer->prepare ? observer->prepare(observer->resource) : NULL;
         if (!proof || !aimee_ir_verification_proof_valid(proof))
            continue;
         const char *session =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(proof, "session_id"));
         const char *turn =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(proof, "turn_id"));
         const char *actor = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(proof, "actor"));
         if (strcmp(session, owner->session) || strcmp(turn, owner->turn) ||
             strcmp(actor, owner->principal))
            continue;
         cJSON *copy = cJSON_Duplicate(proof, 1);
         if (!copy || !cJSON_AddItemToArray(proofs, copy))
         {
            cJSON_Delete(copy);
            goto finish;
         }
      }
   }
   if (event->kind == AIMEE_SESSION_MESSAGE && event->message)
      for (int i = 0; i < event->message->n_blocks; i++)
         if (event->message->blocks[i].input_attachment)
            goto finish; /* Only the atomic initial-input batch may mint it. */
   int tool_sources = 0;
   if (event->kind == AIMEE_SESSION_MESSAGE && event->message)
   {
      for (int i = 0; i < event->message->n_blocks; i++)
         tool_sources |= event->message->blocks[i].tool_source != NULL;
      if (tool_sources)
      {
         if (!db1_session_tool_source_append || !server_native_history_verify_message_tool_sources)
            goto finish;
         server_session_capture_t *previous_read_owner = documentary_read_lock_owner;
         documentary_read_lock_owner = owner;
         int verified = server_native_history_verify_message_tool_sources(event->message);
         documentary_read_lock_owner = previous_read_owner;
         if (verified)
            goto finish;
      }
   }
   aimee_session_event_t admitted = *event;
   admitted.verification_proofs = cJSON_GetArraySize(proofs) ? proofs : NULL;
   payload = aimee_ir_session_event_json(&admitted);
   if (!payload || qualify(payload, "message_id", owner->turn, event->message_id) ||
       qualify(payload, "request_id", owner->turn, event->request_id))
      goto finish;
   if (event->kind == AIMEE_SESSION_CONTROL_REQUEST && owner->controls &&
       server_control_register(owner->controls, event->request_id, event->control_kind,
                               event->control))
      goto finish;
   char event_id[192], committed[32];
   snprintf(event_id, sizeof event_id, "%s:%llu", owner->turn, ++owner->counts.counter);
   char *wire = cJSON_PrintUnformatted(payload);
   if (wire)
      result = admitted.native_owner
                   ? db1_session_native_source_append(owner->session, event_id, owner->turn,
                                                      owner->principal, wire, committed)
               : tool_sources
                   ? db1_session_tool_source_append(owner->session, event_id, owner->turn,
                                                    owner->principal, wire, committed)
               : admitted.verification_proofs
                   ? db1_session_ir_append_verified(owner->session, event_id, owner->turn,
                                                    owner->principal, wire, committed)
                   : db1_session_ir_append(owner->session, event_id, owner->turn, owner->principal,
                                           wire, committed);
   free(wire);
   if (result)
      goto finish;
   snprintf(owner->committed_sequence, sizeof owner->committed_sequence, "%s", committed);
   /* Seal on the durable write, even if a later subscriber-envelope allocation
    * fails. A second terminal must never follow an already committed terminal. */
   if (event->kind == AIMEE_SESSION_EXECUTION && strcmp(event->execution_status, "running"))
   {
      atomic_store(&owner->closed, 1);
      action = owner->terminal_action;
      resource = owner->terminal_resource;
      owner->terminal_action = NULL;
      owner->terminal_resource = NULL;
      n_observers = terminal_observers_take(owner, observers);
      terminal_success = !strcmp(event->execution_status, "completed");
   }
   if (event->kind == AIMEE_SESSION_EXECUTION && strcmp(event->execution_status, "running"))
      server_learning_observe_terminal(owner->session, owner->turn, committed, event_id,
                                       owner->principal, payload);
   if (envelope)
   {
      *envelope = make_envelope(owner, event_id, committed, &payload);
      if (!*envelope)
      {
         result = -1;
         goto finish;
      }
   }
   if (sequence)
      snprintf(sequence, 32, "%s", committed);
   const char *role =
       event->kind == AIMEE_SESSION_MESSAGE && event->message ? event->message->role : event->role;
   if (role && !strcmp(role, "assistant") &&
       !(event->kind == AIMEE_SESSION_MESSAGE && aimee_ir_message_runtime_advisory(event->message)))
   {
      owner->counts.model_events++;
      if (event->kind == AIMEE_SESSION_MESSAGE ||
          (event->kind == AIMEE_SESSION_DELTA && event->delta &&
           event->delta->type == AIMEE_DELTA_TURN_STOP))
         owner->counts.model_completions++;
   }
finish:
   if (result)
      server_session_capture_fail(owner);
   cJSON_Delete(proofs);
   cJSON_Delete(payload);
   pthread_mutex_unlock(&owner->lock);
   int resource_success = !result && terminal_success && !server_session_capture_failed(owner);
   if (action)
      action(resource_success, resource);
   terminal_observers_finish(owner, observers, n_observers, resource_success);
   capture_append_restore_cancel(previous_cancel_state, envelope, sequence);
   return result;
}
int server_session_capture_sink(const aimee_session_event_t *event, void *owner)
{
   return server_session_capture_append(owner, event, NULL, NULL);
}

__attribute__((used, externally_visible, retain)) int
server_session_capture_original_input_begin(server_session_capture_t *owner,
                                            const aimee_message_t *message)
{
   if (!owner || !message || !message->role || !message->blocks || active_created_cap ||
       active_prepared_cap || active_delivery_cap)
      return -1;
   /* Module transport and the installed synchronous callback may encounter
    * cancellation points. Never unwind while holding the writer lock, a live
    * capability, the post phase, or its retained owner reference. */
   int previous_cancel_state;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous_cancel_state))
      return -1;
   int result = -1, post = 0;
   const void *cookie = NULL;
   aimee_message_t recorded = {0};
   cJSON *delivery = NULL;
   char *delivery_wire = NULL;
   db1_original_input_plan_receipt_t prepared_plan = {0};
   server_input_created_cap_t cap = {0};
   pthread_mutex_lock(&owner->lock);
   if (owner->counts.counter || atomic_load(&owner->closed) || atomic_load(&owner->failed) ||
       strcmp(message->role, "user") || message->n_blocks != 1 ||
       !message->blocks[0].input_attachment || !message->blocks[0].text ||
       !&aimee_original_input_attachment_contract_v1 ||
       aimee_original_input_attachment_contract_v1 != 1 || (!input_admit && !input_owner_admit) ||
       !db1_session_original_input_begin)
      goto done;
   const aimee_block_t *block = &message->blocks[0];
   char actual[65];
   const char *expected = cJSON_GetStringValue(
       cJSON_GetObjectItemCaseSensitive(block->input_attachment, "text_sha256"));
   const char *operation = cJSON_GetStringValue(
       cJSON_GetObjectItemCaseSensitive(block->input_attachment, "operation_uuid"));
   if (!aimee_ir_input_attachment_shape_valid(block->input_attachment) ||
       aimee_sha256_hex(block->text, strlen(block->text), actual) || !expected ||
       strcmp(actual, expected))
      goto done;
   if (input_owner_admit)
   {
      if (input_owner_admit(owner->session, owner->turn, owner->principal, message, &cookie,
                            input_admit_state) ||
          !cookie || !input_created)
         goto done;
   }
   else if (input_admit(owner->session, owner->turn, owner->principal, message, input_admit_state))
      goto done;
   aimee_session_event_t begin = {.kind = AIMEE_SESSION_EXECUTION, .execution_status = "running"};
   aimee_session_event_t input = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = "user", .message = message};
   cJSON *batch = cJSON_CreateObject(), *a = aimee_ir_session_event_json(&begin),
         *b = aimee_ir_session_event_json(&input);
   if (!batch || !a || !b)
   {
      cJSON_Delete(batch);
      cJSON_Delete(a);
      cJSON_Delete(b);
      goto done;
   }
   /* Own exactly the canonical TEXT/opaque bytes being sent, not caller storage.
    */
   if (aimee_ir_message_parse(cJSON_GetObjectItemCaseSensitive(b, "payload"), &recorded))
   {
      cJSON_Delete(batch);
      cJSON_Delete(a);
      cJSON_Delete(b);
      goto done;
   }
   if (delivery_prepare)
   {
      delivery =
          delivery_prepare_original(owner, &recorded, cookie, &delivery_wire, &prepared_plan);
      if (!delivery)
      {
         cJSON_Delete(batch);
         cJSON_Delete(a);
         cJSON_Delete(b);
         goto done;
      }
   }
   cJSON_AddNumberToObject(batch, "schema_version", 1);
   cJSON_AddItemToObject(batch, "begin", a);
   cJSON_AddItemToObject(batch, "input", b);
   char *wire = cJSON_PrintUnformatted(batch);
   cJSON_Delete(batch);
   if (wire && strlen(wire) <= 1048576u)
   {
      db1_original_input_receipt_t receipt = {0};
      uint64_t delivery_id = 0;
      char plan_id[32];
      snprintf(plan_id, sizeof plan_id, "%llu", (unsigned long long)prepared_plan.plan_id);
      result = input_plan_required
                   ? db1_session_original_input_begin_planned_delivery(
                         owner->session, operation, owner->turn, owner->principal, wire,
                         delivery_wire, plan_id, prepared_plan.plan_sha256, &receipt, &delivery_id)
               : delivery_prepare
                   ? db1_session_original_input_begin_delivery(
                         owner->session, operation, owner->turn, owner->principal, wire,
                         delivery_wire, &receipt, &delivery_id)
                   : db1_session_original_input_begin(owner->session, operation, owner->turn,
                                                      owner->principal, wire, &receipt);
      if (result == 0 && receipt.contract_version == 1 &&
          receipt.outcome == DB1_ORIGINAL_INPUT_CREATED && receipt.begin_sequence > 0 &&
          receipt.begin_sequence != UINT64_MAX &&
          receipt.input_sequence == receipt.begin_sequence + 1 &&
          !strcmp(receipt.original_turn, owner->turn))
      {
         owner->original_input_sequence = receipt.input_sequence;
         owner->counts.counter = 2;
         snprintf(owner->committed_sequence, sizeof owner->committed_sequence, "%llu",
                  (unsigned long long)receipt.input_sequence);
         if (input_owner_admit)
         {
            const cJSON *attachment = recorded.blocks[0].input_attachment;
            cap.owner = owner;
            if (delivery_prepare)
               cap.delivery = (server_input_created_delivery_view_t){
                   .schema_version = 1,
                   .delivery_id = delivery_id,
                   .receiver_tag = delivery_string(delivery, "receiver_tag"),
                   .intent_key = delivery_string(delivery, "intent_key"),
                   .opaque_intent = delivery_string(delivery, "opaque_intent"),
                   .opaque_intent_len = strlen(delivery_string(delivery, "opaque_intent")),
                   .receipt = receipt};
            cap.view = (server_input_created_view_t){
                .schema_version = 1,
                .session = owner->session,
                .principal = owner->principal,
                .execution_session = owner->execution_session,
                .operation_uuid = cJSON_GetStringValue(
                    cJSON_GetObjectItemCaseSensitive(attachment, "operation_uuid")),
                .text = recorded.blocks[0].text,
                .opaque =
                    cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(attachment, "opaque")),
                .text_sha256 = cJSON_GetStringValue(
                    cJSON_GetObjectItemCaseSensitive(attachment, "text_sha256")),
                .receipt = receipt,
                .owner_cookie = cookie};
            cap.view.text_len = strlen(cap.view.text);
            cap.view.opaque_len = strlen(cap.view.opaque);
            owner->original_post_in_progress = 1;
            atomic_fetch_add(&owner->references, 1);
            post = 1;
         }
      }
      else if (result == 0)
         result = -1;
   }
   free(wire);
done:
   pthread_mutex_unlock(&owner->lock);
   if (post)
   {
      active_created_cap = &cap;
      int accepted = input_created(&cap, input_admit_state) == 0;
      active_created_cap = NULL;
      pthread_mutex_lock(&owner->lock);
      owner->original_post_in_progress = 0;
      if (!accepted || atomic_load(&owner->closed) || server_session_capture_failed(owner))
      {
         server_session_capture_fail(owner);
         result = -4;
      }
      pthread_mutex_unlock(&owner->lock);
      server_session_capture_release(owner);
   }
   cJSON_Delete(delivery);
   free(delivery_wire);
   aimee_message_free(&recorded);
   pthread_setcancelstate(previous_cancel_state, NULL);
   if (previous_cancel_state == PTHREAD_CANCEL_ENABLE)
      pthread_testcancel();
   return result;
}

__attribute__((used, externally_visible, retain)) const char *
server_session_capture_execution_session(const server_session_capture_t *owner)
{
   return owner ? owner->execution_session : NULL;
}
__attribute__((used, externally_visible, retain)) int
server_session_capture_set_documentary(server_session_capture_t *owner,
                                       const host_original_documentary *documentary)
{
   if (!owner || owner != current || !documentary || !host_original_documentary_retain ||
       !host_original_documentary_release)
      return -1;
   int previous;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous))
      return -1;
   host_original_documentary *copy = NULL;
   int result = host_original_documentary_retain(documentary, &copy);
   if (!result)
   {
      pthread_mutex_lock(&owner->lock);
      if (atomic_load(&owner->closed) || atomic_load(&owner->failed) ||
          owner->original_post_in_progress || owner->model_generation ||
          owner->original_documentary)
         result = -1;
      else
      {
         owner->original_documentary = copy;
         copy = NULL;
      }
      pthread_mutex_unlock(&owner->lock);
   }
   host_original_documentary_release(&copy);
   pthread_setcancelstate(previous, NULL);
   if (previous == PTHREAD_CANCEL_ENABLE)
      pthread_testcancel();
   return result;
}
struct documentary_capture_scope
{
   server_session_capture_t *capture;
   host_original_documentary *documentary;
};
static void documentary_capture_cancelled(void *opaque)
{
   struct documentary_capture_scope *scope = opaque;
   host_original_documentary_release(&scope->documentary);
   server_session_capture_release(scope->capture);
}
__attribute__((used, externally_visible, retain)) int
server_session_capture_documentary_current(server_session_capture_t **owner,
                                           host_original_documentary **documentary)
{
   if (!owner || *owner || !documentary || *documentary)
      return -1;
   if (!current)
      return 0;
   if (!host_original_documentary_retain || !host_original_documentary_release)
      return -1;
   int previous;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous))
      return -1;
   struct documentary_capture_scope scope = {0};
   int result = server_session_capture_retain_current(current->execution_session,
                                                      current->principal, &scope.capture);
   if (!result && scope.capture)
   {
      int already_locked = scope.capture == documentary_read_lock_owner && current == scope.capture;
      if (!already_locked)
         pthread_mutex_lock(&scope.capture->lock);
      if (atomic_load(&scope.capture->closed) || atomic_load(&scope.capture->failed) ||
          scope.capture->original_post_in_progress)
         result = -1;
      else if (scope.capture->original_documentary)
         result = host_original_documentary_retain(scope.capture->original_documentary,
                                                   &scope.documentary);
      if (!already_locked)
         pthread_mutex_unlock(&scope.capture->lock);
   }
   if (result || !scope.documentary)
   {
      host_original_documentary_release(&scope.documentary);
      server_session_capture_release(scope.capture);
      scope.capture = NULL;
   }
   /* Retain both while pending cancellation is delivered; transfer only after. */
   pthread_cleanup_push(documentary_capture_cancelled, &scope);
   pthread_setcancelstate(previous, NULL);
   if (previous == PTHREAD_CANCEL_ENABLE)
      pthread_testcancel();
   pthread_cleanup_pop(0);
   *owner = scope.capture;
   *documentary = scope.documentary;
   return result;
}

__attribute__((used, externally_visible, retain)) const char *
server_session_capture_principal(const server_session_capture_t *owner)
{
   return owner ? owner->principal : NULL;
}

/* This exact pair boundary never follows later callback appends. It carries
 * delivery position only, with no original-input or publication authority. */
__attribute__((used, externally_visible, retain)) int
server_session_capture_original_input_before(server_session_capture_t *owner, char before[32])
{
   if (!owner || !before)
      return -1;
   before[0] = '\0';
   pthread_mutex_lock(&owner->lock);
   int result = -1;
   if (!atomic_load(&owner->closed) && !atomic_load(&owner->failed) &&
       !owner->original_post_in_progress && owner->original_input_sequence >= 2)
   {
      snprintf(before, 32, "%llu", owner->original_input_sequence - 2);
      result = 0;
   }
   pthread_mutex_unlock(&owner->lock);
   return result;
}
