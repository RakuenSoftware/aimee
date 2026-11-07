/* Synthetic canonical DB fixture shared with the session logger tests. This
 * test does not install a production human resolver or private signer. */
#include "db1_client/primary_sessions.h"
#define main retained_fixture_main
#include "test_server_session_log.c"
#undef main
#include "../server/host_input_attachment.h"
#include "../server/original_input_created.h"
#include "../server/original_input_delivery.h"
#include "aimee_sha256.h"
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
const unsigned aimee_original_input_attachment_contract_v1 = 1;
static int executions, admissions, batch_calls, db_mode, installed_rows;
static int post_mode;
static cJSON *minted;
static char *stored_wire;
static char stored_turn[128];
static int random_calls;
int __wrap_RAND_bytes(unsigned char *p, int n)
{
   memset(p, ++random_calls, (size_t)n);
   return 1;
}
void db1_agent_job_set_agent(int n, const char *s)
{
   (void)n;
   (void)s;
}
static int admit(const char *session, const char *turn, const char *principal,
                 const aimee_message_t *m, void *ctx)
{
   (void)turn;
   (void)ctx;
   assert(!strcmp(session, "prototype") && !strcmp(principal, "alice"));
   admissions++;
   return m->blocks[0].input_attachment == minted ? 0 : -1;
}
int db1_session_original_input_begin(const char *session, const char *operation, const char *turn,
                                     const char *actor, const char *wire,
                                     db1_original_input_receipt_t *receipt)
{
   char sequence[32] = {0};
   memset(receipt, 0, sizeof *receipt);
   batch_calls++;
   assert(!strcmp(operation, "12345678-1234-4234-8234-123456789abc"));
   if (stored_wire)
   {
      assert(strcmp(stored_turn, turn));
      return strcmp(stored_wire, wire) ? -2 : 1;
   }
   if (db_mode == 2)
      return -1; /* Unknown without an acknowledged commit. */
   cJSON *batch = cJSON_Parse(wire);
   assert(batch && cJSON_GetArraySize(batch) == 3);
   const cJSON *begin = cJSON_GetObjectItemCaseSensitive(batch, "begin"),
               *input = cJSON_GetObjectItemCaseSensitive(batch, "input");
   char *a = cJSON_PrintUnformatted(begin), *b = cJSON_PrintUnformatted(input);
   assert(a && b);
   assert(
       !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(begin, "kind")), "execution"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
                      cJSON_GetObjectItemCaseSensitive(begin, "payload"), "status")),
                  "running"));
   /* Explicit DB transaction fixture: validate both, then commit their logical
    * original event order together. No PostgreSQL/rollback certification. */
   assert(!db1_session_ir_append(session, "original:begin", turn, actor, a, sequence));
   assert(!db1_session_ir_append(session, "original:input", turn, actor, b, sequence));
   installed_rows += 2;
   receipt->begin_sequence = 1;
   receipt->input_sequence = 2;
   if (post_mode == 7)
      receipt->input_sequence = 4;
   if (post_mode == 8)
   {
      receipt->begin_sequence = UINT64_MAX;
      receipt->input_sequence = 0;
   }
   receipt->contract_version = 1;
   receipt->outcome = DB1_ORIGINAL_INPUT_CREATED;
   snprintf(receipt->original_turn, sizeof receipt->original_turn, "%s", turn);
   stored_wire = strdup(wire);
   snprintf(stored_turn, sizeof stored_turn, "%s", turn);
   free(a);
   free(b);
   cJSON_Delete(batch);
   return db_mode == 1 ? -1 : 0; /* Lost ACK after actual fixture commit. */
}
/* The executor boundary is deliberately represented by this counter. Runtime
 * capture/logger implementations are linked normally; no dispatch copy is used.
 */
