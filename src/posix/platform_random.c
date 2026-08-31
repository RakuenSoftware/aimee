/* platform_random.c: descriptor-independent random bytes on Linux, with a
 * portable /dev/urandom fallback for older/non-Linux kernels. */
#include "platform_random.h"
#include <errno.h>
#include <stdio.h>
#if defined(__linux__) && !defined(AIMEE_TEST_DB2_RANDOM_IO_SEAM_H)
#include <sys/random.h>
#endif

int platform_random_bytes(void *buf, size_t len)
{
   if (!buf && len != 0)
      return -1;
#if defined(__linux__) && !defined(AIMEE_TEST_DB2_RANDOM_IO_SEAM_H)
   size_t offset = 0;
   while (offset < len)
   {
      ssize_t n = getrandom((unsigned char *)buf + offset, len - offset, 0);
      if (n > 0)
      {
         offset += (size_t)n;
         continue;
      }
      if (n < 0 && errno == EINTR)
         continue;
      if (n < 0 && errno == ENOSYS)
         break;
      return -1;
   }
   if (offset == len)
      return 0;
#endif

   FILE *f = fopen("/dev/urandom", "r");
   if (!f)
      return -1;
   size_t n = fread(buf, 1, len, f);
   fclose(f);
   return (n == len) ? 0 : -1;
}
