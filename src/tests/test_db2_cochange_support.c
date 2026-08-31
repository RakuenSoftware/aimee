/* Parity tests for descriptor-owned DB2 co-change support. */
#include "aimee.h"
#include "index.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
   char a[128];
   char b[128];
} db2_cochange_pair_t;

int db2_support_cochange_pairs_for_commit(char names[][128], int n, int max_files,
                                          db2_cochange_pair_t *out, int out_cap);
int db2_support_cochange_is_hex_sha(const char *s);

_Static_assert(sizeof(cochange_pair_t) == sizeof(db2_cochange_pair_t), "co-change pair size drift");
_Static_assert(offsetof(cochange_pair_t, a) == offsetof(db2_cochange_pair_t, a),
               "co-change pair a offset drift");
_Static_assert(offsetof(cochange_pair_t, b) == offsetof(db2_cochange_pair_t, b),
               "co-change pair b offset drift");

static int load_names(char out[][128], const char *const *names)
{
   int count = 0;
   memset(out, 0, sizeof(char[16][128]));
   while (names[count])
   {
      snprintf(out[count], 128, "%s", names[count]);
      count++;
   }
   return count;
}

static void assert_pair_parity(const char *const *names, int max_files, int out_cap)
{
   char legacy_names[16][128];
   char support_names[16][128];
   cochange_pair_t legacy[64];
   db2_cochange_pair_t support[64];
   int count = load_names(legacy_names, names);
   assert(load_names(support_names, names) == count);
   memset(legacy, 0xa5, sizeof(legacy));
   memset(support, 0xa5, sizeof(support));

   int legacy_count = cochange_pairs_for_commit(legacy_names, count, max_files, legacy, out_cap);
   int support_count =
       db2_support_cochange_pairs_for_commit(support_names, count, max_files, support, out_cap);
   assert(legacy_count == support_count);
   assert(memcmp(legacy_names, support_names, sizeof(legacy_names)) == 0);
   assert(memcmp(legacy, support, sizeof(legacy)) == 0);
}

static void test_pairing_parity(void)
{
   const char *const empty[] = {NULL};
   const char *const lone[] = {"solo.c", NULL};
   const char *const unordered[] = {"c.c", "a.c", "b.c", NULL};
   const char *const duplicate[] = {"z.c", "a.c", "z.c", "b.c", "a.c", NULL};
   const char *const long_names[] = {
       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.c",
       "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.c", NULL};

   assert_pair_parity(empty, 25, 64);
   assert_pair_parity(lone, 25, 64);
   assert_pair_parity(unordered, 25, 64);
   assert_pair_parity(unordered, 2, 64);
   assert_pair_parity(unordered, 25, 0);
   assert_pair_parity(unordered, 25, 2);
   assert_pair_parity(duplicate, 4, 64);
   assert_pair_parity(long_names, 25, 64);

   char legacy_names[1][128] = {{0}};
   char support_names[1][128] = {{0}};
   assert(cochange_pairs_for_commit(legacy_names, -1, 25, NULL, 0) == 0);
   assert(db2_support_cochange_pairs_for_commit(support_names, -1, 25, NULL, 0) == 0);
}

static void test_sha_parity(void)
{
   char max_hex[65];
   char too_long[66];
   memset(max_hex, 'a', sizeof(max_hex) - 1);
   max_hex[sizeof(max_hex) - 1] = '\0';
   memset(too_long, 'f', sizeof(too_long) - 1);
   too_long[sizeof(too_long) - 1] = '\0';

   const char *const cases[] = {
       NULL,       "",        "abc",      "abcd",  "0123456789abcdef",
       "DEADBEEF", "abc-123", "$(false)", max_hex, too_long,
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
      assert(cochange_is_hex_sha(cases[i]) == db2_support_cochange_is_hex_sha(cases[i]));
}

int main(void)
{
   test_pairing_parity();
   test_sha_parity();
   return 0;
}
