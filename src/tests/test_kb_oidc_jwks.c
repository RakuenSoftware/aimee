/* test_kb_oidc_jwks.c (I10): pure JWKS assembly from fleet key rows. */
#include "kb_oidc_jwks_fleet.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, m)                                                                                \
   do                                                                                              \
   {                                                                                               \
      if (!(c))                                                                                    \
      {                                                                                            \
         printf("FAIL: %s\n", m);                                                                  \
         fails++;                                                                                  \
      }                                                                                            \
   } while (0)

int main(void)
{
   char out[512];
   const char *k1 = "{\"kty\":\"RSA\",\"kid\":\"a\",\"n\":\"x\",\"e\":\"AQAB\"}";
   const char *k2 = "{\"kty\":\"RSA\",\"kid\":\"b\",\"n\":\"y\",\"e\":\"AQAB\"}";
   const char *one[] = {k1};
   const char *two[] = {k1, k2};

   CHECK(kb_oidc_jwks_assemble(one, 1, out, sizeof(out)) == 0, "assemble one ok");
   CHECK(strcmp(out, "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"a\",\"n\":\"x\",\"e\":\"AQAB\"}]}") ==
             0,
         "one-key JWKS");
   CHECK(kb_oidc_jwks_assemble(two, 2, out, sizeof(out)) == 0, "assemble two ok");
   CHECK(strstr(out, "\"kid\":\"a\"") && strstr(out, "\"kid\":\"b\"") &&
             strncmp(out, "{\"keys\":[", 9) == 0,
         "two-key JWKS has both, comma-joined");
   CHECK(kb_oidc_jwks_assemble(NULL, 0, out, sizeof(out)) == 0 && strcmp(out, "{\"keys\":[]}") == 0,
         "empty key set");
   /* overflow -> -1, no write past cap */
   char tiny[8];
   CHECK(kb_oidc_jwks_assemble(one, 1, tiny, sizeof(tiny)) == -1, "overflow rejected");
   if (fails == 0)
      printf("test_kb_oidc_jwks: all passed\n");
   return fails ? 1 : 0;
}
