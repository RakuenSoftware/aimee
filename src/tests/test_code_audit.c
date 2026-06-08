/* test_code_audit.c: unit tests for the P4 code-audit pure helpers
 * (file classification, stem extraction, TODO counting, debt scoring). The
 * tree-walking handler is exercised by a functional smoke, not here. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cli_code_audit.h"

static void test_is_code_file(void)
{
   assert(audit_is_code_file("src/foo.c"));
   assert(audit_is_code_file("a/b/x.py"));
   assert(audit_is_code_file("x.tsx"));
   assert(!audit_is_code_file("README.md"));
   assert(!audit_is_code_file("data.json"));
   assert(!audit_is_code_file("noext"));
   printf("is_code_file OK\n");
}

static void test_is_test_file(void)
{
   assert(audit_is_test_file("src/tests/test_x.c"));
   assert(audit_is_test_file("a/__tests__/x.ts"));
   assert(audit_is_test_file("x.test.ts"));
   assert(audit_is_test_file("foo_test.go"));
   assert(audit_is_test_file("test_foo.py"));
   assert(!audit_is_test_file("src/foo.c"));
   assert(!audit_is_test_file("src/contest.c")); /* not a test despite 'test' substring in name */
   printf("is_test_file OK\n");
}

static void test_stem(void)
{
   char s[256];
   audit_stem("src/server/foo.c", s, sizeof(s));
   assert(strcmp(s, "foo") == 0);
   audit_stem("tests/test_foo.c", s, sizeof(s));
   assert(strcmp(s, "foo") == 0);
   audit_stem("pkg/foo_test.go", s, sizeof(s));
   assert(strcmp(s, "foo") == 0);
   audit_stem("a/foo.test.ts", s, sizeof(s));
   assert(strcmp(s, "foo") == 0);
   audit_stem("x/bar_spec.rb", s, sizeof(s));
   assert(strcmp(s, "bar") == 0);
   printf("stem OK\n");
}

static void test_count_todos(void)
{
   assert(audit_count_todos("clean code here") == 0);
   assert(audit_count_todos("// TODO: fix\n/* FIXME */ XXX HACK") == 4);
   assert(audit_count_todos(NULL) == 0);
   printf("count_todos OK\n");
}

static void test_debt_score(void)
{
   assert(audit_debt_score(0, 0, 0) == 100);   /* no code -> clean */
   assert(audit_debt_score(100, 0, 0) == 100); /* all tested, no todos */
   /* all untested -> 60-pt penalty -> 40 */
   assert(audit_debt_score(100, 100, 0) == 40);
   /* half untested -> 30-pt penalty -> 70 */
   assert(audit_debt_score(100, 50, 0) == 70);
   /* todos drag the score down but stay clamped >= 0 */
   int s = audit_debt_score(10, 10, 1000);
   assert(s >= 0 && s <= 100);
   printf("debt_score OK\n");
}

int main(void)
{
   printf("code_audit: ");
   test_is_code_file();
   test_is_test_file();
   test_stem();
   test_count_todos();
   test_debt_score();
   printf("all tests passed\n");
   return 0;
}
