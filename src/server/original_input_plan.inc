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
static void replay_receipt_cancel(void *value){char **receipt=value;free(*receipt);*receipt=NULL;}
__attribute__((used, externally_visible, retain)) int
server_input_plan_replay_receipt_v1(const void *cookie,char **out)
{
   if(!out || *out) return -1;
   int previous;if(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&previous))return -1;
   server_input_plan_owner_view_t owner={0};char *wire=NULL;int result=-1;
   int rc=plan_owner(cookie,&owner);
   if(!rc)rc=db1_session_original_input_delivery_get(owner.session,owner.operation_uuid,
       delivery_actor,delivery_receiver,&wire);
   if(!rc)result=delivery_reconcile_record(wire,owner.session,owner.operation_uuid,NULL,out);
   server_input_plan_owner_view_t after={0};
   if(result==1 && (plan_owner(cookie,&after) || strcmp(after.session,owner.session) ||
      strcmp(after.operation_uuid,owner.operation_uuid) || strcmp(after.principal,owner.principal)))
      {free(*out);*out=NULL;result=-1;}
   free(wire);
   pthread_cleanup_push(replay_receipt_cancel,out);
   pthread_setcancelstate(previous,NULL);
   if(previous==PTHREAD_CANCEL_ENABLE)pthread_testcancel();
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
