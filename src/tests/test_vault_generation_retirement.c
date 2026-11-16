#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vault_store.h"
#include "vault_internal.h"
#include "vault_service.h"
#include "cJSON.h"
#include <aimee/core/codec/base64.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/file.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned file_syncs, dir_syncs, callbacks;
static int fail_file, fail_dir, crash_dir, fail_parent;
static unsigned parent_syncs;
static struct stat home_stat;
static uint8_t key[VAULT_KEK_LEN];
static const char *principal = "uid:73101", *agent = "opaque-erasure-provider";
static _Thread_local int cancellation_worker;
static int cancellation_mode, cancellation_attempt[2], cancellation_release[2];
static atomic_int cancellation_once;
static atomic_int cancellation_returned __attribute__((unused));
extern int __real_pthread_mutex_lock(pthread_mutex_t *);
extern int __real_flock(int, int);
static void cancel_pause(int mode)
{
   if (cancellation_worker && cancellation_mode == mode && !atomic_exchange(&cancellation_once, 1))
   {
      char c = 'a';
      assert(write(cancellation_attempt[1], &c, 1) == 1);
      if (mode == 3)
         assert(read(cancellation_release[0], &c, 1) == 1);
   }
}
int __wrap_pthread_mutex_lock(pthread_mutex_t *m)
{
   cancel_pause(1);
   return __real_pthread_mutex_lock(m);
}
int __wrap_flock(int fd, int op)
{
   if (op & LOCK_EX)
      cancel_pause(2);
   return __real_flock(fd, op);
}
extern int __real_fsync(int);
int __wrap_fsync(int fd)
{
   cancel_pause(3);
   struct stat st;
   assert(!fstat(fd, &st));
   if (S_ISDIR(st.st_mode))
   {
      dir_syncs++;
      if (crash_dir)
         _exit(72);
      if (fail_dir)
      {
         fail_dir--;
         errno = EIO;
         return -1;
      }
      if (st.st_dev == home_stat.st_dev && st.st_ino == home_stat.st_ino)
      {
         parent_syncs++;
         if (fail_parent)
         {
            fail_parent--;
            errno = EIO;
            return -1;
         }
      }
   }
   else
   {
      file_syncs++;
      if (fail_file)
      {
         fail_file--;
         errno = EIO;
         return -1;
      }
   }
   return __real_fsync(fd);
}
static int use(const char *v, size_t n, void *ctx)
{
   (void)ctx;
   assert(n == strlen("synthetic erasure test value") &&
          !memcmp(v, "synthetic erasure test value", n));
   callbacks++;
   return 0;
}
static void create(const char *slot, char gen[VAULT_IMMUTABLE_GENERATION_CAP])
{
   assert(vault_store_create_server_once(
              principal, key, agent, slot, "synthetic erasure test value", gen,
              VAULT_IMMUTABLE_GENERATION_CAP) == VAULT_CREATE_ONCE_CREATED);
}
static void retired(const char *slot, const char *gen)
{
   unsigned before = callbacks;
   assert(vault_store_use_server_generation_fenced(principal, key, agent, slot, gen, use, NULL) ==
          VAULT_STORE_RETIRED);
   assert(before == callbacks);
   char ignored[VAULT_IMMUTABLE_GENERATION_CAP];
   assert(vault_store_create_server_once(principal, key, agent, slot,
                                         "synthetic erasure test value", ignored,
                                         sizeof ignored) == VAULT_CREATE_ONCE_RETIRED);
}
static void reexec(const char *self, const char *home, const char *slot, const char *gen)
{
   pid_t child = fork();
   assert(child >= 0);
   if (!child)
   {
      execl(self, self, "reopen", home, slot, gen, (char *)NULL);
      _exit(127);
   }
   int st;
   assert(waitpid(child, &st, 0) == child && WIFEXITED(st) && !WEXITSTATUS(st));
}
static int start_pipe[2], release_pipe[2];
static int held(const char *v, size_t n, void *ctx)
{
   assert(!use(v, n, ctx));
   char c = 's';
   assert(write(start_pipe[1], &c, 1) == 1 && read(release_pipe[0], &c, 1) == 1);
   return 0;
}
static unsigned fallback_calls;
static int unsupported_delete(void *c, const char *p, const char *a, const char *s)
{
   (void)c;
   (void)p;
   (void)a;
   (void)s;
   fallback_calls++;
   return 0;
}
static const char *cancel_slot;
static int cancellation_closure;
static char cancel_gen[VAULT_IMMUTABLE_GENERATION_CAP];
static void *cancel_retirement(void *opaque)
{
   if (opaque)
      assert(!pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL));
   cancellation_worker = 1;
   if (cancellation_closure)
   {
      char generation[VAULT_IMMUTABLE_GENERATION_CAP];
      (void)vault_store_close_server_slot_once(principal, agent, cancel_slot, generation,
                                               sizeof generation);
   }
   else
      (void)vault_store_retire_server_generation(principal, agent, cancel_slot, cancel_gen);
   atomic_store(&cancellation_returned, 1);
   return NULL;
}
static void *held_thread(void *opaque)
{
   (void)opaque;
   assert(!vault_store_use_server_generation_fenced(principal, key, agent, cancel_slot, cancel_gen,
                                                    held, NULL));
   return NULL;
}
static void cancellation_modes_preserved(void)
{
   int saved_state, saved_type;
   assert(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &saved_state));
   assert(!pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &saved_type));
   const int states[] = {PTHREAD_CANCEL_ENABLE, PTHREAD_CANCEL_DISABLE};
   const int types[] = {PTHREAD_CANCEL_DEFERRED, PTHREAD_CANCEL_ASYNCHRONOUS};
   for (int operation = 0; operation < 3; operation++)
      for (size_t state = 0; state < 2; state++)
         for (size_t type = 0; type < 2; type++)
         {
            char slot[64], gen[VAULT_IMMUTABLE_GENERATION_CAP], closed[sizeof gen];
            assert(snprintf(slot, sizeof slot, "cancel-restore-%d-%zu-%zu", operation, state,
                            type) < (int)sizeof slot);
            create(slot, gen);
            assert(!pthread_setcanceltype(types[type], NULL));
            assert(!pthread_setcancelstate(states[state], NULL));
            if (operation == 0)
               assert(vault_store_retire_server_generation(principal, agent, slot, gen) ==
                      VAULT_RETIRE_GENERATION_CONFIRMED);
            else if (operation == 1)
               assert(vault_store_close_server_slot_once(principal, agent, slot, closed,
                                                         sizeof closed) ==
                          VAULT_RETIRE_GENERATION_CONFIRMED &&
                      !strcmp(closed, gen));
            else
               assert(!vault_store_use_server_generation_fenced(principal, key, agent, slot, gen,
                                                                use, NULL));
            int actual_state, actual_type;
            assert(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &actual_state));
            assert(!pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &actual_type));
            assert(actual_state == states[state] && actual_type == types[type]);
            assert(vault_store_retire_server_generation(principal, agent, slot, gen) ==
                   VAULT_RETIRE_GENERATION_CONFIRMED);
            retired(slot, gen);
         }
   assert(!pthread_setcanceltype(saved_type, NULL));
   assert(!pthread_setcancelstate(saved_state, NULL));
   puts("PASS vault guarded operations preserve caller cancellation state/type (12 cases)");
}

