#include <aimee/core/connection/native_tls.h>

#include <stdio.h>
#include <sys/stat.h>

int aimee_tls_client_cert_eligible(const char *home, char *crt, size_t crt_n, char *key,
                                   size_t key_n)
{
   if (!home || !home[0] || !crt || crt_n == 0 || !key || key_n == 0)
      return 0;
   int crt_len = snprintf(crt, crt_n, "%s/tls/client.crt", home);
   int key_len = snprintf(key, key_n, "%s/tls/client.key", home);
   if (crt_len <= 0 || (size_t)crt_len >= crt_n || key_len <= 0 || (size_t)key_len >= key_n)
      return -1;
   struct stat cert_status, key_status;
   int have_cert = stat(crt, &cert_status) == 0;
#ifndef _WIN32
   int have_key = lstat(key, &key_status) == 0;
#else
   int have_key = stat(key, &key_status) == 0;
#endif
   if (!have_cert && !have_key)
      return 0;
   if (!have_cert || !have_key)
      return -1;
#ifndef _WIN32
   if (!S_ISREG(key_status.st_mode) || (key_status.st_mode & 077) != 0)
      return -1;
#endif
   return 1;
}
