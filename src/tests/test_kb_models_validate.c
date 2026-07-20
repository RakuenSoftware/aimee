/* test_kb_models_validate.c: P2a route-admission validators for the /v1/models routes.
 *
 * Pure unit coverage of the admission gate that the catalog CRUD routes apply before
 * touching the DB: the wire whitelist (mirrors the schema CHECK) and the printable /
 * bounded name check for model_id + provider. The entitled-resolution + admin-gate live
 * in SQL (SECURITY DEFINER + RLS) and are proven by scripts/p2a_catalog_rls_test.sql on
 * real Postgres; this test locks the C admission logic that guards those calls. */
#include "kb_models_validate.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg)                                                                            \
   do                                                                                               \
   {                                                                                                \
      if (!(cond))                                                                                  \
      {                                                                                             \
         printf("FAIL: %s\n", msg);                                                                 \
         failures++;                                                                                \
      }                                                                                             \
   } while (0)

int main(void)
{
   /* wire whitelist: exactly the four accepted wires, nothing else. */
   CHECK(kb_models_wire_valid("anthropic"), "anthropic is a valid wire");
   CHECK(kb_models_wire_valid("openai"), "openai is a valid wire");
   CHECK(kb_models_wire_valid("responses"), "responses is a valid wire");
   CHECK(kb_models_wire_valid("gemini"), "gemini is a valid wire");
   CHECK(!kb_models_wire_valid(""), "empty wire is rejected");
   CHECK(!kb_models_wire_valid("Anthropic"), "wire is case-sensitive (Anthropic rejected)");
   CHECK(!kb_models_wire_valid("bedrock"), "unknown wire is rejected");
   CHECK(!kb_models_wire_valid(NULL), "NULL wire is rejected");

   /* name check: printable, non-empty, within bound. */
   CHECK(kb_models_name_clean("claude-opus-4", 200), "ordinary model_id is clean");
   CHECK(kb_models_name_clean("x", 200), "single char is clean");
   CHECK(!kb_models_name_clean("", 200), "empty name is rejected");
   CHECK(!kb_models_name_clean(NULL, 200), "NULL name is rejected");

   /* control bytes / DEL must be rejected (no smuggling into stored state). */
   CHECK(!kb_models_name_clean("bad\nname", 200), "newline is rejected");
   CHECK(!kb_models_name_clean("bad\tname", 200), "tab is rejected");
   char del[] = {'a', (char)0x7f, 'b', 0};
   CHECK(!kb_models_name_clean(del, 200), "DEL byte is rejected");

   /* length bound is inclusive on max, exclusive above it. */
   char n200[201];
   memset(n200, 'm', 200);
   n200[200] = 0;
   CHECK(kb_models_name_clean(n200, 200), "200-char name at the bound is clean");
   char n201[202];
   memset(n201, 'm', 201);
   n201[201] = 0;
   CHECK(!kb_models_name_clean(n201, 200), "201-char name over the bound is rejected");
   /* provider bound is 100 in the route. */
   char n101[102];
   memset(n101, 'p', 101);
   n101[101] = 0;
   CHECK(!kb_models_name_clean(n101, 100), "101-char provider over the 100 bound is rejected");

   if (failures == 0)
      printf("test_kb_models_validate: ALL PASS\n");
   else
      printf("test_kb_models_validate: %d FAILURE(S)\n", failures);
   return failures == 0 ? 0 : 1;
}
