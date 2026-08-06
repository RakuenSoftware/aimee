/* aimee_sha256.c: shared SHA-256, backed by OpenSSL.
 *
 * See headers/aimee_sha256.h for why this is substrate rather than a module
 * service. OpenSSL is already a link dependency of every binary built from
 * CORE_SRCS, so this adds no new dependency.
 */
#include "headers/aimee_sha256.h"

#include <openssl/sha.h>

int aimee_sha256_raw(const void *data, size_t len, unsigned char out[32])
{
   if (!out)
      return -1;
   /* A NULL input hashes the empty string, preserving the contract of the
    * workflows-owned wfe_sha256_raw() this replaces. */
   const unsigned char *in = data ? (const unsigned char *)data : (const unsigned char *)"";
   SHA256(in, data ? len : 0, out);
   return 0;
}

int aimee_sha256_hex(const void *data, size_t len, char out_hex[65])
{
   if (!out_hex)
      return -1;
   unsigned char md[32];
   if (aimee_sha256_raw(data, len, md) != 0)
      return -1;
   static const char hx[] = "0123456789abcdef";
   for (int i = 0; i < 32; i++)
   {
      out_hex[i * 2] = hx[md[i] >> 4];
      out_hex[i * 2 + 1] = hx[md[i] & 0x0f];
   }
   out_hex[64] = '\0';
   return 0;
}
