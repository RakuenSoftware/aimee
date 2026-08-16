/* Parity tests for descriptor-owned DB2 relationship helpers. */
#include "aimee.h"
#include "rel_types.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

void db2_support_rel_type_normalize(const char *in, char *out, size_t out_len);
int db2_support_rel_type_is_functional(const char *rel_type);
/* The Make rules rename only the support TU; legacy names below resolve through rel_types.c. */

static void assert_normalize_parity(const char *input, size_t out_len)
{
   unsigned char legacy[80];
   unsigned char support[80];
   memset(legacy, 0xa5, sizeof(legacy));
   memset(support, 0xa5, sizeof(support));

   rel_type_normalize(input, (char *)legacy, out_len);
   db2_support_rel_type_normalize(input, (char *)support, out_len);
   assert(memcmp(legacy, support, sizeof(legacy)) == 0);
   for (size_t i = out_len; i < sizeof(legacy); i++)
      assert(legacy[i] == 0xa5);
}

static void test_normalize_corpus(void)
{
   static const char *const corpus[] = {
       NULL,
       "",
       "worksFor",
       "Works For",
       "works-for",
       "__already__snake__",
       "deviceHasIP",
       "123ABC",
       " punctuation! everywhere? ",
       "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-extra",
   };
   for (size_t i = 0; i < sizeof(corpus) / sizeof(corpus[0]); i++)
      for (size_t out_len = 0; out_len <= 79; out_len++)
         assert_normalize_parity(corpus[i], out_len);

   char untouched = 'x';
   rel_type_normalize("worksFor", NULL, 8);
   db2_support_rel_type_normalize("worksFor", NULL, 8);
   rel_type_normalize("worksFor", &untouched, 0);
   assert(untouched == 'x');
   db2_support_rel_type_normalize("worksFor", &untouched, 0);
   assert(untouched == 'x');
}

static void test_normalize_all_bytes(void)
{
   char input[3] = "";
   for (unsigned int first = 1; first <= UINT8_MAX; first++)
   {
      input[0] = (char)first;
      input[1] = '\0';
      assert_normalize_parity(input, 80);
      for (unsigned int second = 1; second <= UINT8_MAX; second++)
      {
         input[1] = (char)second;
         input[2] = '\0';
         assert_normalize_parity(input, 80);
      }
   }
}

static void test_functional_parity(void)
{
   static const char *const corpus[] = {
       "lives_in", "born_in",       "age",   "located_in", "has_hostname", "spouse",   "works_for",
       "has_role", "device_has_ip", "knows", "member_of",  "parent_of",    "worksFor", "",
       NULL,
   };
   for (size_t i = 0; i < sizeof(corpus) / sizeof(corpus[0]); i++)
      assert(rel_type_is_functional(corpus[i]) == db2_support_rel_type_is_functional(corpus[i]));

   for (size_t i = 0; i < 9; i++)
      assert(db2_support_rel_type_is_functional(corpus[i]) == 1);
   for (size_t i = 9; i < sizeof(corpus) / sizeof(corpus[0]); i++)
      assert(db2_support_rel_type_is_functional(corpus[i]) == 0);
}

int main(void)
{
   test_normalize_corpus();
   test_normalize_all_bytes();
   test_functional_parity();
   return 0;
}
