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
server_input_committed_delivery_completed_receipt_v1(const server_input_committed_delivery_cap_t *cap)
{
   return cap && cap==active_delivery_cap ? cap->completed_receipt : NULL;
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
                                        char **owned_wire, db1_original_input_plan_receipt_t *out_plan)
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
   if (input_plan_required && !cap.plan_bound) return NULL;
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
                                     const char *expected_operation, const char *expected_id, char **completed_receipt)
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
   if (completed_receipt && completed && strcmp(outcome,"delivered"))
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
   cap.completed_receipt = completed && completed_receipt && !strcmp(outcome,"delivered") ? saved_result : NULL;
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
      cJSON *saved=aimee_ir_json_parse_frame(saved_result,strlen(saved_result));
      cJSON *recovered=aimee_ir_json_parse_frame(receipt,strlen(receipt));
      int same=status==SERVER_INPUT_DELIVERY_DELIVERED && !strcmp(outcome,"delivered") &&
         saved && recovered && cJSON_Compare(saved,recovered,1);
      cJSON_Delete(saved);cJSON_Delete(recovered);
      if(same && completed_receipt){*completed_receipt=strdup(saved_result);result=*completed_receipt?1:-1;}
      goto done;
   }
   if (db1_session_original_input_delivery_ack(
           id, receiver, actor, key,
           status == SERVER_INPUT_DELIVERY_DELIVERED ? "delivered" : "terminal", receipt))
      goto done;
   if(completed_receipt){*completed_receipt=strdup(receipt);if(!*completed_receipt)goto done;}
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
       rc == 1 ? 0 : (rc == 0 ? delivery_reconcile_record(wire, session, operation, NULL, NULL) : -1);
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
