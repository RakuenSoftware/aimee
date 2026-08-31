/* Parity tests for descriptor-owned DB2 model-catalog validation. */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int kb_models_wire_valid(const char *wire);
int kb_models_name_clean(const char *value, int max);
int kb_models_endpoint_valid(const char *endpoint, int max);
int db2_support_models_wire_valid(const char *wire);
int db2_support_models_name_clean(const char *value, int max);
int db2_support_models_endpoint_valid(const char *endpoint, int max);

static void assert_wire_parity(const char *wire)
{
   assert(kb_models_wire_valid(wire) == db2_support_models_wire_valid(wire));
}

static void assert_name_parity(const char *value, int max)
{
   assert(kb_models_name_clean(value, max) == db2_support_models_name_clean(value, max));
}

static void assert_endpoint_parity(const char *value, int max)
{
   assert(kb_models_endpoint_valid(value, max) == db2_support_models_endpoint_valid(value, max));
}

static void test_wire_parity(void)
{
   const char *const cases[] = {NULL,     "",       "anthropic", "openai",  "responses",
                                "gemini", "OpenAI", "bedrock",   "openai ", "openai\n"};
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
      assert_wire_parity(cases[i]);
}

static void test_name_byte_and_bound_parity(void)
{
   assert_name_parity(NULL, 200);
   assert_name_parity("", 200);
   assert_name_parity("x", -1);
   assert_name_parity("x", 0);
   assert_name_parity("x", 1);

   char byte_value[] = {'a', 'x', 'b', '\0'};
   for (int byte = 1; byte <= 255; byte++)
   {
      byte_value[1] = (char)byte;
      assert_name_parity(byte_value, 3);
   }

   char value[514];
   for (size_t length = 0; length <= 512; length++)
   {
      memset(value, 'm', length);
      value[length] = '\0';
      assert_name_parity(value, 100);
      assert_name_parity(value, (int)length);
      if (length > 0)
         assert_name_parity(value, (int)length - 1);
   }
}

static void test_endpoint_parity(void)
{
   const char *const cases[] = {NULL,
                                "",
                                "http://",
                                "https://",
                                "http://localhost",
                                "https://example.test/v1",
                                "HTTP://example.test",
                                "ftp://example.test",
                                "example.test",
                                "httpsx://example.test",
                                "https://example.test\nnext"};
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      assert_endpoint_parity(cases[i], 500);
      assert_endpoint_parity(cases[i], 0);
      assert_endpoint_parity(cases[i], -1);
   }

   char byte_value[] = "https://a";
   for (int byte = 1; byte <= 255; byte++)
   {
      byte_value[8] = (char)byte;
      assert_endpoint_parity(byte_value, 9);
   }

   char endpoint[520] = "https://";
   for (size_t length = 8; length <= 512; length++)
   {
      memset(endpoint + 8, 'e', length - 8);
      endpoint[length] = '\0';
      assert_endpoint_parity(endpoint, 500);
      assert_endpoint_parity(endpoint, (int)length);
      assert_endpoint_parity(endpoint, (int)length - 1);
   }
}

int main(void)
{
   test_wire_parity();
   test_name_byte_and_bound_parity();
   test_endpoint_parity();
   return 0;
}
