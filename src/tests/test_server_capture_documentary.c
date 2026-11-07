
#define AIMEE_CHAT_HANDOFF_TEST_MAIN inherited_handoff_main
#include "test_server_chat_original_handoff.c"
#undef AIMEE_CHAT_HANDOFF_TEST_MAIN
static int document_cookie, retained_documents, released_documents;
static int document_retain(void *owner, void **document, void *unused)
{
   (void)unused;
   assert(owner == &cookie && finished == 1);
   retained_documents++;
   *document = &document_cookie;
   return 0;
}
static void document_release(void *document, void *unused)
{
   (void)unused;
   assert(document == &document_cookie);
   released_documents++;
}
static void *cancelled_getter(void *opaque)
{
   server_session_capture_bind(opaque);
   assert(!pthread_cancel(pthread_self()));
   server_session_capture_t *held = NULL;
   host_original_documentary *document = NULL;
   (void)server_session_capture_documentary_current(&held, &document);
   assert(!"pending cancellation must unwind owned documentary leases");
   return NULL;
}

#include <stdatomic.h>
#include <sys/mman.h>
static int verify_mode, verify_depth, tool_appends, reader_spawned;
static volatile sig_atomic_t *reentry_seen;
static atomic_int reader_started, reader_done;
static pthread_t reader_thread;
static server_session_capture_t *verify_owner;
static host_original_documentary *verify_document;
static aimee_message_t *verify_message;
static char *cancel_envelope;
static char cancel_sequence[32];
static void documentary_read_assert(server_session_capture_t *expected)
{
   server_session_capture_t *held = NULL;
   host_original_documentary *document = NULL;
   *reentry_seen = 1;
   assert(!server_session_capture_documentary_current(&held, &document));
   assert(held == expected && document);
   assert(host_original_documentary_borrow(document) == &document_cookie);
   host_original_documentary_release(&document);
   server_session_capture_release(held);
}
static void *independent_reader(void *opaque)
{
   server_session_capture_bind(opaque);
   atomic_store(&reader_started, 1);
   documentary_read_assert(opaque);
   atomic_store(&reader_done, 1);
   server_session_capture_bind(NULL);
   return NULL;
}
int server_native_history_verify_message_tool_sources(const aimee_message_t *message)
{
   assert(message == verify_message);
   if (verify_mode == 8)
      assert(!pthread_cancel(pthread_self()));
   documentary_read_assert(server_session_capture_current());
   if (verify_mode == 9 && !verify_depth)
   {
      verify_depth++;
      server_session_capture_t *other = server_session_capture_open("session-one", "nested-tool", "alice", "session-one");
      assert(other);
      server_session_capture_t *prior = server_session_capture_bind(other);
      assert(!server_session_capture_set_documentary(other, verify_document));
      aimee_session_event_t nested = {.kind=AIMEE_SESSION_MESSAGE,.message_id="nested-tool",.message=message};
      assert(!server_session_capture_append(other, &nested, NULL, NULL));
      server_session_capture_bind(prior);
      server_session_capture_close(other); /* Consumes the open reference. */
      documentary_read_assert(prior); /* Nested scope must restore the outer owner. */
      verify_depth--;
   }
   if (verify_mode == 10)
   {
      assert(!pthread_create(&reader_thread, NULL, independent_reader, verify_owner));
      reader_spawned = 1;
      while (!atomic_load(&reader_started)) usleep(1000);
      usleep(10000);
      assert(!atomic_load(&reader_done)); /* Another thread still takes the lock. */
   }
   if (verify_mode == 11)
   {
      server_session_capture_t *other = server_session_capture_open("session-one", "no-document", "alice", "session-one");
      assert(other);
      server_session_capture_t *prior = server_session_capture_bind(other), *held = NULL;
      host_original_documentary *document = NULL;
      assert(!server_session_capture_documentary_current(&held, &document) && !held && !document);
      server_session_capture_bind(prior);
      server_session_capture_close(other); /* Consumes the open reference. */
      documentary_read_assert(prior);
   }
   return verify_mode == 7 ? -1 : 0;
}
int db1_session_tool_source_append(const char *session, const char *event, const char *turn,
                                   const char *actor, const char *wire, char sequence[32])
{
   tool_appends++;
   return db1_session_ir_append(session, event, turn, actor, wire, sequence);
}
static void *cancelled_append(void *opaque)
{
   server_session_capture_bind(opaque);
   aimee_session_event_t event = {.kind=AIMEE_SESSION_MESSAGE,.message_id="cancel-tool",.message=verify_message};
   (void)server_session_capture_append(opaque, &event, &cancel_envelope, cancel_sequence);
   assert(!"pending cancellation must follow unlocked append cleanup");
   return NULL;
}
static void reentrant_case(int which, server_session_capture_t *owner,
                            host_original_documentary *document)
{
   verify_mode=which;verify_owner=owner;verify_document=document;
   char hash[65]; const char *visible="synthetic tool result";
   assert(!aimee_sha256_hex(visible,strlen(visible),hash));
   cJSON *source=cJSON_CreateObject();
   cJSON_AddNumberToObject(source,"schema_version",1);
   cJSON_AddStringToObject(source,"owner","00000000-0000-4000-8000-000000000001");
   cJSON_AddStringToObject(source,"visible_sha256",hash);
   cJSON_AddStringToObject(source,"receipt","{}");
   aimee_block_t block={.type=AIMEE_BLK_TOOL_RESULT,.tool_id=strdup("tool-one"),
      .tool_result=cJSON_CreateString(visible),.tool_source=source};
   aimee_message_t message={.role="tool",.blocks=&block,.n_blocks=1};
   verify_message=&message;
   if(which==8)
   {
      pthread_t thread;void *result=NULL;
      assert(!pthread_create(&thread,NULL,cancelled_append,owner));
      assert(!pthread_join(thread,&result)&&result==PTHREAD_CANCELED);
      assert(!cancel_envelope&&!cancel_sequence[0]&&tool_appends==1);
      documentary_read_assert(owner); /* No stranded writer lock or leaked scope. */
   }
   else
   {
      aimee_session_event_t event={.kind=AIMEE_SESSION_MESSAGE,.message_id="actual-tool",.message=&message};
      int result=server_session_capture_append(owner,&event,NULL,NULL);
      assert(which==7 ? result==-1&&tool_appends==0 : result==0&&tool_appends==(which==9?2:1));
      if(reader_spawned){assert(!pthread_join(reader_thread,NULL));assert(atomic_load(&reader_done));}
      if(which!=7)documentary_read_assert(owner);
      else assert(server_session_capture_failed(owner));
   }
   aimee_block_free_contents(&block);
}

