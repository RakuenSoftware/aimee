/* test_json_fluent.c: unit tests for the json_fluent helpers */
#include "json_fluent.h"
#include "json_wire.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                                                \
   do                                                                                              \
   {                                                                                               \
      if (!(cond))                                                                                 \
      {                                                                                            \
         fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                           \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

static int test_optional_getters(void)
{
   cJSON *o = cJSON_Parse("{\"s\":\"hi\",\"n\":42,\"b\":true,\"d\":3.5}");
   CHECK(o);
   CHECK(strcmp(jo_str(o, "s", "def"), "hi") == 0);
   CHECK(strcmp(jo_str(o, "missing", "def"), "def") == 0);
   CHECK(jo_str(o, "missing", NULL) == NULL);
   CHECK(jo_int(o, "n", 0) == 42);
   CHECK(jo_int(o, "missing", -1) == -1);
   CHECK(jo_i64(o, "n", 0) == 42);
   CHECK(jo_num(o, "d", 0) == 3.5);
   CHECK(jo_bool(o, "b", 0) == 1);
   CHECK(jo_bool(o, "missing", 1) == 1);
   /* Wrong type → default */
   CHECK(jo_int(o, "s", 7) == 7);
   CHECK(strcmp(jo_str(o, "n", "def"), "def") == 0);
   /* NULL obj tolerated */
   CHECK(strcmp(jo_str(NULL, "x", "ok"), "ok") == 0);
   CHECK(jo_int(NULL, "x", 5) == 5);
   cJSON_Delete(o);
   return 0;
}

static int test_required_getters(void)
{
   cJSON *o = cJSON_Parse("{\"name\":\"alice\",\"age\":30}");
   CHECK(o);
   const char *s = NULL;
   CHECK(jo_need_str(o, "name", &s) == 0);
   CHECK(strcmp(s, "alice") == 0);
   CHECK(jo_need_str(o, "missing", &s) == -1);
   CHECK(jo_need_str(o, "age", &s) == -1); /* wrong type */
   double d = 0;
   CHECK(jo_need_num(o, "age", &d) == 0);
   CHECK(d == 30.0);
   CHECK(jo_need_num(o, "name", &d) == -1);
   cJSON_Delete(o);
   return 0;
}

static int test_null_safe_adds(void)
{
   cJSON *o = cJSON_CreateObject();
   jo_add_str(o, "k", NULL);
   jo_add_str(o, "j", "v");
   jo_add_i64(o, "i", 123);
   jo_add_num(o, "d", 1.5);
   jo_add_bool(o, "b", 1);
   CHECK(strcmp(jo_str(o, "k", "x"), "") == 0);
   CHECK(strcmp(jo_str(o, "j", "x"), "v") == 0);
   CHECK(jo_i64(o, "i", 0) == 123);
   CHECK(jo_num(o, "d", 0) == 1.5);
   CHECK(jo_bool(o, "b", 0) == 1);
   cJSON_Delete(o);
   return 0;
}

static int test_response_builders(void)
{
   cJSON *ok = jo_ok();
   CHECK(strcmp(jo_str(ok, "status", ""), "ok") == 0);
   cJSON_Delete(ok);

   cJSON *okv = jo_ok_kv("key", "val");
   CHECK(strcmp(jo_str(okv, "status", ""), "ok") == 0);
   CHECK(strcmp(jo_str(okv, "key", ""), "val") == 0);
   cJSON_Delete(okv);

   cJSON *err = jo_err("bad");
   CHECK(strcmp(jo_str(err, "status", ""), "error") == 0);
   CHECK(strcmp(jo_str(err, "message", ""), "bad") == 0);
   cJSON_Delete(err);

   /* NULL message is safe */
   cJSON *err2 = jo_err(NULL);
   CHECK(strcmp(jo_str(err2, "message", "x"), "") == 0);
   cJSON_Delete(err2);
   return 0;
}

static int test_numeric_arrays(void)
{
   float out[4] = {9, 9, 9, 9};
   const char *invalid[] = {"null", "[1,2,3,4,5]", "[1,null]", "[1,1e39]", "[1,1e999]"};
   for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
   {
      cJSON *array = cJSON_Parse(invalid[i]);
      CHECK(jo_float_array(array, out, 4) == 0);
      CHECK(out[0] == 9 && out[1] == 9 && out[2] == 9 && out[3] == 9);
      cJSON_Delete(array);
   }
   cJSON *matrix = cJSON_Parse("[[1,2],[3,1e39]]");
   CHECK(jo_float_matrix(matrix, out, 2, 2) == 0 && out[0] == 9 && out[2] == 9);
   cJSON_Delete(matrix);
   matrix = cJSON_Parse("[[1,2],[3,4]]");
   CHECK(jo_float_matrix(matrix, out, 1, 2) == 0 && out[0] == 9);
   CHECK(jo_float_matrix(matrix, out, 2, 3) == 0 && out[0] == 9);
   CHECK(jo_float_matrix(matrix, out, 2, 2) == 2 && out[0] == 1 && out[3] == 4);
   CHECK(jo_float_array(cJSON_GetArrayItem(matrix, 0), out, 4) == 2 && out[1] == 2);
   cJSON_Delete(matrix);
   return 0;
}

static int test_exact_integer_wire(void)
{
   const char *wire = "{\"text\":\"123 \\\" -9007199254740995\",\"9007199254740997\":"
                      "[42,9007199254740993,-9223372036854775808,1.25,1e20],"
                      "\"nested\":{\"id\":9223372036854775807}}";
   cJSON *doc = json_wire_parse_exact_integers(wire);
   CHECK(doc);
   char *rendered = cJSON_PrintUnformatted(doc);
   CHECK(rendered && strstr(rendered, "[42,9007199254740993,-9223372036854775808,1.25,"));
   CHECK(strstr(rendered, "\"id\":9223372036854775807"));
   cJSON_free(rendered);
   cJSON_Delete(doc);
   doc = json_wire_parse_exact_integers("9007199254740993");
   CHECK(cJSON_IsRaw(doc) && !strcmp(doc->valuestring, "9007199254740993"));
   cJSON_Delete(doc);
   CHECK(!json_wire_parse_exact_integers("{\"id\":1} trailing"));
   CHECK(!json_wire_parse_exact_integers("{\"id\":9223372036854775807"));
   return 0;
}

static int test_exact_i64_values(void)
{
   const int64_t values[] = {0,
                             42,
                             -42,
                             INT64_C(9007199254740991),
                             INT64_C(9007199254740992),
                             INT64_C(9007199254740993),
                             -INT64_C(9007199254740993),
                             INT64_MIN,
                             INT64_MAX};
   for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++)
   {
      cJSON *value = jo_i64_value_exact(values[i]);
      CHECK(value);
      CHECK(cJSON_IsNumber(value) ==
            (values[i] >= -INT64_C(9007199254740991) && values[i] <= INT64_C(9007199254740991)));
      char *wire = cJSON_PrintUnformatted(value);
      cJSON *parsed = cJSON_Parse(wire);
      int64_t decoded = 17;
      CHECK(jo_read_i64_exact(parsed, &decoded) && decoded == values[i]);
      cJSON_Delete(parsed);
      cJSON_free(wire);
      cJSON_Delete(value);
   }
   const char *invalid[] = {"null",
                            "true",
                            "[]",
                            "{}",
                            "1.5",
                            "1e999",
                            "9007199254740992",
                            "-9007199254740992",
                            "9223372036854775807",
                            "\"\"",
                            "\"+1\"",
                            "\"01\"",
                            "\"-0\"",
                            "\"1x\"",
                            "\" 1\"",
                            "\"1 \"",
                            "\"1.0\"",
                            "\"1e2\"",
                            "\"9223372036854775808\"",
                            "\"-9223372036854775809\""};
   for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
   {
      cJSON *value = cJSON_Parse(invalid[i]);
      int64_t decoded = 17;
      CHECK(!jo_read_i64_exact(value, &decoded) && decoded == 17);
      cJSON_Delete(value);
   }
   CHECK(json_wire_has_nul_escape("[\"9007199254740993\\u0000suffix\"]",
                                  strlen("[\"9007199254740993\\u0000suffix\"]")));
   CHECK(!json_wire_has_nul_escape("[\"literal\\\\u0000\"]", strlen("[\"literal\\\\u0000\"]")));
   return 0;
}

int main(void)
{
   int failed = test_exact_integer_wire();
   failed += test_exact_i64_values();
   failed += test_numeric_arrays();
   failed += test_optional_getters();
   failed += test_required_getters();
   failed += test_null_safe_adds();
   failed += test_response_builders();
   if (failed == 0)
      printf("test_json_fluent: ok\n");
   else
      fprintf(stderr, "test_json_fluent: %d failure(s)\n", failed);
   return failed ? 1 : 0;
}
