#define main inherited_submission_fixture_main
#include "test_host_original_submission.c"
#undef main
static int doc_mode, retained_docs, released_docs;
static host_original_submission *held_original;
static int doc_retain(void *pointer, void **document, void *state)
{
   assert(state == &doc_mode);
   owner *o = pointer;
   assert(o && !o->active && !live);
   retained_docs++;
   if (doc_mode == 4 || doc_mode == 9)
      return doc_mode == 4 ? 0 : -1;
   *document = strdup("opaque documentary source only");
   assert(*document);
   if (doc_mode == 10)
   {
      host_original_submission_destroy(&held_original);
      assert(held_original);
   }
   if (doc_mode == 5)
      assert(!pthread_cancel(pthread_self()));
   return doc_mode == 3 ? -1 : 0;
}
static void doc_release(void *document, void *state)
{
   assert(state == &doc_mode);
   assert(!strcmp(document, "opaque documentary source only"));
   released_docs++;
   free(document);
   if (doc_mode == 6)
      assert(!pthread_cancel(pthread_self()));
}
static host_original_documentary_hooks doc_hooks = {1, doc_retain, doc_release};
typedef struct
{
   host_original_submission *original;
   host_original_submission_result result;
   host_original_documentary *doc;
   cJSON *request;
} documentary_scope;
static void scope_cleanup(void *pointer)
{
   documentary_scope *scope = pointer;
   host_original_documentary_release(&scope->doc);
   host_original_submission_destroy(&scope->original);
   host_original_submission_result_destroy(&scope->result);
}
static void *foreign_export(void *pointer)
{
   host_original_documentary *doc = NULL;
   assert(host_original_submission_documentary_retain(pointer, &doc) == HOST_TEXT_REFUSED && !doc);
   return NULL;
}
static void *document_worker(void *pointer)
{
   documentary_scope *scope = pointer;
   pthread_cleanup_push(scope_cleanup, scope);
   assert(host_original_submission_consume(scope->original, scope->request, &scope->result) ==
          HOST_TEXT_OK);
   assert(host_original_submission_documentary_retain(scope->original, &scope->doc) ==
              HOST_TEXT_REFUSED &&
          !scope->doc);
   host_original_submission_finish(scope->original, doc_mode != 2);
   if (doc_mode == 7)
   {
      pthread_t t;
      assert(!pthread_create(&t, NULL, foreign_export, scope->original) && !pthread_join(t, NULL));
   }
   held_original = scope->original;
   host_text_status status =
       host_original_submission_documentary_retain(scope->original, &scope->doc);
   if (doc_mode == 2)
      assert(status == HOST_TEXT_REFUSED && !scope->doc && !retained_docs);
   else if (doc_mode == 0)
      assert(status == HOST_TEXT_UNAVAILABLE && !scope->doc && !retained_docs);
   else if (doc_mode == 3 || doc_mode == 4 || doc_mode == 9)
      assert(status == HOST_TEXT_UNAVAILABLE && !scope->doc);
   else
   {
      assert(status == HOST_TEXT_OK && scope->doc);
      assert(
          !strcmp(host_original_documentary_borrow(scope->doc), "opaque documentary source only"));
      if (doc_mode == 8)
      {
         host_original_documentary *before = scope->doc;
         assert(host_original_submission_documentary_retain(scope->original, &scope->doc) ==
                    HOST_TEXT_INVALID &&
                scope->doc == before);
         assert(host_original_documentary_retain(scope->doc, &scope->doc) == -1 &&
                scope->doc == before);
      }
      host_original_documentary *copy = NULL;
      assert(!host_original_documentary_retain(scope->doc, &copy) && copy == scope->doc);
      host_original_submission_destroy(&scope->original);
      assert(!scope->original);
      host_original_documentary_release(&scope->doc);
      assert(!scope->doc);
      scope->doc = copy;
      assert(
          !strcmp(host_original_documentary_borrow(scope->doc), "opaque documentary source only"));
      host_original_documentary_release(&scope->doc);
   }
   pthread_cleanup_pop(1);
   return NULL;
}
static void documentary_case(int selected)
{
   doc_mode = selected;
   mode = 1;
   authenticated = 1;
   assert(!host_original_submission_documentary_required());
   host_original_documentary *fake = (void *)(uintptr_t)1, *out = NULL;
   assert(!host_original_documentary_borrow(fake));
   assert(host_original_documentary_retain(fake, &out) == -1 && !out);
   host_original_documentary_release(&fake);
   assert(!fake && !released_docs);
   assert(host_original_submission_documentary_retain((void *)(uintptr_t)1, &out) ==
              HOST_TEXT_REFUSED &&
          !out);
   if (selected != 0)
   {
      host_original_documentary_hooks bad = doc_hooks;
      bad.abi_version = 2;
      assert(host_original_submission_install_documentary_hooks(&bad, &doc_mode) == -1);
      assert(!host_original_submission_install_documentary_hooks(&doc_hooks, &doc_mode));
      assert(host_original_submission_documentary_required() == 1);
      assert(host_original_submission_install_documentary_hooks(&doc_hooks, &doc_mode) == -1);
   }
   assert(!host_original_submission_install(&ops, NULL));
   documentary_scope scope = {0};
   scope.request = cJSON_Parse("{\"message\":\"synthetic original\"}");
   assert(scope.request);
   assert(capture_request(scope.request, &scope.original) == HOST_TEXT_OK);
   assert(host_original_submission_install_documentary_hooks(&doc_hooks, &doc_mode) == -1);
   assert(host_original_submission_documentary_retain(scope.original, &scope.doc) ==
              HOST_TEXT_REFUSED &&
          !scope.doc);
   pthread_t t;
   void *result;
   assert(!pthread_create(&t, NULL, document_worker, &scope) && !pthread_join(t, &result));
   fprintf(stderr, "documentary case %d: %s\n", selected,
           result == PTHREAD_CANCELED ? "cancelled" : "returned");
   assert(result == ((selected == 5 || selected == 6) ? PTHREAD_CANCELED : NULL));
   assert(!scope.original && !scope.doc && !scope.result.text && destroys == 1 && finishes == 1);
   assert(retained_docs == (selected == 0 || selected == 2 ? 0 : 1));
   assert(released_docs ==
          (selected == 0 || selected == 2 || selected == 4 || selected == 9 ? 0 : 1));
   cJSON_Delete(scope.request);
}
int main(void)
{
   int modes[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
   for (size_t i = 0; i < sizeof modes / sizeof *modes; i++)
   {
      pid_t p = fork();
      assert(p >= 0);
      if (!p)
      {
         alarm(10);
         documentary_case(modes[i]);
         exit(0);
      }
      int status;
      assert(waitpid(p, &status, 0) == p && WIFEXITED(status) && WEXITSTATUS(status) == 0);
   }
   puts("PASS documentary: 11 "
        "lifecycle/absence/refusal/forgery/thread/cancellation/pin cases");
   return 0;
}