static int run(const cJSON *attachment)
{
   server_session_log_t *scope = NULL;
   int rc =
       server_session_log_begin_original("safe original", attachment, "fixture", "", "", &scope);
   if (rc)
   {
      assert(!scope);
      return rc;
   }
   executions++;
   model_reply();
   return server_session_log_finish(scope, 1, NULL);
}
static int owner_cookie, created_calls;
static server_session_capture_t *cancel_owner;
static char cancel_turn[128];
static int cancel_observed;
static const server_input_created_cap_t *saved_cap;
static void cancelled_scope_closed(int success, void *resource)
{
   (void)resource;
   assert(!success);
   server_input_created_view_t v = {0};
   assert(server_input_created_view(saved_cap, &v) == -1);
   cancel_observed++;
}
static int owner_admit(const char *session, const char *turn, const char *principal,
                       const aimee_message_t *m, const void **cookie, void *state)
{
   (void)state;
   assert(session && turn && principal);
   if (m->blocks[0].input_attachment != minted)
      return -1;
   *cookie = post_mode == 4 ? NULL : &owner_cookie;
   return 0;
}
static int created(const server_input_created_cap_t *cap, void *state)
{
   (void)state;
   created_calls++;
   saved_cap = cap;
   server_input_created_view_t v = {0};
   assert(!server_input_created_view(cap, &v));
   assert(v.schema_version == 1 && v.owner_cookie == &owner_cookie &&
          !strcmp(v.session, "prototype") && !strcmp(v.principal, "alice") &&
          !strcmp(v.text, "safe original") && v.text_len == 13 && v.receipt.begin_sequence == 1 &&
          v.receipt.input_sequence == 2 && v.receipt.contract_version == 1 &&
          v.receipt.outcome == DB1_ORIGINAL_INPUT_CREATED);
   // This accessor takes the writer mutex: callback would deadlock under it.
   server_session_capture_counts_t counts = {0};
   assert(!server_session_capture_counts(server_session_capture_current(), &counts) &&
          counts.counter == 2);
   assert(server_input_created_view((const server_input_created_cap_t *)1, &v) == -1);
   if (post_mode == 10)
   {
      assert(!server_session_capture_retain_current("prototype", "alice", &cancel_owner) &&
             cancel_owner);
      snprintf(cancel_turn, sizeof cancel_turn, "%s", server_session_capture_turn(cancel_owner));
      assert(!server_session_capture_after_terminal(cancel_owner, cancelled_scope_closed, NULL));
      assert(!pthread_cancel(pthread_self()));
      pthread_testcancel(); /* Disabled by the handoff: callback returns normally.
                             */
      assert(!server_input_created_view(cap, &v));
   }
   if (post_mode == 5)
   {
      aimee_session_event_t event = {.kind = AIMEE_SESSION_EXECUTION, .execution_status = "failed"};
      assert(server_session_capture_append(server_session_capture_current(), &event, NULL, NULL) ==
             -1);
   }
   return post_mode == 1 ? 1 : post_mode == 2 ? 2 : 0;
}
static void minted_setup(void)
{
   minted = cJSON_CreateObject();
   char sha[65];
   assert(!aimee_sha256_hex("safe original", 13, sha));
   cJSON_AddNumberToObject(minted, "schema_version", 1);
   cJSON_AddStringToObject(minted, "kind", "host.original-input.v1");
   cJSON_AddStringToObject(minted, "operation_uuid", "12345678-1234-4234-8234-123456789abc");
   cJSON_AddStringToObject(minted, "text_sha256", sha);
   cJSON_AddStringToObject(minted, "opaque", "opaque-source-fixture");
}
static void *cancel_worker(void *unused)
{
   (void)unused;
   attest("prototype", "alice");
   server_session_log_t *scope = NULL;
   (void)server_session_log_begin_original("safe original", minted, "fixture", "", "", &scope);
   assert(!"pending cancellation returned to executor");
   return NULL;
}
static void run_case(int mode)
{
   post_mode = mode;
   attest("prototype", "alice");
   minted_setup();
   assert(!server_input_attachment_install_owner(owner_admit, created, NULL));
   assert(server_input_created_owner_configured());
   assert(server_input_attachment_install(admit, NULL) == -1);
   server_input_created_view_t view = {0};
   assert(server_input_created_view((const server_input_created_cap_t *)1, &view) == -1);
   if (post_mode == 3)
      db_mode = 1;
   if (post_mode == 6)
      db_mode = 2;
   cJSON *supplied = post_mode == 9 ? cJSON_Duplicate(minted, 1) : minted;
   if (post_mode == 10)
   {
      pthread_t thread;
      assert(!pthread_create(&thread, NULL, cancel_worker, NULL));
      void *outcome = NULL;
      assert(!pthread_join(thread, &outcome) && outcome == PTHREAD_CANCELED);
      struct row *r = find("prototype");
      assert(r && cJSON_GetArraySize(r->events) == 2);
      assert(executions == 0 && created_calls == 1 && cancel_observed == 1 && cancel_owner);
      server_session_capture_counts_t counts = {0};
      assert(!server_session_capture_counts(cancel_owner, &counts) && counts.counter == 2);
      assert(!server_session_capture_accepting(cancel_owner));
      assert(server_session_capture_has_owner("prototype", "alice", cancel_turn));
      server_session_capture_release(cancel_owner);
      cancel_owner = NULL;
      assert(!server_session_capture_has_owner("prototype", "alice", cancel_turn));
      assert(!server_session_capture_current() && !server_session_log_active());
      puts("PASS real pthread_cancel: cap cleared, RAM closed, owner released, "
           "events=2, execution=0");
      return;
   }
   int rc = run(supplied);
   struct row *r = find("prototype");
   assert(r);
   if (post_mode == 0)
   {
      assert(!rc && executions == 1 && created_calls == 1 && cJSON_GetArraySize(r->events) == 4);
      assert(server_input_created_view(saved_cap, &view) == -1);
      rc = run(minted);
      assert(rc < 0 && executions == 1 && created_calls == 1 && cJSON_GetArraySize(r->events) == 4);
   }
   else
   {
      assert(rc < 0 && executions == 0);
      int rows = post_mode == 4 || post_mode == 6 || post_mode == 9 ? 0 : 2;
      assert(cJSON_GetArraySize(r->events) == rows);
      assert(created_calls == (post_mode == 1 || post_mode == 2 || post_mode == 5 ? 1 : 0));
      assert(server_input_created_view(saved_cap, &view) == -1);
      assert(!server_session_capture_current() && !server_session_log_active());
   }
   printf("PASS post56 case%d callback=%d execution=%d events=%d\n", post_mode, created_calls,
          executions, cJSON_GetArraySize(r->events));
}