static void cancellation_tests(const char *home)
{
   for (cancellation_closure = 0; cancellation_closure < 2; cancellation_closure++)
      for (int async = 0; async < 2; async++)
         for (int mode = 1; mode <= 3; mode++)
         {
            char slot[64];
            assert(snprintf(slot, sizeof slot, "cancel-%d-%d-%d", cancellation_closure, async,
                            mode) < (int)sizeof slot);
            cancel_slot = slot;
            create(slot, cancel_gen);
            cancellation_mode = mode;
            atomic_store(&cancellation_once, 0);
            atomic_store(&cancellation_returned, 0);
            assert(!pipe(cancellation_attempt) && !pipe(cancellation_release));
            pthread_t holder, worker;
            pid_t child = 0;
            int held_ready[2], held_release[2];
            if (mode == 1)
            {
               assert(!pipe(start_pipe) && !pipe(release_pipe));
               assert(!pthread_create(&holder, NULL, held_thread, NULL));
               char c;
               assert(read(start_pipe[0], &c, 1) == 1);
            }
            if (mode == 2)
            {
               assert(!pipe(held_ready) && !pipe(held_release));
               child = fork();
               assert(child >= 0);
               if (!child)
               {
                  char path[1024];
                  assert(snprintf(path, sizeof path, "%s/.vault/.write.lock", home) <
                         (int)sizeof path);
                  int fd = open(path, O_RDWR | O_CLOEXEC);
                  assert(fd >= 0 && !__real_flock(fd, LOCK_EX));
                  char c = 'h';
                  assert(write(held_ready[1], &c, 1) == 1 && read(held_release[0], &c, 1) == 1);
                  assert(!__real_flock(fd, LOCK_UN));
                  close(fd);
                  _exit(0);
               }
               char c;
               assert(read(held_ready[0], &c, 1) == 1);
            }
            assert(!pthread_create(&worker, NULL, cancel_retirement, async ? (void *)1 : NULL));
            char c;
            assert(read(cancellation_attempt[0], &c, 1) == 1);
            assert(!pthread_cancel(worker));
            c = 'r';
            if (mode == 1)
            {
               assert(write(release_pipe[1], &c, 1) == 1);
               assert(!pthread_join(holder, NULL));
            }
            if (mode == 2)
            {
               assert(write(held_release[1], &c, 1) == 1);
               int status;
               assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                      !WEXITSTATUS(status));
            }
            if (mode == 3)
               assert(write(cancellation_release[1], &c, 1) == 1);
            void *result = NULL;
            assert(!pthread_join(worker, &result));
            if (result != PTHREAD_CANCELED || atomic_load(&cancellation_returned))
               fprintf(stderr,
                       "vault cancellation closure=%d async=%d phase=%d cancelled=%d "
                       "null_result=%d returned=%d\n",
                       cancellation_closure, async, mode, result == PTHREAD_CANCELED,
                       result == NULL, atomic_load(&cancellation_returned));
            assert(result == PTHREAD_CANCELED && !atomic_load(&cancellation_returned));
            assert(vault_store_retire_server_generation(principal, agent, slot, cancel_gen) ==
                   VAULT_RETIRE_GENERATION_CONFIRMED);
            retired(slot, cancel_gen);
            close(cancellation_attempt[0]);
            close(cancellation_attempt[1]);
            close(cancellation_release[0]);
            close(cancellation_release[1]);
         }
   puts("PASS actual deferred+async cancel while mutex/flock/fsync:0successACK, no stranded lock, "
        "exact-generation reconcile then0freshcallbacks");
}

