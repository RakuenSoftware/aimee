#include "kb_mgmt_token.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_MAX 65536u

static int fake_sign(void *ctx, const unsigned char *input, size_t input_n,
                     unsigned char *signature, size_t cap, size_t *signature_n)
{
   if (ctx)
      (*(unsigned *)ctx)++;
   if (!input || !input_n || cap < 256)
      return 0;
   for (size_t i = 0; i < 256; ++i)
      signature[i] = (unsigned char)(input[i % input_n] ^ (unsigned char)i);
   *signature_n = 256;
   return 1;
}

static kb_mgmt_token_claims_t valid_claims(void)
{
   kb_mgmt_token_claims_t c;
   memset(&c, 0, sizeof(c));
   snprintf(c.issuer, sizeof(c.issuer), "issuer");
   snprintf(c.audience, sizeof(c.audience), "server-01");
   snprintf(c.subject, sizeof(c.subject), "owner");
   c.team_id = 1;
   c.capability = KB_MGMT_TOKEN_CAP_REMOTE_WRITES;
   snprintf(c.jti, sizeof(c.jti), "0123456789abcdef");
   snprintf(c.correlation_id, sizeof(c.correlation_id), "request-1");
   snprintf(c.request_sha256, sizeof(c.request_sha256),
            "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
   snprintf(c.peer_issuer, sizeof(c.peer_issuer), "CN=management");
   snprintf(c.peer_serial, sizeof(c.peer_serial), "01af");
   snprintf(c.peer_fingerprint, sizeof(c.peer_fingerprint),
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
   snprintf(c.kid, sizeof(c.kid), "management-1");
   c.issued_at = INT64_C(1900000000);
   c.expires_at = INT64_C(1900000090);
   return c;
}

static void fuzz_one(const unsigned char *data, size_t size)
{
   if ((!data && size) || size > FUZZ_MAX)
      return;
   char jwt[KB_MGMT_TOKEN_WIRE_MAX + 1];
   size_t jwt_n = 0;
   kb_mgmt_token_claims_t seed = valid_claims();
   unsigned calls = 0;
   if (kb_mgmt_token_build(&seed, fake_sign, &calls, jwt, sizeof(jwt), &jwt_n) !=
           KB_MGMT_TOKEN_OK ||
       calls != 1 || !jwt_n || jwt_n > KB_MGMT_TOKEN_WIRE_MAX || jwt[jwt_n] != '\0')
      abort();

   kb_mgmt_token_claims_t mutated = seed;
   unsigned char *raw = (unsigned char *)&mutated;
   for (size_t i = 0; i + 1 < size && i < 1024; i += 2)
      raw[(size_t)data[i] % sizeof(mutated)] ^= data[i + 1];
   size_t cap = size ? (size_t)data[0] * 33u : sizeof(jwt);
   if (cap > sizeof(jwt))
      cap = sizeof(jwt);
   jwt_n = size;
   (void)kb_mgmt_token_build(&mutated, fake_sign, NULL, jwt, cap, &jwt_n);
}

#ifndef FUZZ_STANDALONE
int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
   fuzz_one(data, size);
   return 0;
}
#else
static int fuzz_file(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return 1;
   unsigned char input[FUZZ_MAX];
   size_t n = fread(input, 1, sizeof(input), f);
   int too_large = n == sizeof(input) && fgetc(f) != EOF;
   int failed = ferror(f);
   fclose(f);
   if (!failed && !too_large)
      fuzz_one(input, n);
   return failed ? 1 : 0;
}

int main(int argc, char **argv)
{
   if (argc < 2)
   {
      unsigned char input[FUZZ_MAX];
      fuzz_one(input, fread(input, 1, sizeof(input), stdin));
   }
   else
      for (int i = 1; i < argc; ++i)
         if (fuzz_file(argv[i]) != 0)
            return 1;
   printf("fuzz_kb_mgmt_token: %d inputs ok\n", argc > 1 ? argc - 1 : 1);
   return 0;
}
#endif
