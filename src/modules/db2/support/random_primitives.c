#include "db2_random.h"

#ifdef _WIN32
#include <bcrypt.h>
#elif defined(__linux__) && !defined(AIMEE_TEST_DB2_RANDOM_IO_SEAM_H)
#include <errno.h>
#include <sys/random.h>
#endif
#include <stdio.h>
#include <string.h>

#ifndef DB2_RANDOM_FOPEN
#define DB2_RANDOM_FOPEN fopen
#endif
#ifndef DB2_RANDOM_FREAD
#define DB2_RANDOM_FREAD fread
#endif
#ifndef DB2_RANDOM_FCLOSE
#define DB2_RANDOM_FCLOSE fclose
#endif

int platform_random_bytes(void *buf, size_t len)
{
#ifdef _WIN32
   NTSTATUS status =
       BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
   return (status >= 0) ? 0 : -1;
#else
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
   FILE *f = DB2_RANDOM_FOPEN("/dev/urandom", "r");
   if (!f)
      return -1;
   size_t n = DB2_RANDOM_FREAD(buf, 1, len, f);
   DB2_RANDOM_FCLOSE(f);
   return (n == len) ? 0 : -1;
#endif
}

int platform_random_hex(char *out, size_t hex_len)
{
   if (!out || hex_len == 0 || (hex_len % 2) != 0)
      return -1;

   size_t raw_len = hex_len / 2;
   unsigned char raw[256];
   if (raw_len > sizeof(raw))
      return -1;

   if (platform_random_bytes(raw, raw_len) != 0)
   {
      out[0] = '\0';
      return -1;
   }

   for (size_t i = 0; i < raw_len; i++)
      snprintf(out + i * 2, 3, "%02x", raw[i]);
   out[hex_len] = '\0';
   return 0;
}
