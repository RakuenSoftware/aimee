/* test_kb_http_json.c: the KB HTTP scalar field scanners.
 *
 * These read untrusted request bodies, so the cases that matter are the
 * malformed ones: what a client can send that the scanner mis-reads. The
 * negative-integer case below was a live defect -- json_int tested the first
 * character against '0'..'9', so a leading '-' failed and every negative
 * silently became the caller's default instead of the value sent. */

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "kb/http/kb_http_json.h"

static void test_int_basic(void)
{
   assert(kb_http_json_int("{\"max_results\":5}", "max_results", 10) == 5);
   assert(kb_http_json_int("{\"max_results\": 5}", "max_results", 10) == 5);
   assert(kb_http_json_int("{\"max_results\"\t:\t5}", "max_results", 10) == 5);
   assert(kb_http_json_int("{\"max_results\":0}", "max_results", 10) == 0);

   /* Absent, null body, and non-numeric all fall back to the default. */
   assert(kb_http_json_int("{\"other\":5}", "max_results", 10) == 10);
   assert(kb_http_json_int(NULL, "max_results", 10) == 10);
   assert(kb_http_json_int("{\"max_results\":\"abc\"}", "max_results", 10) == 10);
   assert(kb_http_json_int("{\"max_results\"}", "max_results", 10) == 10);
   printf("  PASS: int basic\n");
}

static void test_int_negative(void)
{
   /* The regression. Before the fix these returned the default (10), so a
    * caller could not distinguish "client sent -1" from "client sent nothing"
    * -- invalid input was silently swallowed rather than reported. */
   assert(kb_http_json_int("{\"max_results\":-1}", "max_results", 10) == -1);
   assert(kb_http_json_int("{\"max_results\": -42}", "max_results", 10) == -42);
   assert(kb_http_json_int("{\"limit\":-1}", "limit", 20) == -1);

   /* An explicit sign is honoured in both directions. */
   assert(kb_http_json_int("{\"max_results\":+7}", "max_results", 10) == 7);

   /* A lone sign is not a number, so it is still the default. */
   assert(kb_http_json_int("{\"max_results\":-}", "max_results", 10) == 10);
   assert(kb_http_json_int("{\"max_results\":-abc}", "max_results", 10) == 10);
   printf("  PASS: int negative\n");
}

static void test_int_range(void)
{
   /* atoi() on an out-of-range literal is undefined behaviour. Saturate
    * instead, so a hostile body cannot produce an implementation-defined
    * value that then flows into a caller's clamp. */
   assert(kb_http_json_int("{\"n\":99999999999999999999}", "n", 10) == INT_MAX);
   assert(kb_http_json_int("{\"n\":-99999999999999999999}", "n", 10) == INT_MIN);
   assert(kb_http_json_int("{\"n\":2147483647}", "n", 10) == INT_MAX);
   assert(kb_http_json_int("{\"n\":-2147483648}", "n", 10) == INT_MIN);
   printf("  PASS: int range saturates\n");
}

static void test_key_inside_string_value_is_not_matched(void)
{
   /* A JSON string cannot contain an unescaped quote, so a caller cannot
    * smuggle a parameter through a string field: the needle's own quotes
    * prevent \"max_results\" from matching. This pins that property -- it is
    * the reason the substring scan is acceptable here at all. */
   const char *body = "{\"query\":\"give me \\\"max_results\\\":25 please\",\"max_results\":2}";
   assert(kb_http_json_int(body, "max_results", 10) == 2);
   printf("  PASS: key inside a string value does not match\n");
}

static void test_str(void)
{
   char out[64];
   assert(kb_http_json_str("{\"fusion_mode\":\"rrf\"}", "fusion_mode", out, sizeof(out)) == 1);
   assert(strcmp(out, "rrf") == 0);

   /* Escapes are unescaped one level. */
   assert(kb_http_json_str("{\"q\":\"a\\\"b\"}", "q", out, sizeof(out)) == 1);
   assert(strcmp(out, "a\"b") == 0);

   /* Absent / wrong type / empty capacity yield "" and 0. */
   assert(kb_http_json_str("{\"other\":\"x\"}", "q", out, sizeof(out)) == 0);
   assert(out[0] == '\0');
   assert(kb_http_json_str("{\"q\":5}", "q", out, sizeof(out)) == 0);

   /* Truncation must still NUL-terminate. */
   char small[4];
   assert(kb_http_json_str("{\"q\":\"abcdefgh\"}", "q", small, sizeof(small)) == 1);
   assert(strlen(small) == 3);
   printf("  PASS: str\n");
}

static void test_bool(void)
{
   assert(kb_http_json_bool("{\"all\":true}", "all", 0) == 1);
   assert(kb_http_json_bool("{\"all\":false}", "all", 1) == 0);
   assert(kb_http_json_bool("{\"all\": true}", "all", 0) == 1);
   assert(kb_http_json_bool("{\"other\":true}", "all", 0) == 0);
   assert(kb_http_json_bool("{\"all\":1}", "all", 0) == 0);
   assert(kb_http_json_bool(NULL, "all", 1) == 1);
   printf("  PASS: bool\n");
}

static void test_truncated_body_is_tolerated(void)
{
   /* Deliberate: a truncated body that still names its field is read, because
    * these scanners exist to be forgiving where cJSON would reject. Pinned so
    * that anyone who "fixes" it into strictness sees the contract change. */
   assert(kb_http_json_int("{\"max_results\":5,", "max_results", 10) == 5);
   char out[16];
   assert(kb_http_json_str("{\"fusion_mode\":\"rrf\"", "fusion_mode", out, sizeof(out)) == 1);
   assert(strcmp(out, "rrf") == 0);
   printf("  PASS: truncated body tolerated\n");
}

int main(void)
{
   printf("kb_http_json:\n");
   test_int_basic();
   test_int_negative();
   test_int_range();
   test_key_inside_string_value_is_not_matched();
   test_str();
   test_bool();
   test_truncated_body_is_tolerated();
   printf("kb_http_json: all tests passed\n");
   return 0;
}
