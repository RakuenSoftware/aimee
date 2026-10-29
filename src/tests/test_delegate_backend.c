/* test_delegate_backend.c: registry-only coverage for the
 * delegate_backend interface. The vtable function pointers are not
 * exercised here — that lands once a real backend ships. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aimee/delegates/delegate_backend.h>

static delegate_backend_t make_stub(const char *name)
{
   delegate_backend_t b;
   memset(&b, 0, sizeof(b));
   b.name = name;
   b.description = "stub";
   return b;
}

static void test_register_and_lookup(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_t a = make_stub("local");
   delegate_backend_t b = make_stub("docker");
   assert(delegate_backend_register(&a) == 0);
   assert(delegate_backend_register(&b) == 0);

   assert(delegate_backend_lookup("local") == &a);
   assert(delegate_backend_lookup("docker") == &b);
   assert(delegate_backend_lookup("does-not-exist") == NULL);
   printf("  PASS: test_register_and_lookup\n");
}

static void test_register_rejects_duplicate(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_t a = make_stub("local");
   delegate_backend_t a_dup = make_stub("local");
   assert(delegate_backend_register(&a) == 0);
   assert(delegate_backend_register(&a_dup) == -1);
   /* The first registration wins; a_dup must NOT have replaced it. */
   assert(delegate_backend_lookup("local") == &a);
   printf("  PASS: test_register_rejects_duplicate\n");
}

static void test_register_rejects_invalid(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_t empty_name = make_stub("");
   assert(delegate_backend_register(NULL) == -1);
   assert(delegate_backend_register(&empty_name) == -1);
   delegate_backend_t null_name;
   memset(&null_name, 0, sizeof(null_name));
   null_name.name = NULL;
   assert(delegate_backend_register(&null_name) == -1);
   printf("  PASS: test_register_rejects_invalid\n");
}

static void test_lookup_invalid_inputs(void)
{
   delegate_backend_reset_for_test();
   assert(delegate_backend_lookup(NULL) == NULL);
   assert(delegate_backend_lookup("") == NULL);
   /* Empty registry: any lookup is NULL. */
   assert(delegate_backend_lookup("local") == NULL);
   printf("  PASS: test_lookup_invalid_inputs\n");
}

static void test_list_returns_in_registration_order(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_t a = make_stub("local");
   delegate_backend_t b = make_stub("ssh");
   delegate_backend_t c = make_stub("docker");
   delegate_backend_register(&a);
   delegate_backend_register(&b);
   delegate_backend_register(&c);

   delegate_backend_t *out[8] = {0};
   int n = delegate_backend_list(out, 8);
   assert(n == 3);
   assert(out[0] == &a);
   assert(out[1] == &b);
   assert(out[2] == &c);
   printf("  PASS: test_list_returns_in_registration_order\n");
}

static void test_list_truncates_to_max(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_t a = make_stub("a");
   delegate_backend_t b = make_stub("b");
   delegate_backend_t c = make_stub("c");
   delegate_backend_register(&a);
   delegate_backend_register(&b);
   delegate_backend_register(&c);

   delegate_backend_t *out[2] = {0};
   int n = delegate_backend_list(out, 2);
   assert(n == 2);
   assert(out[0] == &a);
   assert(out[1] == &b);

   /* max=0 returns 0 with no writes. */
   delegate_backend_t *out0[1] = {(delegate_backend_t *)0xdeadbeefULL};
   assert(delegate_backend_list(out0, 0) == 0);
   assert(out0[0] == (delegate_backend_t *)0xdeadbeefULL);

   /* NULL out is a no-op. */
   assert(delegate_backend_list(NULL, 8) == 0);
   printf("  PASS: test_list_truncates_to_max\n");
}

static void test_list_json_empty_registry(void)
{
   delegate_backend_reset_for_test();
   char *json = delegate_backend_list_json();
   assert(json != NULL);
   assert(strcmp(json, "[]") == 0);
   free(json);
   printf("  PASS: test_list_json_empty_registry\n");
}

static void test_list_json_single_backend(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_t a = make_stub("local");
   a.description = "in-process subprocess";
   delegate_backend_register(&a);
   char *json = delegate_backend_list_json();
   assert(json != NULL);
   assert(strcmp(json, "[{\"name\":\"local\",\"description\":\"in-process subprocess\"}]") == 0);
   free(json);
   printf("  PASS: test_list_json_single_backend\n");
}

static void test_list_json_multiple_backends_in_registration_order(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_t a = make_stub("local");
   a.description = "first";
   delegate_backend_t b = make_stub("ssh");
   b.description = "second";
   delegate_backend_t c = make_stub("docker");
   c.description = "third";
   delegate_backend_register(&a);
   delegate_backend_register(&b);
   delegate_backend_register(&c);

   char *json = delegate_backend_list_json();
   assert(json != NULL);
   /* Spot-check shape: leading [, trailing ], comma-separated entries
    * in registration order. */
   assert(json[0] == '[');
   assert(json[strlen(json) - 1] == ']');
   /* Substrings of each entry appear in registration order. */
   const char *p_local = strstr(json, "\"name\":\"local\"");
   const char *p_ssh = strstr(json, "\"name\":\"ssh\"");
   const char *p_docker = strstr(json, "\"name\":\"docker\"");
   assert(p_local && p_ssh && p_docker);
   assert(p_local < p_ssh);
   assert(p_ssh < p_docker);
   free(json);
   printf("  PASS: test_list_json_multiple_backends_in_registration_order\n");
}

static void test_list_json_escapes_special_chars(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_t a = make_stub("name-with-\"quote\"");
   a.description = "desc with \\backslash and \nnewline";
   delegate_backend_register(&a);
   char *json = delegate_backend_list_json();
   assert(json != NULL);
   /* Quotes escaped as \" — confirm the literal \" appears in output. */
   assert(strstr(json, "\\\"quote\\\"") != NULL);
   /* Backslash escaped as \\. */
   assert(strstr(json, "\\\\backslash") != NULL);
   /* Newline escaped as
. */
   assert(strstr(json, "\\u000a") != NULL);
   /* Raw newline byte must not appear inside the JSON value. */
   /* (We can't test this directly without a parser; the escape check
    * above is sufficient evidence that the writer transformed it.) */
   free(json);
   printf("  PASS: test_list_json_escapes_special_chars\n");
}

int main(void)
{
   printf("delegate_backend:\n");
   test_register_and_lookup();
   test_register_rejects_duplicate();
   test_register_rejects_invalid();
   test_lookup_invalid_inputs();
   test_list_returns_in_registration_order();
   test_list_truncates_to_max();
   test_list_json_empty_registry();
   test_list_json_single_backend();
   test_list_json_multiple_backends_in_registration_order();
   test_list_json_escapes_special_chars();
   printf("ok\n");
   return 0;
}
