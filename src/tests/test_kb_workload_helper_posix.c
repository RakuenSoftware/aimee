#include "kb/kb_workload_helper_posix.h"

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *find_native(const char *a, const char *b)
{
   struct stat st;
   return stat(a, &st) == 0 ? a : b;
}

int main(void)
{
#if !defined(__linux__)
   int fd = -1;
   assert(kb_workload_helper_open("/usr/bin/cat", &fd) == KB_WORKLOAD_HELPER_DISABLED);
   puts("kb_workload_helper_posix: disabled");
   return 0;
#else
   int fd = 99;
   assert(kb_workload_helper_open(NULL, &fd) == KB_WORKLOAD_HELPER_INVALID && fd == -1);
   assert(kb_workload_helper_open("usr/bin/cat", &fd) == KB_WORKLOAD_HELPER_INVALID && fd == -1);
   assert(kb_workload_helper_open("/usr//bin/cat", &fd) == KB_WORKLOAD_HELPER_INVALID && fd == -1);
   assert(kb_workload_helper_open("/usr/../bin/cat", &fd) == KB_WORKLOAD_HELPER_INVALID &&
          fd == -1);
   assert(kb_workload_helper_open("/etc/hosts", &fd) == KB_WORKLOAD_HELPER_INTEGRITY && fd == -1);
   assert(kb_workload_helper_open("/definitely-missing-aimee-workload-helper", &fd) ==
              KB_WORKLOAD_HELPER_UNAVAILABLE &&
          fd == -1);

   const char *cat = find_native("/usr/bin/cat", "/bin/cat");
   assert(kb_workload_helper_open(cat, &fd) == KB_WORKLOAD_HELPER_OK && fd > 2);
   static const unsigned char request[] = "descriptor-pinned duplex";
   unsigned char response[128];
   memset(response, 0xa5, sizeof(response));
   size_t response_len = 99;
   assert(kb_workload_helper_invoke(fd, request, sizeof(request) - 1, response, sizeof(response),
                                    &response_len, 1000) == KB_WORKLOAD_HELPER_OK);
   assert(response_len == sizeof(request) - 1 && !memcmp(response, request, response_len));

   sigset_t sigpipe_set, old_mask, pending;
   sigemptyset(&sigpipe_set);
   sigaddset(&sigpipe_set, SIGPIPE);
   assert(pthread_sigmask(SIG_BLOCK, &sigpipe_set, &old_mask) == 0);
   assert(raise(SIGPIPE) == 0);
   response_len = 99;
   assert(kb_workload_helper_invoke(fd, request, sizeof(request) - 1, response, sizeof(response),
                                    &response_len, 1000) == KB_WORKLOAD_HELPER_OK);
   assert(sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1);
   int received_signal = 0;
   assert(sigwait(&sigpipe_set, &received_signal) == 0 && received_signal == SIGPIPE);
   assert(pthread_sigmask(SIG_SETMASK, &old_mask, NULL) == 0);
   close(fd);

   const char *env = find_native("/usr/bin/env", "/bin/env");
   assert(kb_workload_helper_open(env, &fd) == KB_WORKLOAD_HELPER_OK);
   memset(response, 0xa5, sizeof(response));
   response_len = 99;
   assert(kb_workload_helper_invoke(fd, NULL, 0, response, sizeof(response), &response_len, 1000) ==
          KB_WORKLOAD_HELPER_OK);
   assert(response_len == 0); /* explicit empty envp */
   close(fd);

   const char *yes = find_native("/usr/bin/yes", "/bin/yes");
   assert(kb_workload_helper_open(yes, &fd) == KB_WORKLOAD_HELPER_OK);
   memset(response, 0xa5, sizeof(response));
   response_len = 99;
   assert(kb_workload_helper_invoke(fd, NULL, 0, response, 8, &response_len, 1000) ==
          KB_WORKLOAD_HELPER_UNAVAILABLE);
   assert(response_len == 0);
   for (size_t i = 0; i < 8; ++i)
      assert(response[i] == 0);
   close(fd);

   /* A helper that exits without reading must not deliver SIGPIPE to the KB.
    * A full-sized request ensures the nonblocking pipe cannot accept it all
    * before the child closes its read end. */
   const char *true_path = find_native("/usr/bin/true", "/bin/true");
   assert(kb_workload_helper_open(true_path, &fd) == KB_WORKLOAD_HELPER_OK);
   unsigned char closed_request[KB_WORKLOAD_HELPER_FRAME_MAX];
   memset(closed_request, 0x5a, sizeof(closed_request));
   int observed_closed_pipe = 0;
   for (int attempt = 0; attempt < 64 && !observed_closed_pipe; ++attempt)
   {
      response_len = 99;
      kb_workload_helper_result_t closed_result =
          kb_workload_helper_invoke(fd, closed_request, sizeof(closed_request), response,
                                    sizeof(response), &response_len, 1000);
      assert(closed_result == KB_WORKLOAD_HELPER_OK ||
             closed_result == KB_WORKLOAD_HELPER_UNAVAILABLE);
      observed_closed_pipe = closed_result == KB_WORKLOAD_HELPER_UNAVAILABLE;
      assert(response_len == 0 && response[0] == 0);
   }
   /* On kernels whose pipe accepts the complete frame, `true` can exit after
    * the write and the call is legitimately OK. Other attempts exercise EPIPE;
    * either way, reaching here proves a close cannot terminate the process. */
   (void)observed_closed_pipe;
   close(fd);

   assert(kb_workload_checked_root_file_open("/etc/hosts", 0, &fd) == KB_WORKLOAD_HELPER_OK);
   close(fd);
   memset(response, 0xa5, sizeof(response));
   response_len = 99;
   assert(kb_workload_helper_invoke(-1, request, sizeof(request) - 1, response, sizeof(response),
                                    &response_len, 1000) == KB_WORKLOAD_HELPER_INVALID);
   assert(response_len == 0 && response[0] == 0);
   assert(kb_workload_helper_invoke(3, request, sizeof(request) - 1, response, sizeof(response),
                                    &response_len, KB_WORKLOAD_HELPER_TIMEOUT_MAX_MS + 1) ==
          KB_WORKLOAD_HELPER_INVALID);
   puts("kb_workload_helper_posix: all tests passed");
   return 0;
#endif
}
