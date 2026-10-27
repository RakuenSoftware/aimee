/* Pin the legacy server's deliberately narrow handoff to the Go egress binary. */
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"
#include "platform_test_util.h"
#include "server/delegate_egress_adapter.h"

void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
{
   (void)level;
   (void)module;
   (void)fmt;
}

static void test_rejects_non_unix_listener(void)
{
   static const char head[] = "GET http://deb.debian.org/ HTTP/1.1\r\n\r\n";
   assert(delegate_egress_adapter_serve(7, 0, head, sizeof(head) - 1, "test") == -1);
}

static void test_hands_exact_bytes_and_unix_fd_to_helper(void)
{
   char helper[256], record[256];
   snprintf(helper, sizeof(helper), "%s/aimee-egress-adapter-%d.sh", platform_tmpdir(),
            (int)getpid());
   snprintf(record, sizeof(record), "%s/aimee-egress-adapter-%d.request", platform_tmpdir(),
            (int)getpid());
   unlink(record);
   FILE *f = fopen(helper, "w");
   assert(f != NULL);
   fprintf(f,
           "#!/bin/sh\n"
           "[ \"$1\" = proxy ] || exit 10\n"
           "[ -S /proc/self/fd/3 ] || exit 11\n"
           "cat > '%s'\n"
           "printf 'HTTP/1.1 204 No Content\\r\\nConnection: close\\r\\n\\r\\n' >&3\n",
           record);
   fclose(f);
   assert(chmod(helper, 0700) == 0);
   setenv("AIMEE_DELEGATE_EGRESS_BIN", helper, 1);

   int pair[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
   static const char head[] =
       "GET http://deb.debian.org/debian/ HTTP/1.1\r\nHost: deb.debian.org\r\n\r\n";
   assert(delegate_egress_adapter_serve(pair[0], 1, head, sizeof(head) - 1, "test") == 0);

   char response[128] = "";
   ssize_t n = read(pair[1], response, sizeof(response) - 1);
   assert(n > 0);
   response[n] = '\0';
   assert(strstr(response, "204 No Content") != NULL);

   f = fopen(record, "r");
   assert(f != NULL);
   char observed[sizeof(head)] = "";
   size_t got = fread(observed, 1, sizeof(observed), f);
   fclose(f);
   assert(got == sizeof(head) - 1);
   assert(memcmp(observed, head, sizeof(head) - 1) == 0);

   close(pair[0]);
   close(pair[1]);
   unsetenv("AIMEE_DELEGATE_EGRESS_BIN");
   unlink(helper);
   unlink(record);
}

static int cancel_owned(void *context)
{
   int *polls = context;
   return ++*polls >= 5;
}
static long long now_ms(void)
{
   struct timespec now;
   assert(!clock_gettime(CLOCK_MONOTONIC, &now));
   return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static void test_owned_deadline_and_cancellation_join(void)
{
   char helper[256], record[256];
   snprintf(helper, sizeof helper, "%s/aimee-owned-egress-%d.sh", platform_tmpdir(), (int)getpid());
   snprintf(record, sizeof record, "%s/aimee-owned-egress-%d.pid", platform_tmpdir(),
            (int)getpid());
   FILE *file = fopen(helper, "w");
   assert(file);
   fprintf(file, "#!/bin/sh\nprintf '%%s' $$ > '%s'\nexec sleep 20\n", record);
   assert(!fclose(file) && !chmod(helper, 0700));
   assert(!setenv("AIMEE_DELEGATE_EGRESS_BIN", helper, 1));
   static const char head[] = "CONNECT deb.debian.org:443 HTTP/1.1\r\n\r\n";
   for (int cancellation = 0; cancellation < 2; cancellation++)
   {
      int pair[2], polls = 0;
      assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
      long long start = now_ms();
      assert(delegate_egress_adapter_serve_owned(pair[0], head, sizeof head - 1, "owned-test",
                                                 cancellation ? 5000 : 100,
                                                 cancellation ? cancel_owned : NULL, &polls) == -1);
      assert(now_ms() - start < 3000);
      file = fopen(record, "r");
      assert(file);
      long child = 0;
      assert(fscanf(file, "%ld", &child) == 1 && child > 1);
      fclose(file);
      errno = 0;
      assert(kill((pid_t)child, 0) == -1 && errno == ESRCH);
      close(pair[0]);
      close(pair[1]);
   }
   int tcp = socket(AF_INET, SOCK_STREAM, 0);
   assert(tcp >= 0);
   assert(delegate_egress_adapter_serve_owned(tcp, head, sizeof head - 1, "foreign", 100, NULL,
                                              NULL) == -1);
   close(tcp);
   unsetenv("AIMEE_DELEGATE_EGRESS_BIN");
   unlink(helper);
   unlink(record);
}

int main(void)
{
   test_rejects_non_unix_listener();
   test_hands_exact_bytes_and_unix_fd_to_helper();
   test_owned_deadline_and_cancellation_join();
   puts("delegate_egress_adapter: all tests passed");
   return 0;
}
