#include "db2/management_client_instance.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int all_zero(const void *value, size_t length)
{
   const unsigned char *p = value;
   unsigned char any = 0;
   for (size_t i = 0; i < length; ++i)
      any |= p[i];
   return any == 0;
}

static void test_sqlstate(void)
{
   assert(db2_management_client_instance_classify_sqlstate("22023") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID);
   assert(db2_management_client_instance_classify_sqlstate("28000") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_DENIED);
   assert(db2_management_client_instance_classify_sqlstate("42501") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_DENIED);
   assert(db2_management_client_instance_classify_sqlstate("23505") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_CONFLICT);
   assert(db2_management_client_instance_classify_sqlstate("40001") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_RETRY);
   assert(db2_management_client_instance_classify_sqlstate("40P01") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_RETRY);
   assert(db2_management_client_instance_classify_sqlstate("55000") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_INTEGRITY);
   assert(db2_management_client_instance_classify_sqlstate("08006") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE);
   assert(db2_management_client_instance_classify_sqlstate("25006") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE);
   assert(db2_management_client_instance_classify_sqlstate("XX000") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE);
   assert(db2_management_client_instance_classify_sqlstate("") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE);
   assert(db2_management_client_instance_classify_sqlstate(NULL) ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE);
}

static void test_digest_vector(void)
{
   static const unsigned char expected[32] = {0x95, 0x63, 0x7a, 0xcc, 0x7f, 0x1b, 0x99, 0xb7,
                                              0xa5, 0x93, 0x27, 0x43, 0xdc, 0x7d, 0x56, 0x45,
                                              0x52, 0x35, 0x48, 0x62, 0x2a, 0xfb, 0x1e, 0x67,
                                              0x9a, 0x62, 0x13, 0x78, 0xc1, 0xe1, 0x6e, 0xe4};
   unsigned char proof[32], custody[32], digest[32];
   for (size_t i = 0; i < sizeof(proof); ++i)
   {
      proof[i] = (unsigned char)i;
      custody[i] = (unsigned char)(i + 32);
   }
   assert(db2_management_client_instance_binding_digest(
              "spiffe://example.test", "spiffe://example.test/kb/node-1", proof, custody, digest) ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_OK);
   assert(memcmp(digest, expected, sizeof(expected)) == 0);

   proof[0] ^= 1;
   assert(db2_management_client_instance_binding_digest(
              "spiffe://example.test", "spiffe://example.test/kb/node-1", proof, custody, digest) ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_OK);
   assert(memcmp(digest, expected, sizeof(expected)) != 0);
}

static void test_bounds_and_clearing(void)
{
   unsigned char anchor[32] = {1}, digest[32];
   memset(digest, 0xa5, sizeof(digest));
   assert(db2_management_client_instance_binding_digest("", "subject", anchor, anchor, digest) ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID);
   assert(all_zero(digest, sizeof(digest)));

   char too_long[DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 2];
   memset(too_long, 'a', sizeof(too_long));
   too_long[sizeof(too_long) - 1] = 0;
   memset(digest, 0xa5, sizeof(digest));
   assert(
       db2_management_client_instance_binding_digest(too_long, "subject", anchor, anchor, digest) ==
       DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID);
   assert(all_zero(digest, sizeof(digest)));

   memset(digest, 0xa5, sizeof(digest));
   assert(db2_management_client_instance_binding_digest("issuer", "bad subject", anchor, anchor,
                                                        digest) ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID);
   assert(all_zero(digest, sizeof(digest)));
   assert(db2_management_client_instance_binding_digest(
              "issuer", "subject", anchor, anchor, NULL) == DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID);

   db2_management_client_instance_binding_t binding;
   memset(&binding, 0xa5, sizeof(binding));
   assert(
       db2_management_client_instance_binding_init("issuer", too_long, anchor, anchor, &binding) ==
       DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID);
   assert(all_zero(&binding, sizeof(binding)));
   assert(db2_management_client_instance_binding_init(
              "issuer", "subject", anchor, anchor, &binding) == DB2_MANAGEMENT_CLIENT_INSTANCE_OK);
   assert(strcmp(binding.issuer, "issuer") == 0);
   assert(strcmp(binding.subject, "subject") == 0);
   assert(memcmp(binding.proof_anchor, anchor, sizeof(anchor)) == 0);
   assert(!all_zero(binding.binding_digest, sizeof(binding.binding_digest)));
}

int main(void)
{
   test_sqlstate();
   test_digest_vector();
   test_bounds_and_clearing();
   puts("management_client_instance: all tests passed");
   return 0;
}
