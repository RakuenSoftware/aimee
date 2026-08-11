/* economizer_json is the last of this file's three units still written in C.
 * The provenance capability moved to the Go economizer module (provenance.go, with
 * its own tests) and the dispatch lease was deleted outright -- it had no caller
 * anywhere in the tree, only this test. What remains is the JSON canonicaliser. */
#include "economizer_json.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void test_json_compaction_preserves_non_whitespace_bytes(void)
{
   static const char source[] =
       " \n { \"a\" : [ 1.2300e+02 , \" x \\u0061 \" ], \"z\" : true } \r\n";
   static const char expected[] = "{\"a\":[1.2300e+02,\" x \\u0061 \"],\"z\":true}";
   uint8_t *out = NULL;
   size_t n = 0;
   assert(econ_json_compact(source, strlen(source), &out, &n) == ECON_JSON_OK);
   assert(n == strlen(expected));
   assert(memcmp(out, expected, n) == 0);
   free(out);

   assert(econ_json_compact(expected, strlen(expected), &out, &n) == ECON_JSON_NOT_SHORTER);
}

static void test_json_rejects_ambiguity_and_invalid_syntax(void)
{
   uint8_t *out = NULL;
   size_t n = 0;
   const char duplicate[] = "{ \"a\": 1, \"\\u0061\": 2 }";
   assert(econ_json_compact(duplicate, strlen(duplicate), &out, &n) == ECON_JSON_DUPLICATE_KEY);
   const char invalid_number[] = "[ 01 ]";
   assert(econ_json_compact(invalid_number, strlen(invalid_number), &out, &n) ==
          ECON_JSON_INVALID_SYNTAX);
   const unsigned char invalid_utf8[] = {'[', ' ', '"', 0xc0, 0x80, '"', ' ', ']'};
   assert(econ_json_compact(invalid_utf8, sizeof(invalid_utf8), &out, &n) ==
          ECON_JSON_INVALID_UTF8);
   const char lone_surrogate[] = "[ \"\\ud800\" ]";
   assert(econ_json_compact(lone_surrogate, strlen(lone_surrogate), &out, &n) ==
          ECON_JSON_INVALID_SYNTAX);
}

static void test_json_depth_bound(void)
{
   char deep[(ECON_JSON_MAX_DEPTH + 3) * 2 + 1];
   size_t p = 0;
   for (unsigned i = 0; i < ECON_JSON_MAX_DEPTH + 2; i++)
      deep[p++] = '[';
   deep[p++] = '0';
   for (unsigned i = 0; i < ECON_JSON_MAX_DEPTH + 2; i++)
      deep[p++] = ']';
   deep[p] = 0;
   uint8_t *out = NULL;
   size_t n = 0;
   assert(econ_json_compact(deep, p, &out, &n) == ECON_JSON_TOO_DEEP);
}

static void test_json_deterministic_whitespace_property(void)
{
   static const char *tokens[] = {
       "{", "\"a\"", ":", "[", "-0.25e-2", ",", "true",
       ",", "null",  "]", ",", "\"b\"",    ":", "\"space stays: \\t \\u0061\"",
       "}"};
   static const char whitespace[] = " \t\r\n";
   char expected[256] = {0};
   for (size_t i = 0; i < sizeof(tokens) / sizeof(tokens[0]); i++)
      strcat(expected, tokens[i]);
   unsigned state = 0x9e3779b9u;
   for (unsigned iteration = 0; iteration < 1000; iteration++)
   {
      char source[1024];
      size_t used = 0;
      for (size_t i = 0; i < sizeof(tokens) / sizeof(tokens[0]); i++)
      {
         state = state * 1664525u + 1013904223u;
         unsigned before = state & 3u;
         while (before--)
            source[used++] = whitespace[(state >> (before * 2)) & 3u];
         size_t token_len = strlen(tokens[i]);
         memcpy(source + used, tokens[i], token_len);
         used += token_len;
         state = state * 1664525u + 1013904223u;
         unsigned after = state & 3u;
         while (after--)
            source[used++] = whitespace[(state >> (after * 2)) & 3u];
      }
      uint8_t *out = NULL;
      size_t n = 0;
      assert(econ_json_compact(source, used, &out, &n) == ECON_JSON_OK);
      assert(n == strlen(expected));
      assert(memcmp(out, expected, n) == 0);
      free(out);
   }
}

int main(void)
{
   test_json_compaction_preserves_non_whitespace_bytes();
   test_json_rejects_ambiguity_and_invalid_syntax();
   test_json_depth_bound();
   test_json_deterministic_whitespace_property();
   puts("economizer_activation: ALL PASS");
   return 0;
}