static void capture_case(int which)
{
   selected_case = mode = 1;
   log_entries = cJSON_CreateArray();
   host_original_submission_owner ops = {capture, consume, finish, destroy};
   host_original_documentary_hooks docs = {1, document_retain, document_release};
   assert(!host_original_submission_install(&ops, NULL));
   assert(!host_original_submission_install_documentary_hooks(&docs, NULL));
   assert(!server_input_attachment_install_owner(admit, created, NULL));
   const char *frame = "{\"message\":\"raw synthetic input\",\"aimee_session_id\":\"session-one\",\"ir_version\":0}";
   host_original_submission *cap = NULL;
   assert(host_original_submission_capture("header", frame, strlen(frame), &cap) == HOST_TEXT_OK && cap);
   compute_ctx_t *c = malloc(sizeof *c);
   *c = context();
   c->write_mutex = malloc(sizeof *c->write_mutex);
   assert(!pthread_mutex_init(c->write_mutex, NULL));
   c->conn_fd = -1;
   c->original_submission = cap;
   c->req = cJSON_Parse(frame);
   compute_ctx_lifetime_bind(&c);
   assert(!server_chat_original_submission_prepare(c));
   assert(c->original_admitted && finished == 1 && retained_documents == 1);
   server_session_capture_t *held = NULL;
   host_original_documentary *document = NULL;
   assert(!server_session_capture_documentary_current(&held, &document) && held == c->ir_capture && document);
   assert(!strcmp(server_session_capture_execution_session(held), "session-one"));
   assert(!strcmp(server_session_capture_principal(held), "alice"));
   assert(!strcmp(server_session_capture_turn(held), c->ir_turn));
   assert(host_original_documentary_borrow(document) == &document_cookie);
   assert(server_session_capture_set_documentary(held, document) == -1);
   assert(server_session_capture_documentary_current(&held, &document) == -1);
   if (which == 5)
   {
      pthread_t thread; void *result = NULL;
      assert(!pthread_create(&thread, NULL, cancelled_getter, held));
      assert(!pthread_join(thread, &result) && result == PTHREAD_CANCELED);
      assert(!released_documents);
   }
   if (which >= 6) reentrant_case(which, held, document);
   server_session_capture_t *other = server_session_capture_open("session-one", "independent-turn", "alice", "session-one");
   assert(other);
   assert(server_session_capture_set_documentary(other, document) == -1);
   server_session_capture_t *previous = server_session_capture_bind(other);
   server_session_capture_t *other_held = NULL;
   assert(!server_session_capture_retain_current("session-one", "alice", &other_held) && other_held == other);
   server_session_capture_t *absent = NULL;
   host_original_documentary *no_document = NULL;
   assert(!server_session_capture_documentary_current(&absent, &no_document) && !absent && !no_document);
   if (which == 1)
   {
      assert(!server_session_capture_set_documentary(other, document));
      assert(!server_session_capture_documentary_current(&absent, &no_document) && absent == other && no_document);
      host_original_documentary_release(&no_document);
      server_session_capture_release(absent);
   }
   if (which == 2)
   {
      unsigned long long lease = 0;
      assert(!server_session_capture_model_enter(other, &lease));
      assert(server_session_capture_set_documentary(other, document) == -1);
      assert(!server_session_capture_model_leave(other, lease));
      assert(server_session_capture_set_documentary(other, document) == -1);
   }
   if (which == 3)
      server_session_capture_fail(other);
   if (which == 4)
      server_session_capture_close(other);
   if (which == 3 || which == 4)
   {
      assert(server_session_capture_documentary_current(&absent, &no_document) == (which == 4 ? 0 : -1) && !absent && !no_document);
      assert(server_session_capture_set_documentary(other, document) == -1);
   }
   server_session_capture_bind(previous);
   if (which != 4) server_session_capture_close(other);
   server_session_capture_release(other_held);
   compute_ctx_free(c);
   assert(!c && destroyed == 1 && !released_documents);
   compute_ctx_lifetime_bind(NULL);
   assert(!server_session_capture_current());
   absent = NULL;
   assert(!server_session_capture_documentary_current(&absent, &no_document) && !absent && !no_document);
   assert(host_original_documentary_borrow(document) == &document_cookie);
   server_session_capture_release(held);
   assert(!released_documents);
   host_original_documentary_release(&document);
   assert(!document && released_documents == 1);
   cJSON_Delete(log_entries);
}
int main(void)
{
   reentry_seen=mmap(NULL,sizeof *reentry_seen,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
   assert(reentry_seen!=MAP_FAILED);
   int old=getenv("EXPECT_OLD_REENTRY_BLOCK")!=NULL;
   for (int which = old?6:1; which <= (old?6:11); which++)
   {
      *reentry_seen=0;
      pid_t p=fork();assert(p>=0);
      if(!p){alarm(old?2:5);capture_case(which);exit(0);}
      int status;assert(waitpid(p,&status,0)==p);
      if(old)assert(WIFSIGNALED(status)&&WTERMSIG(status)==SIGALRM&&*reentry_seen);
      else assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
      fprintf(stderr,"capture documentary case %d status %d reached %d\n",which,status,*reentry_seen);
   }
   assert(!munmap((void *)reentry_seen,sizeof *reentry_seen));
   puts(old?"PASS old actual capture blocks in callback documentary_current (bounded watchdog)":"PASS actual capture/documentary reentry/nested/current isolation/cancel/atomic result controls");
   return 0;
}
