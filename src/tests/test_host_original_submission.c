#include "server/host_original_submission.h"
#include "cJSON.h"
#include "aimee_sha256.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>
typedef struct
{
   char *original;
   int active;
} owner;
static int mode, authenticated;
static atomic_int captures, consumes, finishes, destroys, admitted;
static _Thread_local owner *live;
static void *captured_dom_string;
static size_t captured_dom_length;
static int wiped_dom;
static void inspect_json_free(void *p)
{
   if (p && p == captured_dom_string)
   {
      const unsigned char *bytes = p;
      for (size_t i = 0; i < captured_dom_length; ++i)
         assert(bytes[i] == 0);
      wiped_dom++;
   }
   free(p);
}
static host_text_status capture(const char *headers, const cJSON *request, void **out, void *state)
{
   (void)state;
   assert(headers && !strcmp(headers, "synthetic headers"));
   atomic_fetch_add(&captures, 1);
   if (!authenticated)
      return HOST_TEXT_REFUSED;
   const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "message"));
   if (!text)
      return HOST_TEXT_INVALID;
   if (mode == 20)
   {
      captured_dom_string = (void *)text;
      captured_dom_length = strlen(text);
   }
   owner *o = calloc(1, sizeof *o);
   assert(o);
   o->original = strdup(text);
   assert(o->original);
   *out = o;
   if (mode == 8)
      assert(!pthread_cancel(pthread_self()));
   return HOST_TEXT_OK;
}
static host_text_status consume(void *pointer, const cJSON *request,
                                host_original_submission_result *out, void *state)
{
   (void)state;
   owner *o = pointer;
   atomic_fetch_add(&consumes, 1);
   const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "message"));
   if (!text || strcmp(text, o->original))
      return HOST_TEXT_REFUSED;
   assert(!live);
   live = o;
   o->active = 1;
   out->text = strdup("normalized safe text");
   out->attachment = cJSON_CreateObject();
   assert(out->text && out->attachment);
   out->text_len = strlen(out->text);
   out->text_capacity = out->text_len + 1;
   char digest[65];
   assert(!aimee_sha256_hex(out->text, strlen(out->text), digest));
   cJSON_AddNumberToObject(out->attachment, "schema_version", 1);
   cJSON_AddStringToObject(out->attachment, "kind", "host.original-input.v1");
   cJSON_AddStringToObject(out->attachment, "operation_uuid",
                           "12345678-1234-4234-8234-123456789abc");
   cJSON_AddStringToObject(
       out->attachment, "text_sha256",
       mode == 4 ? "0000000000000000000000000000000000000000000000000000000000000000" : digest);
   cJSON_AddStringToObject(out->attachment, "opaque", "opaque unchanged");
   if (mode == 11)
   {
      free(out->text);
      out->text = malloc(16);
      assert(out->text);
      memset(out->text, 'x', 16);
      out->text_len = 15;
      out->text_capacity = 16;
   }
   if (mode == 12)
   {
      free(out->text);
      out->text = malloc(HOST_TEXT_MAX_OUTPUT + 2);
      assert(out->text);
      memset(out->text, 'x', HOST_TEXT_MAX_OUTPUT + 1);
      out->text[HOST_TEXT_MAX_OUTPUT + 1] = 0;
      out->text_len = HOST_TEXT_MAX_OUTPUT + 1;
      out->text_capacity = HOST_TEXT_MAX_OUTPUT + 2;
   }
   if (mode == 13)
      out->text[5] = 0;
   if (mode == 14)
      out->text_capacity = 0;
   if (mode == 7 || mode == 10)
      assert(!pthread_cancel(pthread_self()));
   return mode == 3 ? HOST_TEXT_UNKNOWN : HOST_TEXT_OK;
}
static void finish(void *pointer, int accepted, void *state)
{
   (void)state;
   owner *o = pointer;
   if (o->active)
   {
      assert(live == o);
      live = NULL;
      o->active = 0;
   }
   atomic_fetch_add(&finishes, 1);
   if (accepted)
      atomic_fetch_add(&admitted, 1);
}
static void destroy(void *pointer, void *state)
{
   (void)state;
   owner *o = pointer;
   assert(!o->active);
   free(o->original);
   free(o);
   atomic_fetch_add(&destroys, 1);
}
static host_original_submission_owner ops = {capture, consume, finish, destroy};
static host_text_status capture_request(const cJSON *request, host_original_submission **out)
{
   char *frame = cJSON_PrintUnformatted(request);
   assert(frame);
   host_text_status status;
   pthread_cleanup_push(free, frame);
   status = host_original_submission_capture("synthetic headers", frame, strlen(frame), out);
   pthread_cleanup_pop(1);
   return status;
}
typedef struct
{
   host_original_submission *cap;
   cJSON *request;
   host_text_status status;
   host_original_submission_result out;
} box;
static void *worker(void *pointer)
{
   box *b = pointer;
   b->status = host_original_submission_consume(b->cap, b->request, &b->out);
   if (b->status == HOST_TEXT_OK)
      host_original_submission_finish(b->cap, 1);
   host_original_submission_result_destroy(&b->out);
   assert(!live);
   return NULL;
}
static void *capture_worker(void *pointer)
{
   box *b = pointer;
   b->status = capture_request(b->request, &b->cap);
   return NULL;
}
static void *disabled_worker(void *pointer)
{
   box *b = pointer;
   int old;
   assert(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old));
   assert(host_original_submission_consume(b->cap, b->request, &b->out) == HOST_TEXT_OK);
   host_original_submission_finish(b->cap, 0);
   host_original_submission_result_destroy(&b->out);
   assert(!live);
   assert(!pthread_setcancelstate(old, NULL));
   pthread_testcancel();
   return NULL;
}
static void run(int selected)
{
   mode = selected;
   assert(!host_original_submission_required());
   authenticated = 1;
   if (mode == 20)
   {
      cJSON_Hooks hooks = {malloc, inspect_json_free};
      cJSON_InitHooks(&hooks);
   }
   cJSON *req = cJSON_Parse("{\"message\":\"synthetic original\",\"human\":true}");
   assert(req);
   host_original_submission *cap = NULL;
   host_original_submission_result out = {0};
   if (mode == 0)
   {
      assert(host_original_submission_capture(NULL, NULL, SIZE_MAX, &cap) ==
                 HOST_TEXT_PASSTHROUGH &&
             !cap);
      assert(capture_request(req, &cap) == HOST_TEXT_PASSTHROUGH && !cap);
      assert(host_original_submission_install(&ops, NULL) == -1);
      goto done;
   }
   assert(!host_original_submission_install(&ops, NULL));
   assert(host_original_submission_required() == 1);
   assert(host_original_submission_install(&ops, NULL) == -1);
   if (mode == 1)
   {
      authenticated = 0;
      assert(capture_request(req, &cap) == HOST_TEXT_REFUSED && !cap);
      assert(!atomic_load(&consumes));
      goto done;
   }
   if (mode == 8)
   {
      box b = {.request = req};
      pthread_t t;
      assert(!pthread_create(&t, NULL, capture_worker, &b));
      void *result;
      assert(!pthread_join(t, &result) && result == PTHREAD_CANCELED);
      assert(!b.cap && atomic_load(&destroys) == 1);
      goto done;
   }
   if (mode >= 15 && mode < 20)
   {
      const char *bad[] = {"{\"message\":\"prefix\\u0000tail\"}",
                           "{\"message\":\"one\",\"message\":\"two\"}",
                           "{\"message\":\"value\"} {}", "{\"message\":\"\xc0\xaf\"}", "[]"};
      const char *frame = bad[mode - 15];
      assert(host_original_submission_capture("synthetic headers", frame, strlen(frame), &cap) ==
                 HOST_TEXT_INVALID &&
             !cap);
      assert(!atomic_load(&captures));
      goto done;
   }
   assert(capture_request(req, &cap) == HOST_TEXT_OK && cap);
   if (mode == 2)
   {
      host_original_submission **old = host_original_submission_pending_bind(&cap);
      host_original_submission *taken = host_original_submission_pending_take();
      assert(taken && !cap && !host_original_submission_pending_take());
      host_original_submission_pending_bind(old);
      cap = taken;
      host_original_submission_finish(cap, 1);
      assert(!atomic_load(&finishes));
   }
   if (mode == 5)
      cJSON_ReplaceItemInObjectCaseSensitive(req, "message",
                                             cJSON_CreateString("changed original"));
   if (mode == 6)
   {
      box a = {.cap = cap, .request = req}, b = a;
      pthread_t x, y;
      assert(!pthread_create(&x, NULL, worker, &a) && !pthread_create(&y, NULL, worker, &b));
      assert(!pthread_join(x, NULL) && !pthread_join(y, NULL));
      assert((a.status == HOST_TEXT_OK) + (b.status == HOST_TEXT_OK) == 1);
      assert(atomic_load(&consumes) == 1 && atomic_load(&finishes) == 1 &&
             atomic_load(&admitted) == 1);
      goto release;
   }
   if (mode == 7 || mode == 10)
   {
      box b = {.cap = cap, .request = req};
      pthread_t t;
      assert(!pthread_create(&t, NULL, mode == 10 ? disabled_worker : worker, &b));
      void *result;
      assert(!pthread_join(t, &result) && result == PTHREAD_CANCELED);
      assert(!b.out.text && !b.out.attachment && atomic_load(&finishes) == 1 &&
             !atomic_load(&admitted));
      goto release;
   }
   if (mode == 20)
   {
      assert(wiped_dom == 1);
      goto release;
   }
   if (mode == 9)
      goto release; /* Queued but never consumed. */
   host_text_status result = host_original_submission_consume(cap, req, &out);
   if (mode == 3 || mode == 4 || mode == 5 || mode >= 11)
   {
      assert(result == (mode == 3   ? HOST_TEXT_UNKNOWN
                        : mode == 5 ? HOST_TEXT_REFUSED
                                    : HOST_TEXT_INVALID));
      assert(!out.text && !out.attachment && !live && atomic_load(&finishes) == 1);
   }
   else
   {
      assert(result == HOST_TEXT_OK && out.text && out.attachment && live);
      host_original_submission_finish(cap, 1);
      host_original_submission_finish(cap, 1);
      assert(!live && atomic_load(&finishes) == 1 && atomic_load(&admitted) == 1);
   }
   assert(host_original_submission_consume(cap, req, &(host_original_submission_result){0}) ==
          HOST_TEXT_REFUSED);
   host_original_submission_result_destroy(&out);
release:
   host_original_submission_destroy(&cap);
   assert(!cap && atomic_load(&destroys) == 1);
done:
   cJSON_Delete(req);
   printf("PASS generic submission case %d\n", mode);
}
int main(void)
{
   for (int i = 0; i < 21; i++)
   {
      pid_t p = fork();
      assert(p >= 0);
      if (!p)
      {
         alarm(5);
         run(i);
         exit(0);
      }
      int status;
      assert(waitpid(p, &status, 0) == p && WIFEXITED(status) && WEXITSTATUS(status) == 0);
   }
   puts("PASS one-use submission custody, transfer and cancellation (21 cases)");
   return 0;
}