/* Startup installation is process-wide and immutable. Isolate each case using
 * fork, retaining ordinary zero-argument unit-test runner semantics. */
int retained_created_main(void)
{
   for (int mode = 0; mode < 11; mode++)
   {
      pid_t child = fork();
      assert(child >= 0);
      if (!child)
      {
         alarm(5);
         run_case(mode);
         fflush(stdout);
         _exit(0);
      }
      int status = 0;
      assert(waitpid(child, &status, 0) == child);
      assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
   }
   puts("PASS original-input post-created protocol and cancellation (11 cases)");
   return 0;
}

const unsigned aimee_original_input_delivery_client_contract_v1=1;
static int prepare_calls,recover_calls,ack_calls,delivery_case;
static char *delivery_row,*second_delivery_row;
static const server_input_prepared_cap_t *saved_prepared;
static const server_input_committed_delivery_cap_t *saved_delivery;
static char *wire_insert_nul_escape(const char *wire,const char *key)
{
 char needle[128];snprintf(needle,sizeof needle,"\"%s\":\"",key);const char *start=strstr(wire,needle);assert(start);const char *end=strchr(start+strlen(needle),'"');assert(end);const char *extra="\\u0000suffix";size_t prefix=(size_t)(end-wire),n=strlen(wire),e=strlen(extra);char *out=malloc(n+e+1);assert(out);memcpy(out,wire,prefix);memcpy(out+prefix,extra,e);memcpy(out+prefix+e,end,n-prefix+1);return out;
}
static char *wire_duplicate_actor(const char *wire)
{
 const char *needle="\"actor\":\"alice\"",*extra="\"actor\":\"alice\",\"actor\":\"alice\"";const char *at=strstr(wire,needle);assert(at);size_t prefix=(size_t)(at-wire),n=strlen(wire),a=strlen(needle),b=strlen(extra);char *out=malloc(n-a+b+1);assert(out);memcpy(out,wire,prefix);memcpy(out+prefix,extra,b);memcpy(out+prefix+b,at+a,n-prefix-a+1);return out;
}
static char *wire_duplicate_role(const char *wire)
{
 const char *needle="\"role\":\"user\"",*extra="\"role\":\"user\",\"role\":\"user\"";const char *at=strstr(wire,needle);assert(at);size_t prefix=(size_t)(at-wire),n=strlen(wire),a=strlen(needle),b=strlen(extra);char *out=malloc(n-a+b+1);assert(out);memcpy(out,wire,prefix);memcpy(out+prefix,extra,b);memcpy(out+prefix+b,at+a,n-prefix-a+1);return out;
}
static void *prepared_other_thread(void *ptr)
{server_input_prepared_view_t v={0};assert(server_input_prepared_view(ptr,&v)==-1);return NULL;}
static int prepare_delivery(const server_input_prepared_cap_t *cap,char **wire,void *state)
{
 (void)state;server_input_prepared_view_t v={0};assert(!server_input_prepared_view(cap,&v));
 assert(v.owner_cookie==&owner_cookie && !strcmp(v.principal,"alice") && !strcmp(v.text,"safe original") && v.text_len==13 && strlen(v.opaque_sha256)==64);
 assert(server_input_prepared_view((void*)1,&v)==-1);pthread_t other;assert(!pthread_create(&other,NULL,prepared_other_thread,(void*)cap));assert(!pthread_join(other,NULL));saved_prepared=cap;prepare_calls++;
 *wire=strdup("{\"schema_version\":1,\"receiver_tag\":\"source-owner\",\"intent_key\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"opaque_intent\":\"signed-prepared-first\"}");
 if(delivery_case>=11&&delivery_case<=13){const char *keys[]={"receiver_tag","intent_key","opaque_intent"};char *bad=wire_insert_nul_escape(*wire,keys[delivery_case-11]);free(*wire);*wire=bad;}
 if(delivery_case==14){char *bad=malloc(strlen(*wire)+4);assert(bad);strcpy(bad,*wire);strcat(bad," {}");free(*wire);*wire=bad;}
 return 0;
}
int db1_session_original_input_begin_delivery(const char *sid,const char *op,const char *turn,const char *actor,const char *wire,const char *intent,db1_original_input_receipt_t *r,uint64_t *id)
{
 assert(prepare_calls==1 && intent);int rc=db1_session_original_input_begin(sid,op,turn,actor,wire,r);*id=73;
 if(rc!=0)return rc;
 cJSON *batch=cJSON_Parse(wire),*row=cJSON_CreateObject();char *event=cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(batch,"input"));char sha[65];assert(!aimee_sha256_hex(event,strlen(event),sha));
 cJSON_AddNumberToObject(row,"schema_version",1);cJSON_AddStringToObject(row,"delivery_id","73");cJSON_AddStringToObject(row,"session_id",sid);cJSON_AddStringToObject(row,"operation_uuid",op);cJSON_AddStringToObject(row,"actor",actor);cJSON_AddStringToObject(row,"receiver_tag","source-owner");cJSON_AddStringToObject(row,"intent_key","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");cJSON_AddStringToObject(row,"opaque_intent","signed-prepared-first");cJSON_AddStringToObject(row,"begin_sequence","1");cJSON_AddStringToObject(row,"input_sequence","2");cJSON_AddStringToObject(row,"original_turn",turn);cJSON_AddStringToObject(row,"begin_sha256","bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");cJSON_AddStringToObject(row,"input_sha256",sha);cJSON_AddStringToObject(row,"input_event_json",event);cJSON_AddStringToObject(row,"outcome","pending");cJSON_AddStringToObject(row,"result_json","");delivery_row=cJSON_PrintUnformatted(row);free(event);cJSON_Delete(row);cJSON_Delete(batch);return rc;
}
int db1_session_original_input_delivery_get(const char *sid,const char *op,const char *actor,const char *receiver,char **wire)
{(void)op;assert(!strcmp(actor,"alice")&&!strcmp(receiver,"source-owner"));if(delivery_case==9&&!strcmp(sid,"prototype-second")){*wire=strdup(second_delivery_row);return 0;}if(strcmp(sid,"prototype"))return 1;*wire=strdup(delivery_row);return 0;}
int db1_session_original_input_delivery_page(const char *receiver,const char *actor,const char *after,const char *limit,const char *budget,char **out)
{(void)receiver;(void)actor;(void)after;(void)limit;(void)budget;cJSON *p=cJSON_CreateObject(),*rows=cJSON_CreateArray();cJSON_AddNumberToObject(p,"schema_version",1);cJSON_AddItemToArray(rows,cJSON_Parse(delivery_row));if(delivery_case==9)cJSON_AddItemToArray(rows,cJSON_Parse(second_delivery_row));cJSON_AddItemToObject(p,"rows",rows);cJSON_AddStringToObject(p,"next_cursor","73");cJSON_AddBoolToObject(p,"has_more",0);*out=cJSON_PrintUnformatted(p);cJSON_Delete(p);if(delivery_case==26){char *bad=wire_duplicate_actor(*out);free(*out);*out=bad;}return 0;}
int db1_session_original_input_delivery_ack(const char *id,const char *receiver,const char *actor,const char *key,const char *outcome,const char *result)
{
 assert(!strcmp(id,"73")&&!strcmp(receiver,"source-owner")&&!strcmp(actor,"alice")&&strlen(key)==64&&!strcmp(result,"{\"wide\":9007199254740993}"));ack_calls++;
 if(delivery_case==4)return -1;
 cJSON *row=cJSON_Parse(delivery_row);cJSON_ReplaceItemInObjectCaseSensitive(row,"outcome",cJSON_CreateString(outcome));cJSON_ReplaceItemInObjectCaseSensitive(row,"result_json",cJSON_CreateString(result));free(delivery_row);delivery_row=cJSON_PrintUnformatted(row);cJSON_Delete(row);return 0;
}
static server_input_delivery_outcome_t recover_delivery(const server_input_committed_delivery_cap_t *cap,char **result,void *state)
{
 (void)state;server_input_committed_delivery_view_t v={0};assert(!server_input_committed_delivery_view(cap,&v));saved_delivery=cap;recover_calls++;
 assert((v.delivery_id==73||v.delivery_id==74) && v.receipt.begin_sequence==1 && v.receipt.input_sequence==2 && v.receipt.outcome==DB1_ORIGINAL_INPUT_REPLAY && !strcmp(v.text,"safe original") && !strcmp(v.opaque_intent,"signed-prepared-first"));
 server_input_created_view_t created_view={0};assert(server_input_created_view((void*)cap,&created_view)==-1);
 if(delivery_case==5){assert(!pthread_cancel(pthread_self()));pthread_testcancel();return SERVER_INPUT_DELIVERY_UNKNOWN;}
 if(delivery_case==1||(delivery_case==9&&v.delivery_id==74))return SERVER_INPUT_DELIVERY_UNKNOWN;
 *result=strdup("{\"wide\":9007199254740993}");return delivery_case==2?SERVER_INPUT_DELIVERY_TERMINAL:SERVER_INPUT_DELIVERY_DELIVERED;
}
static int delivery_created(const server_input_created_cap_t *cap,void *state)
{
 assert(!created(cap,state));server_input_created_delivery_view_t v={0};assert(!server_input_created_delivery_view(cap,&v)&&v.delivery_id==73&&!strcmp(v.opaque_intent,"signed-prepared-first"));
 return delivery_case==3?server_input_created_delivery_ack_v1(cap,"{\"wide\":9007199254740993}"):0;
}
static void *recovery_cancel_worker(void *unused)
{(void)unused;(void)server_input_delivery_reconcile_one_v1("prototype","12345678-1234-4234-8234-123456789abc");assert(!"pending recovery cancellation returned");return NULL;}
static void delivery_run(int mode)
{
 delivery_case=mode;attest("prototype","alice");minted_setup();assert(!server_input_attachment_install_owner(owner_admit,delivery_created,NULL));
 if(mode==10){server_session_capture_t *early=server_session_capture_open("prototype","early-turn","alice","exec");assert(early);server_session_capture_release(early);assert(server_input_delivery_install_owner_v1("source-owner","alice",prepare_delivery,recover_delivery,NULL)==-1);puts("PASS owner delivery cannot be installed after capture has started");return;}
 assert(!server_input_delivery_install_owner_v1("source-owner",mode==8?"bob":"alice",prepare_delivery,recover_delivery,NULL));assert(server_input_delivery_install_owner_v1("other","alice",prepare_delivery,recover_delivery,NULL)==-1);
 if(mode>=11&&mode<=14){assert(run(minted)<0&&executions==0&&prepare_calls==1&&created_calls==0&&batch_calls==0);puts("PASS malformed prepared intent decoded-NUL/trailing refuses before canonical DB dispatch");return;}
 if(mode==8){assert(run(minted)<0&&executions==0&&prepare_calls==0&&created_calls==0&&batch_calls==0);puts("PASS wrong installed owner denies before prepared callback and canonical DB dispatch");return;}
 assert(!run(minted)&&executions==1&&prepare_calls==1&&created_calls==1);
 server_input_prepared_view_t pv={0};assert(server_input_prepared_view(saved_prepared,&pv)==-1);assert(server_input_created_delivery_ack_v1(saved_cap,"{}")==-1);
 if(mode==5){pthread_t thread;assert(!pthread_create(&thread,NULL,recovery_cancel_worker,NULL));void *status=NULL;assert(!pthread_join(thread,&status)&&status==PTHREAD_CANCELED);assert(recover_calls==1&&ack_calls==0&&executions==1);puts("PASS recovery cancellation clears cap, leaves pending, acknowledges nothing and executes nothing");return;}
 if(mode==6||mode==7){cJSON *row=cJSON_Parse(delivery_row);cJSON_ReplaceItemInObjectCaseSensitive(row,mode==6?"session_id":"actor",cJSON_CreateString(mode==6?"other-session":"bob"));free(delivery_row);delivery_row=cJSON_PrintUnformatted(row);cJSON_Delete(row);}
 if(mode==9){cJSON *row=cJSON_Parse(delivery_row);const char *other="12345678-1234-4234-8234-123456789abd";cJSON_ReplaceItemInObjectCaseSensitive(row,"delivery_id",cJSON_CreateString("74"));cJSON_ReplaceItemInObjectCaseSensitive(row,"session_id",cJSON_CreateString("prototype-second"));cJSON_ReplaceItemInObjectCaseSensitive(row,"operation_uuid",cJSON_CreateString(other));cJSON *event=cJSON_Parse(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row,"input_event_json")));cJSON *payload=cJSON_GetObjectItemCaseSensitive(event,"payload"),*blocks=cJSON_GetObjectItemCaseSensitive(payload,"blocks"),*attachment=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(blocks,0),"input_attachment");cJSON_ReplaceItemInObjectCaseSensitive(attachment,"operation_uuid",cJSON_CreateString(other));char *wire=cJSON_PrintUnformatted(event),sha[65];assert(!aimee_sha256_hex(wire,strlen(wire),sha));cJSON_ReplaceItemInObjectCaseSensitive(row,"input_event_json",cJSON_CreateString(wire));cJSON_ReplaceItemInObjectCaseSensitive(row,"input_sha256",cJSON_CreateString(sha));second_delivery_row=cJSON_PrintUnformatted(row);free(wire);cJSON_Delete(event);cJSON_Delete(row);}
 if(mode>=15&&mode<=18){const char *keys[]={"receiver_tag","intent_key","delivery_id","opaque_intent"};char *bad=wire_insert_nul_escape(delivery_row,keys[mode-15]);free(delivery_row);delivery_row=bad;}
 if(mode==19||mode==21||mode==25){cJSON *row=cJSON_Parse(delivery_row);const char *input=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row,"input_event_json"));char *bad=mode==21?wire_insert_nul_escape(input,"opaque"):wire_duplicate_role(input);char sha[65];assert(!aimee_sha256_hex(bad,strlen(bad),sha));cJSON_ReplaceItemInObjectCaseSensitive(row,"input_event_json",cJSON_CreateString(bad));cJSON_ReplaceItemInObjectCaseSensitive(row,"input_sha256",cJSON_CreateString(sha));if(mode==25){cJSON_ReplaceItemInObjectCaseSensitive(row,"outcome",cJSON_CreateString("delivered"));cJSON_ReplaceItemInObjectCaseSensitive(row,"result_json",cJSON_CreateString("{}"));}free(delivery_row);delivery_row=cJSON_PrintUnformatted(row);free(bad);cJSON_Delete(row);}
 if(mode==20){cJSON *row=cJSON_Parse(delivery_row);cJSON_DeleteItemFromObjectCaseSensitive(row,"begin_sha256");cJSON_AddStringToObject(row,"unknown_authority","ignored");free(delivery_row);delivery_row=cJSON_PrintUnformatted(row);cJSON_Delete(row);}
 if(mode==22){char *bad=malloc(strlen(delivery_row)+4);assert(bad);strcpy(bad,delivery_row);strcat(bad," {}");free(delivery_row);delivery_row=bad;}
 if(mode==23||mode==24){cJSON *row=cJSON_Parse(delivery_row);cJSON_ReplaceItemInObjectCaseSensitive(row,"delivery_id",cJSON_CreateString(mode==23?"18446744073709551616":"9223372036854775808"));free(delivery_row);delivery_row=cJSON_PrintUnformatted(row);cJSON_Delete(row);}
 uint64_t cursor=0;int before=executions;int rc=mode>=15&&mode<=25?server_input_delivery_reconcile_one_v1("prototype","12345678-1234-4234-8234-123456789abc"):server_input_delivery_reconcile_page_v1(0,32,4194304,&cursor);
 if(mode>=15&&mode<=26){fprintf(stderr,"malformed case%d rc%d cursor%llu callbacks%d ack%d\n",mode,rc,(unsigned long long)cursor,recover_calls,ack_calls);assert(rc==-1&&cursor==0&&recover_calls==0&&ack_calls==0);}
 else if(mode==9){assert(rc==-1&&cursor==73&&recover_calls==2&&ack_calls==1);}
 else if(mode==6||mode==7){assert(rc==-1&&cursor==0&&recover_calls==0&&ack_calls==0);}
 else if(mode==1||mode==4){assert(rc==-1&&cursor==0&&recover_calls==1);assert(ack_calls==(mode==4));}
 else {assert(rc==1&&cursor==73);assert(recover_calls==(mode==3?0:1));assert(ack_calls==1);assert(server_input_delivery_reconcile_one_v1("prototype","12345678-1234-4234-8234-123456789abc")==1);assert(recover_calls==(mode==3?0:1));}
 assert(executions==before);server_input_committed_delivery_view_t cv={0};assert(server_input_committed_delivery_view(saved_delivery,&cv)==-1);printf("PASS generic delivery case%d no recovered execution, cap lifetime, fixed owner, exact receipt, completed skip/unknown prefix\n",mode);
}
#ifndef SERVER_INPUT_DELIVERY_TEST_ENTRY
#define SERVER_INPUT_DELIVERY_TEST_ENTRY main
#endif
int SERVER_INPUT_DELIVERY_TEST_ENTRY(void)
{
 assert(!retained_created_main());fflush(stdout);
 for(int mode=0;mode<27;mode++){pid_t child=fork();assert(child>=0);if(!child){alarm(8);delivery_run(mode);fflush(stdout);_exit(0);}int status=0;assert(waitpid(child,&status,0)==child);assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);}
 puts("PASS generic delivery27 +legacy11 actual capture/logger; DB transport is explicit synthetic fixture");return 0;
}
