/* test_code_match.c: ingress-compression P1b — code_match_line() locates the
 * 1-based line of a ts_headline/snippet match (">>>token<<<") within file content. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "code_match.h"

#define PASS(name) printf("  PASS: %s\n", (name))

static const char *CONTENT = "#include <stdio.h>\n"         /* line 1 */
                             "\n"                           /* line 2 */
                             "int add(int a, int b)\n"      /* line 3 */
                             "{\n"                          /* line 4 */
                             "   return a + b;\n"           /* line 5 */
                             "}\n"                          /* line 6 */
                             "\n"                           /* line 7 */
                             "int subtract(int a, int b)\n" /* line 8 */
                             "{\n"                          /* line 9 */
                             "   return a - b;\n"           /* line 10 */
                             "}\n";                         /* line 11 */

int main(void)
{
   /* 1. Match on line 3 (the add signature). */
   assert(code_match_line(CONTENT, "...int >>>add<<<(int a, int b)...") == 3);
   PASS("match maps to line 3");

   /* 2. Match a token unique to line 10. */
   assert(code_match_line(CONTENT, ">>>subtract<<<") == 8);
   PASS("match maps to line 8");

   /* 3. Match on line 1. */
   assert(code_match_line(CONTENT, ">>>stdio<<<") == 1);
   PASS("match maps to line 1");

   /* 4. No markers -> 0 (unknown). */
   assert(code_match_line(CONTENT, "plain snippet no markers") == 0);
   PASS("no markers -> 0");

   /* 5. Empty token -> 0. */
   assert(code_match_line(CONTENT, ">>><<<") == 0);
   PASS("empty token -> 0");

   /* 6. Token not present in content -> 0. */
   assert(code_match_line(CONTENT, ">>>nonexistent_token_zzz<<<") == 0);
   PASS("absent token -> 0");

   /* 7. NULL guards. */
   assert(code_match_line(NULL, ">>>x<<<") == 0);
   assert(code_match_line(CONTENT, NULL) == 0);
   PASS("NULL guards");

   /* 8. First occurrence wins (token appears on multiple lines: "return" is on 5 and 10). */
   assert(code_match_line(CONTENT, ">>>return<<<") == 5);
   PASS("first occurrence wins");

   printf("All code_match tests passed.\n");
   return 0;
}
