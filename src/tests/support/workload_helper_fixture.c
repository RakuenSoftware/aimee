/* Stand-in workload helpers for test_kb_workload_helper_posix, built once per
 * behaviour with -DFIXTURE_<NAME>. The helper is launched by descriptor with a
 * fixed argv[0] and an empty environment, which distro tools do not all accept
 * (Ubuntu 26.04's multi-call coreutils refuses it), so the test owns these. */
#include <unistd.h>

extern char **environ;

#if !defined(FIXTURE_TRUE)
static int write_all(const char *buf, size_t len)
{
   while (len)
   {
      ssize_t n = write(STDOUT_FILENO, buf, len);
      if (n <= 0)
         return -1;
      buf += n;
      len -= (size_t)n;
   }
   return 0;
}
#endif

int main(void)
{
#if defined(FIXTURE_CAT)
   char buf[4096];
   ssize_t n;
   while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0)
      if (write_all(buf, (size_t)n) != 0)
         return 1;
   return n == 0 ? 0 : 1;
#elif defined(FIXTURE_ENV)
   for (char **e = environ; *e; ++e)
   {
      size_t len = 0;
      while ((*e)[len])
         ++len;
      if (write_all(*e, len) != 0 || write_all("\n", 1) != 0)
         return 1;
   }
   return 0;
#elif defined(FIXTURE_YES)
   while (write_all("y\n", 2) == 0)
      ;
   return 1;
#elif defined(FIXTURE_TRUE)
   return 0;
#else
#error "define one of FIXTURE_CAT, FIXTURE_ENV, FIXTURE_YES, FIXTURE_TRUE"
#endif
}