static vault_retire_generation_result_t broken_close(void *ctx, const char *p, const char *a,
                                                     const char *s, char *g, size_t n)
{
   (void)p;
   (void)a;
   (void)s;
   (void)g;
   (void)n;
   return ctx ? VAULT_RETIRE_GENERATION_MISSING : VAULT_RETIRE_GENERATION_CONFIRMED;
}
static int close_reentry(const char *value, size_t length, void *context)
{
   char generation[VAULT_IMMUTABLE_GENERATION_CAP];
   assert(vault_store_close_server_slot_once(principal, agent, "reentry-close", generation,
                                             sizeof generation) == VAULT_RETIRE_GENERATION_BADARG);
   assert(!generation[0]);
   return use(value, length, context);
}
static void capacity_test(const char *home)
{
   const char *owner = "uid:73104";
   char gen[VAULT_IMMUTABLE_GENERATION_CAP], closed[VAULT_IMMUTABLE_GENERATION_CAP];
   assert(vault_store_create_server_once(owner, key, agent, "retained",
                                         "synthetic erasure test value", gen,
                                         sizeof gen) == VAULT_CREATE_ONCE_CREATED);
   char encoded[256] = {0}, path[1024], raw[4096];
   size_t written = 0;
   assert(aimee_codec_base64_encode((const uint8_t *)owner, strlen(owner), encoded, sizeof encoded,
                                    AIMEE_BASE64_URL, &written));
   assert(snprintf(path, sizeof path, "%s/.vault/%.*s.json", home, (int)written, encoded) <
          (int)sizeof path);
   FILE *file = fopen(path, "rb");
   assert(file);
   size_t n = fread(raw, 1, sizeof raw - 1, file);
   assert(n > 0 && n < sizeof raw - 1);
   assert(!fclose(file));
   raw[n] = 0;
   cJSON *root = cJSON_Parse(raw);
   assert(root && cJSON_AddStringToObject(root, "padding", ""));
   char *base = cJSON_PrintUnformatted(root);
   assert(base);
   const size_t limit = 8u * 1024u * 1024u, target = limit - 1;
   size_t padding_size = target - strlen(base);
   free(base);
   char *padding = malloc(padding_size + 1);
   assert(padding);
   memset(padding, 'x', padding_size);
   padding[padding_size] = 0;
   cJSON *value = cJSON_CreateString(padding);
   free(padding);
   assert(value && cJSON_ReplaceItemInObjectCaseSensitive(root, "padding", value));
   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   assert(json && strlen(json) == target);
   file = fopen(path, "wb");
   assert(file);
   assert(fwrite(json, 1, target, file) == target && !fflush(file) && !fsync(fileno(file)) &&
          !fclose(file));
   assert(vault_store_close_server_slot_once(owner, agent, "would-exceed-reader", closed,
                                             sizeof closed) == VAULT_RETIRE_GENERATION_IO &&
          !closed[0]);
   file = fopen(path, "rb");
   assert(file);
   char *retained = malloc(limit);
   assert(retained);
   n = fread(retained, 1, limit, file);
   assert(!fclose(file));
   assert(n == target && !memcmp(retained, json, target));
   free(retained);
   free(json);
   assert(!vault_store_use_server_generation_fenced(owner, key, agent, "retained", gen, use, NULL));
   puts("PASS actual near8MiB valid vault refuses growth before rename, byte-identical old file "
        "remains readable");
}
static void slot_closure_tests(const char *home, const char *self)
{
   char gen[VAULT_IMMUTABLE_GENERATION_CAP], again[VAULT_IMMUTABLE_GENERATION_CAP];
   const char *fresh = "uid:73103";
   unsigned parents = parent_syncs;
   assert(vault_store_close_server_slot_once(fresh, agent, "never-created", gen, sizeof gen) ==
          VAULT_RETIRE_GENERATION_CONFIRMED);
   assert(strlen(gen) == 32 && parent_syncs > parents);
   assert(vault_store_create_server_once(fresh, key, agent, "never-created", "synthetic value",
                                         again, sizeof again) == VAULT_CREATE_ONCE_RETIRED);
   assert(vault_store_set_server(fresh, key, agent, "never-created", "synthetic value") != 0);
   assert(!vault_store_delete(fresh, agent, "never-created"));
   assert(vault_store_close_server_slot_once(fresh, agent, "never-created", again, sizeof again) ==
              VAULT_RETIRE_GENERATION_CONFIRMED &&
          !strcmp(gen, again));
   puts("PASS absent vault/slot closure is durable, stable and survives ordinary delete");

   create("unknown-create-ack", gen);
   unsigned uses = callbacks;
   assert(vault_service_close_server_wrap_slot_once(principal, agent, "unknown-create-ack", again,
                                                    sizeof again) == VAULT_OK &&
          !strcmp(gen, again));
   retired("unknown-create-ack", gen);
   assert(callbacks == uses);
   create("reentry-close", gen);
   assert(!vault_store_use_server_generation_fenced(principal, key, agent, "reentry-close", gen,
                                                    close_reentry, NULL));
   assert(vault_store_close_server_slot_once(principal, agent, "reentry-close", again,
                                             sizeof again) == VAULT_RETIRE_GENERATION_CONFIRMED &&
          !strcmp(gen, again));
   retired("reentry-close", gen);
   assert(!vault_store_set_server(principal, key, agent, "legacy-close", "synthetic value"));
   assert(vault_store_close_server_slot_once(principal, agent, "legacy-close", again,
                                             sizeof again) == VAULT_RETIRE_GENERATION_MISMATCH &&
          !again[0]);
   assert(vault_store_create_server_once(principal, key, agent, "unknown-create-ack",
                                         "changed value", again,
                                         sizeof again) == VAULT_CREATE_ONCE_RETIRED);
   assert(vault_store_close_server_slot_once("server", agent, "slot", again, sizeof again) ==
          VAULT_RETIRE_GENERATION_UNSUPPORTED);
   assert(vault_service_close_server_wrap_slot_once(NULL, agent, "slot", again, sizeof again) ==
              VAULT_ERR_UNATTESTED &&
          !again[0]);
   assert(vault_store_close_server_slot_once(principal, agent, "bad", again, 32) ==
              VAULT_RETIRE_GENERATION_BADARG &&
          !again[0]);
   puts("PASS unknown live generation closes without plaintext; legacy/auth/bounds refuse");

   fail_file = 1;
   assert(vault_store_close_server_slot_once(principal, agent, "file-unknown", gen, sizeof gen) ==
              VAULT_RETIRE_GENERATION_IO &&
          !gen[0]);
   assert(vault_store_close_server_slot_once(principal, agent, "file-unknown", gen, sizeof gen) ==
          VAULT_RETIRE_GENERATION_CONFIRMED);
   fail_dir = 1;
   assert(vault_store_close_server_slot_once(principal, agent, "dir-unknown", gen, sizeof gen) ==
              VAULT_RETIRE_GENERATION_IO &&
          !gen[0]);
   fail_parent = 1;
   assert(vault_store_close_server_slot_once(principal, agent, "dir-unknown", gen, sizeof gen) ==
              VAULT_RETIRE_GENERATION_IO &&
          !gen[0]);
   assert(vault_store_close_server_slot_once(principal, agent, "dir-unknown", gen, sizeof gen) ==
          VAULT_RETIRE_GENERATION_CONFIRMED);
   retired("dir-unknown", gen);
   fail_parent = 1;
   assert(vault_store_close_server_slot_once(principal, agent, "parent-unknown", gen, sizeof gen) ==
              VAULT_RETIRE_GENERATION_IO &&
          !gen[0]);
   fail_parent = 1;
   assert(vault_store_close_server_slot_once(principal, agent, "parent-unknown", gen, sizeof gen) ==
              VAULT_RETIRE_GENERATION_IO &&
          !gen[0]);
   assert(vault_store_close_server_slot_once(principal, agent, "parent-unknown", gen, sizeof gen) ==
          VAULT_RETIRE_GENERATION_CONFIRMED);
   retired("parent-unknown", gen);
   /* Matching old write/replay paths must also refuse a failed parent barrier. */
   fail_parent = 1;
   assert(vault_store_retire_server_generation(principal, agent, "parent-unknown", gen) ==
          VAULT_RETIRE_GENERATION_IO);
   assert(vault_store_retire_server_generation(principal, agent, "parent-unknown", gen) ==
          VAULT_RETIRE_GENERATION_CONFIRMED);
   puts("PASS file/vault-directory/home-directory failures stay unknown until all barriers replay");

   int release[2];
   assert(!pipe(release));
   pid_t delayed = fork();
   assert(delayed >= 0);
   if (!delayed)
   {
      char c;
      assert(read(release[0], &c, 1) == 1);
      assert(vault_store_create_server_once(principal, key, agent, "delayed-create",
                                            "synthetic value", again,
                                            sizeof again) == VAULT_CREATE_ONCE_RETIRED);
      _exit(0);
   }
   assert(vault_store_close_server_slot_once(principal, agent, "delayed-create", gen, sizeof gen) ==
          VAULT_RETIRE_GENERATION_CONFIRMED);
   char c = 'r';
   assert(write(release[1], &c, 1) == 1);
   int status;
   assert(waitpid(delayed, &status, 0) == delayed && WIFEXITED(status) && !WEXITSTATUS(status));
   close(release[0]);
   close(release[1]);
   retired("delayed-create", gen);
   puts("PASS actual process prepared before closure cannot create after confirmed tombstone");

   pid_t crash = fork();
   assert(crash >= 0);
   if (!crash)
   {
      crash_dir = 1;
      (void)vault_store_close_server_slot_once(principal, agent, "close-crash", gen, sizeof gen);
      _exit(99);
   }
   assert(waitpid(crash, &status, 0) == crash && WIFEXITED(status) && WEXITSTATUS(status) == 72);
   pid_t reopened = fork();
   assert(reopened >= 0);
   if (!reopened)
   {
      execl(self, self, "reopen-close", home, "close-crash", (char *)NULL);
      _exit(127);
   }
   assert(waitpid(reopened, &status, 0) == reopened && WIFEXITED(status) && !WEXITSTATUS(status));
   puts("PASS actual close-after-rename process crash requires renewed barriers in fresh process");

   vault_store_backend_t unsupported = {.name = "unsupported close", .delete = unsupported_delete};
   vault_store_set_backend(&unsupported);
   assert(vault_store_close_server_slot_once(principal, agent, "slot", gen, sizeof gen) ==
              VAULT_RETIRE_GENERATION_UNSUPPORTED &&
          !gen[0] && !fallback_calls);
   unsupported.close_server_slot_once = broken_close;
   assert(vault_store_close_server_slot_once(principal, agent, "slot", gen, sizeof gen) ==
              VAULT_RETIRE_GENERATION_IO &&
          !gen[0]);
   unsupported.ctx = (void *)1;
   assert(vault_store_close_server_slot_once(principal, agent, "slot", gen, sizeof gen) ==
              VAULT_RETIRE_GENERATION_IO &&
          !gen[0]);
   vault_store_set_backend(NULL);
   puts("PASS optional provider refuses without fallback or trusting missing/malformed "
        "confirmation");
   capacity_test(home);
}
int main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IOLBF, 0);
   memset(key, 0x51, sizeof key);
   if (argc == 4 && !strcmp(argv[1], "reopen-close"))
   {
      assert(!setenv("AIMEE_HOME", argv[2], 1) && !stat(argv[2], &home_stat));
      char gen[VAULT_IMMUTABLE_GENERATION_CAP];
      fail_parent = 1;
      assert(vault_store_close_server_slot_once(principal, agent, argv[3], gen, sizeof gen) ==
                 VAULT_RETIRE_GENERATION_IO &&
             !gen[0]);
      assert(vault_store_close_server_slot_once(principal, agent, argv[3], gen, sizeof gen) ==
             VAULT_RETIRE_GENERATION_CONFIRMED);
      retired(argv[3], gen);
      return 0;
   }
   if (argc == 5 && !strcmp(argv[1], "reopen"))
   {
      assert(!setenv("AIMEE_HOME", argv[2], 1));
      assert(!stat(argv[2], &home_stat));
      fail_dir = 1;
      assert(vault_store_delete(principal, agent, argv[3]) == -1);
      assert(file_syncs && dir_syncs == 1);
      file_syncs = dir_syncs = 0;
      assert(vault_store_delete(principal, agent, argv[3]) == 0);
      assert(file_syncs && dir_syncs);
      file_syncs = dir_syncs = 0;
      fail_dir = 1;
      assert(vault_store_retire_server_generation(principal, agent, argv[3], argv[4]) ==
             VAULT_RETIRE_GENERATION_IO);
      assert(file_syncs && dir_syncs == 1);
      retired(argv[3], argv[4]);
      file_syncs = dir_syncs = 0;
      fail_file = 1;
      assert(vault_store_retire_server_generation(principal, agent, argv[3], argv[4]) ==
             VAULT_RETIRE_GENERATION_IO);
      assert(file_syncs == 1 && !dir_syncs);
      file_syncs = dir_syncs = 0;
      assert(vault_store_retire_server_generation(principal, agent, argv[3], argv[4]) ==
             VAULT_RETIRE_GENERATION_CONFIRMED);
      assert(file_syncs && dir_syncs);
      retired(argv[3], argv[4]);
      puts("PASS actual exec/reopen: unknown tombstone refuses on fresh sync faults, confirms only "
           "after file+dir fsync;0callbacks");
      return 0;
   }
   assert(argc == 1);
   char home[512];
   assert(snprintf(home, sizeof home, "%s/erasure-XXXXXX",
                   getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") < (int)sizeof home &&
          mkdtemp(home));
   assert(!setenv("AIMEE_HOME", home, 1));
   assert(!stat(home, &home_stat));
   char gen[VAULT_IMMUTABLE_GENERATION_CAP];
   create("unknown-directory", gen);
   fail_dir = 1;
   assert(vault_store_delete(principal, agent, "unknown-directory") == -1);
   retired("unknown-directory", gen);
   file_syncs = dir_syncs = 0;
   fail_dir = 1;
   assert(vault_store_delete(principal, agent, "unknown-directory") == -1);
   assert(file_syncs && dir_syncs);
   file_syncs = dir_syncs = 0;
   assert(vault_store_delete(principal, agent, "unknown-directory") == 0);
   assert(file_syncs && dir_syncs);
   retired("unknown-directory", gen);
   file_syncs = dir_syncs = 0;
   assert(vault_store_retire_server_generation(principal, agent, "unknown-directory", gen) ==
          VAULT_RETIRE_GENERATION_CONFIRMED);
   assert(file_syncs && dir_syncs);
   puts("PASS same legacy delete OLDFAIL->fixedPASS: immutable retry refuses renewed fsyncfault, "
        "then both barriers before OK");
   /* Provider mechanism only: caller authorization and native withdrawal are separate. */
   const char *wrong = "11111111111111111111111111111111";
   create("live", gen);
   file_syncs = dir_syncs = 0;
   assert(vault_store_retire_server_generation(principal, agent, "live", wrong) ==
              VAULT_RETIRE_GENERATION_MISMATCH &&
          !file_syncs && !dir_syncs);
   assert(!vault_store_use_server_generation_fenced(principal, key, agent, "live", gen, use, NULL));
   fail_file = 1;
   assert(vault_store_retire_server_generation(principal, agent, "live", gen) ==
          VAULT_RETIRE_GENERATION_IO);
   assert(!vault_store_use_server_generation_fenced(principal, key, agent, "live", gen, use, NULL));
   assert(vault_store_retire_server_generation(principal, agent, "live", gen) ==
          VAULT_RETIRE_GENERATION_CONFIRMED);
   retired("live", gen);
   assert(vault_store_retire_server_generation(principal, agent, "live", wrong) ==
          VAULT_RETIRE_GENERATION_MISMATCH);
   assert(vault_store_retire_server_generation(principal, agent, "absent", gen) ==
          VAULT_RETIRE_GENERATION_MISSING);
   assert(vault_store_retire_server_generation("uid:73102", agent, "absent", gen) ==
          VAULT_RETIRE_GENERATION_MISSING);
   assert(vault_store_retire_server_generation(principal, agent, "live", "bad") ==
          VAULT_RETIRE_GENERATION_BADARG);
   assert(vault_service_retire_server_wrap_generation(principal, agent, "absent", gen) ==
          VAULT_NO_ENTRY);
   assert(vault_store_delete(principal, agent, "absent") == 0);
   assert(
       !vault_store_set_server(principal, key, agent, "mutable", "synthetic erasure test value"));
   assert(vault_store_retire_server_generation(principal, agent, "mutable", gen) ==
          VAULT_RETIRE_GENERATION_MISMATCH);
   assert(!vault_store_delete(principal, agent, "mutable") &&
          !vault_store_delete(principal, agent, "mutable"));
   puts("PASS expected generation/missing/invalid fail closed; legacy mutable and absent delete "
        "success unchanged");
   create("post-rename-crash", gen);
   pid_t crash = fork();
   assert(crash >= 0);
   if (!crash)
   {
      crash_dir = 1;
      vault_store_retire_server_generation(principal, agent, "post-rename-crash", gen);
      _exit(99);
   }
   int st;
   assert(waitpid(crash, &st, 0) == crash && WIFEXITED(st) && WEXITSTATUS(st) == 72);
   retired("post-rename-crash", gen);
   reexec(argv[0], home, "post-rename-crash", gen);
   puts("PASS real process crash after rename leaves no fresh use/recreation; exec reconciliation "
        "required");
   create("fenced", gen);
   assert(!pipe(start_pipe) && !pipe(release_pipe));
   pid_t reader = fork();
   assert(reader >= 0);
   if (!reader)
   {
      assert(!vault_store_use_server_generation_fenced(principal, key, agent, "fenced", gen, held,
                                                       NULL));
      _exit(0);
   }
   char c;
   assert(read(start_pipe[0], &c, 1) == 1);
   int attempted[2], finished[2];
   assert(!pipe(attempted) && !pipe(finished));
   pid_t retirement = fork();
   assert(retirement >= 0);
   if (!retirement)
   {
      c = 'a';
      assert(write(attempted[1], &c, 1) == 1);
      assert(vault_store_retire_server_generation(principal, agent, "fenced", gen) ==
             VAULT_RETIRE_GENERATION_CONFIRMED);
      c = 'd';
      assert(write(finished[1], &c, 1) == 1);
      _exit(0);
   }
   assert(read(attempted[0], &c, 1) == 1);
   struct pollfd f = {.fd = finished[0], .events = POLLIN};
   assert(!poll(&f, 1, 100));
   c = 'r';
   assert(write(release_pipe[1], &c, 1) == 1);
   assert(waitpid(reader, &st, 0) == reader && WIFEXITED(st) && !WEXITSTATUS(st));
   assert(read(finished[0], &c, 1) == 1);
   assert(waitpid(retirement, &st, 0) == retirement && WIFEXITED(st) && !WEXITSTATUS(st));
   retired("fenced", gen);
   puts("PASS actual2process retirement waits for same-generation fenced callback "
        "then0futurecallbacks");
   vault_store_backend_t unsupported = {.name = "explicit unsupported",
                                        .delete = unsupported_delete};
   vault_store_set_backend(&unsupported);
   assert(vault_store_retire_server_generation(principal, agent, "slot", gen) ==
              VAULT_RETIRE_GENERATION_UNSUPPORTED &&
          !fallback_calls);
   vault_store_set_backend(NULL);
   puts("PASS missing optional backend capability refuses without legacy-delete fallback");
   slot_closure_tests(home, argv[0]);
   cancellation_tests(home);
   cancellation_modes_preserved();
   return 0;
}
