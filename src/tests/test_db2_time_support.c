/* Parity tests for descriptor-owned DB2 UTC timestamp support. */
#include "aimee.h"

#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void db2_support_now_utc(char *buf, size_t len);
time_t db2_support_parse_utc_ts(const char *s);

_Static_assert(sizeof(time_t) == sizeof(db2_support_parse_utc_ts("1970-01-01")),
               "support parser must preserve the time_t ABI");

static void assert_parse_parity(const char *input)
{
   assert(parse_utc_ts(input) == db2_support_parse_utc_ts(input));
}

static void test_parse_corpus(void)
{
   static const char *const corpus[] = {
       "1970-01-01T00:00:00Z",
       "1970-01-02T00:00:00Z",
       "2000-02-29T12:34:56Z",
       "2038-01-19T03:14:07Z",
       "2026-08-16 09:30:00",
       "2026-08-16",
       "2024-02-31T00:00:00Z",
       "2024-01-01T24:00:00Z",
       "2024-01-01T00:60:00Z",
       "2024-01-01T00:00:60Z",
       "2024-01-01T00:00:00Ztrailing",
       "1969-12-31T23:59:59Z",
       "2024-00-01T00:00:00Z",
       "2024-13-01T00:00:00Z",
       "2024-01-00T00:00:00Z",
       "2024-01-32T00:00:00Z",
       "2024/01/01T00:00:00Z",
       "2024-01-01X00:00:00Z",
       "2024-01-01T00:00",
       "2024-01-01Z",
       "not-a-time",
       "",
       NULL,
   };
   for (size_t i = 0; i < sizeof(corpus) / sizeof(corpus[0]); i++)
      assert_parse_parity(corpus[i]);

   assert(parse_utc_ts("1970-01-02T00:00:00Z") == 86400);
   assert(db2_support_parse_utc_ts("1970-01-02T00:00:00Z") == 86400);
}

static void test_timezone_independence(void)
{
   static const char *const zones[] = {"UTC0", "PST8PDT", "JST-9"};
   const char *saved = getenv("TZ");
   char *saved_copy = saved ? strdup(saved) : NULL;

   for (size_t i = 0; i < sizeof(zones) / sizeof(zones[0]); i++)
   {
      assert(setenv("TZ", zones[i], 1) == 0);
      tzset();
      assert(parse_utc_ts("1970-01-02 00:00:00") == 86400);
      assert(db2_support_parse_utc_ts("1970-01-02 00:00:00") == 86400);
      assert_parse_parity("2024-02-31T12:34:56Z");
   }

   if (saved_copy)
   {
      assert(setenv("TZ", saved_copy, 1) == 0);
      free(saved_copy);
   }
   else
      assert(unsetenv("TZ") == 0);
   tzset();
}

static void assert_utc_shape(const char text[21])
{
   assert(strlen(text) == 20);
   assert(text[4] == '-' && text[7] == '-' && text[10] == 'T');
   assert(text[13] == ':' && text[16] == ':' && text[19] == 'Z');
   for (size_t i = 0; i < 20; i++)
      if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 && i != 19)
         assert(isdigit((unsigned char)text[i]));
}

static void test_now_utc(void)
{
   char legacy[21] = "";
   char support[21] = "";
   time_t before = time(NULL);
   now_utc(legacy, sizeof(legacy));
   db2_support_now_utc(support, sizeof(support));
   time_t after = time(NULL);

   assert_utc_shape(legacy);
   assert_utc_shape(support);
   time_t legacy_epoch = parse_utc_ts(legacy);
   time_t support_epoch = db2_support_parse_utc_ts(support);
   assert(legacy_epoch >= before - 5 && legacy_epoch <= after + 5);
   assert(support_epoch >= before - 5 && support_epoch <= after + 5);
   assert(labs((long)(legacy_epoch - support_epoch)) <= 5);
}

int main(void)
{
   test_parse_corpus();
   test_timezone_independence();
   test_now_utc();
   return 0;
}
