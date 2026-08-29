#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tool_args_coerce.h"

static void test_string_to_integer(void)
{
   cJSON *schema =
       cJSON_Parse("{\"type\":\"object\",\"properties\":{\"n\":{\"type\":\"integer\"}}}");
   cJSON *raw = cJSON_Parse("{\"n\":\"42\"}");
   cJSON *out = tool_args_coerce(schema, raw);
   cJSON *n = cJSON_GetObjectItemCaseSensitive(out, "n");
   assert(cJSON_IsNumber(n));
   assert(n->valueint == 42);
   cJSON_Delete(out);
   cJSON_Delete(raw);
   cJSON_Delete(schema);
   printf("  PASS: test_string_to_integer\n");
}

static void test_string_to_number(void)
{
   cJSON *schema =
       cJSON_Parse("{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"}}}");
   cJSON *raw = cJSON_Parse("{\"x\":\"3.14\"}");
   cJSON *out = tool_args_coerce(schema, raw);
   cJSON *x = cJSON_GetObjectItemCaseSensitive(out, "x");
   assert(cJSON_IsNumber(x));
   assert(x->valuedouble > 3.13 && x->valuedouble < 3.15);
   cJSON_Delete(out);
   cJSON_Delete(raw);
   cJSON_Delete(schema);
   printf("  PASS: test_string_to_number\n");
}

static void test_string_to_boolean(void)
{
   cJSON *schema =
       cJSON_Parse("{\"type\":\"object\",\"properties\":{\"b\":{\"type\":\"boolean\"}}}");

   /* true */
   cJSON *raw = cJSON_Parse("{\"b\":\"true\"}");
   cJSON *out = tool_args_coerce(schema, raw);
   cJSON *b = cJSON_GetObjectItemCaseSensitive(out, "b");
   assert(cJSON_IsTrue(b));
   cJSON_Delete(out);
   cJSON_Delete(raw);

   /* false */
   raw = cJSON_Parse("{\"b\":\"false\"}");
   out = tool_args_coerce(schema, raw);
   b = cJSON_GetObjectItemCaseSensitive(out, "b");
   assert(cJSON_IsFalse(b));
   cJSON_Delete(out);
   cJSON_Delete(raw);

   cJSON_Delete(schema);
   printf("  PASS: test_string_to_boolean\n");
}

static void test_scalar_to_array_wrap(void)
{
   cJSON *schema =
       cJSON_Parse("{\"type\":\"object\",\"properties\":{\"items\":{\"type\":\"array\"}}}");
   cJSON *raw = cJSON_Parse("{\"items\":\"x\"}");
   cJSON *out = tool_args_coerce(schema, raw);
   cJSON *items = cJSON_GetObjectItemCaseSensitive(out, "items");
   assert(cJSON_IsArray(items));
   assert(items->child != NULL);
   assert(items->child->next == NULL);
   assert(cJSON_IsString(items->child));
   assert(strcmp(items->child->valuestring, "x") == 0);
   cJSON_Delete(out);
   cJSON_Delete(raw);
   cJSON_Delete(schema);
   printf("  PASS: test_scalar_to_array_wrap\n");
}

static void test_json_string_to_object(void)
{
   cJSON *schema =
       cJSON_Parse("{\"type\":\"object\",\"properties\":{\"meta\":{\"type\":\"object\"}}}");

   /* Build raw = {"meta":"{\"k\":1}"} using cJSON_Parse for inner string. */
   cJSON *raw = cJSON_CreateObject();
   cJSON_AddItemToObject(raw, "meta", cJSON_CreateString("{\"k\":1}"));

   cJSON *out = tool_args_coerce(schema, raw);
   cJSON *meta = cJSON_GetObjectItemCaseSensitive(out, "meta");
   assert(cJSON_IsObject(meta));
   cJSON *k = cJSON_GetObjectItemCaseSensitive(meta, "k");
   assert(cJSON_IsNumber(k));
   assert(k->valueint == 1);
   cJSON_Delete(out);
   cJSON_Delete(raw);
   cJSON_Delete(schema);
   printf("  PASS: test_json_string_to_object\n");
}

static void test_already_correct_passthrough(void)
{
   cJSON *schema =
       cJSON_Parse("{\"type\":\"object\",\"properties\":{\"n\":{\"type\":\"integer\"}}}");
   cJSON *raw = cJSON_Parse("{\"n\":42}");
   cJSON *out = tool_args_coerce(schema, raw);
   cJSON *n = cJSON_GetObjectItemCaseSensitive(out, "n");
   assert(cJSON_IsNumber(n));
   assert(n->valueint == 42);
   cJSON_Delete(out);
   cJSON_Delete(raw);
   cJSON_Delete(schema);
   printf("  PASS: test_already_correct_passthrough\n");
}

static void test_null_schema_clones(void)
{
   cJSON *raw = cJSON_Parse("{\"n\":99}");
   cJSON *out = tool_args_coerce(NULL, raw);
   assert(out != NULL);
   cJSON *n = cJSON_GetObjectItemCaseSensitive(out, "n");
   assert(cJSON_IsNumber(n));
   assert(n->valueint == 99);
   cJSON_Delete(out);
   cJSON_Delete(raw);
   printf("  PASS: test_null_schema_clones\n");
}

static void test_failed_coercion_leaves_alone(void)
{
   cJSON *schema = cJSON_Parse("{\"type\":\"object\",\"properties\":{\"n\":{\"type\":\"integer\"},"
                               "\"b\":{\"type\":\"boolean\"}}}");
   cJSON *raw = cJSON_Parse("{\"n\":\"abc\",\"b\":\"true\"}");
   cJSON *out = tool_args_coerce(schema, raw);

   /* n should still be string "abc" */
   cJSON *n = cJSON_GetObjectItemCaseSensitive(out, "n");
   assert(cJSON_IsString(n));
   assert(strcmp(n->valuestring, "abc") == 0);

   /* b should be bool true */
   cJSON *b = cJSON_GetObjectItemCaseSensitive(out, "b");
   assert(cJSON_IsTrue(b));

   cJSON_Delete(out);
   cJSON_Delete(raw);
   cJSON_Delete(schema);
   printf("  PASS: test_failed_coercion_leaves_alone\n");
}

/* --- Canonicalization: one shape, decided once ---------------------------
 *
 * These pin the defect the canonicalizer exists to close. Arguments used to be
 * normalized twice, differently, and authorized on neither result: the
 * validator resolved aliases on a copy it discarded, the dispatcher resolved a
 * different alias set and executed THAT, and execution-policy plus the audit
 * record saw the raw arguments in between. */

/* The load-bearing property: canonicalizing is a fixed point. The agent loop
 * canonicalizes before its gates and the dispatcher canonicalizes again for its
 * other entry points, so a second pass must not transform anything a second
 * time -- a scalar wrapped into an array once must not become [[x]]. */
static void test_canonicalize_is_idempotent(void)
{
   cJSON *schema = cJSON_Parse("{\"type\":\"object\",\"properties\":{"
                               "\"paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
                               "\"n\":{\"type\":\"integer\"},"
                               "\"deep\":{\"type\":\"boolean\"}}}");
   cJSON *raw = cJSON_Parse("{\"paths\":\"/etc/passwd\",\"n\":\"7\",\"deep\":\"true\"}");

   cJSON *once = tool_args_canonicalize(schema, raw);
   cJSON *twice = tool_args_canonicalize(schema, once);

   char *a = cJSON_PrintUnformatted(once);
   char *b = cJSON_PrintUnformatted(twice);
   assert(strcmp(a, b) == 0);

   cJSON *paths = cJSON_GetObjectItemCaseSensitive(once, "paths");
   assert(cJSON_IsArray(paths) && cJSON_GetArraySize(paths) == 1);
   assert(cJSON_IsString(cJSON_GetArrayItem(paths, 0)));
   assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(once, "n")));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(once, "deep")));

   free(a);
   free(b);
   cJSON_Delete(twice);
   cJSON_Delete(once);
   cJSON_Delete(raw);
   cJSON_Delete(schema);
   printf("  PASS: test_canonicalize_is_idempotent\n");
}

/* An alias is rewritten only when the canonical name is a real parameter and
 * the alias is not. This is what makes the table safe to actually apply: the
 * dispatcher's old unguarded rewrite would rename a tool's genuine `file`
 * argument to `path` and the tool would lose it. */
static void test_alias_rewrites_only_toward_a_declared_parameter(void)
{
   cJSON *schema =
       cJSON_Parse("{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}");
   cJSON *raw = cJSON_Parse("{\"filepath\":\"/tmp/x\"}");
   cJSON *out = tool_args_canonicalize(schema, raw);
   assert(cJSON_GetObjectItemCaseSensitive(out, "path") != NULL);
   assert(cJSON_GetObjectItemCaseSensitive(out, "filepath") == NULL);
   cJSON_Delete(out);
   cJSON_Delete(raw);
   cJSON_Delete(schema);

   /* A tool whose real parameter IS `file` keeps it. */
   cJSON *schema2 =
       cJSON_Parse("{\"type\":\"object\",\"properties\":{\"file\":{\"type\":\"string\"}}}");
   cJSON *raw2 = cJSON_Parse("{\"file\":\"/tmp/y\"}");
   cJSON *out2 = tool_args_canonicalize(schema2, raw2);
   cJSON *kept = cJSON_GetObjectItemCaseSensitive(out2, "file");
   assert(kept && cJSON_IsString(kept) && strcmp(kept->valuestring, "/tmp/y") == 0);
   assert(cJSON_GetObjectItemCaseSensitive(out2, "path") == NULL);
   cJSON_Delete(out2);
   cJSON_Delete(raw2);
   cJSON_Delete(schema2);

   /* An explicit `path` already present is never overwritten by an alias. */
   cJSON *schema3 =
       cJSON_Parse("{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}");
   cJSON *raw3 = cJSON_Parse("{\"path\":\"/real\",\"filepath\":\"/decoy\"}");
   cJSON *out3 = tool_args_canonicalize(schema3, raw3);
   cJSON *p = cJSON_GetObjectItemCaseSensitive(out3, "path");
   assert(p && strcmp(p->valuestring, "/real") == 0);
   cJSON_Delete(out3);
   cJSON_Delete(raw3);
   cJSON_Delete(schema3);
   printf("  PASS: test_alias_rewrites_only_toward_a_declared_parameter\n");
}

/* The whole point: the bytes the gates authorize are the bytes that execute.
 * Before the canonicalizer, the authorizer saw {"paths":"/etc/passwd"} while
 * the tool ran {"paths":["/etc/passwd"]}. */
static void test_authorized_bytes_equal_executed_bytes(void)
{
   cJSON *schema = cJSON_Parse("{\"type\":\"object\",\"properties\":{"
                               "\"paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}}");
   const char *raw = "{\"paths\":\"/etc/passwd\"}";

   /* What the agent loop computes once and hands to every gate. */
   char *gate_view = tool_args_canonicalize_json(schema, raw);
   assert(gate_view != NULL);

   /* What the dispatcher independently derives for its own entry points. */
   cJSON *parsed = cJSON_Parse(gate_view);
   cJSON *executed = tool_args_canonicalize(schema, parsed);
   char *executed_view = cJSON_PrintUnformatted(executed);

   assert(strcmp(gate_view, executed_view) == 0);
   assert(strcmp(gate_view, raw) != 0); /* load-bearing: the shape really did change */

   free(executed_view);
   cJSON_Delete(executed);
   cJSON_Delete(parsed);
   free(gate_view);
   cJSON_Delete(schema);
   printf("  PASS: test_authorized_bytes_equal_executed_bytes\n");
}

/* Unparseable arguments are the validator's error to report, not something to
 * paper over here: the caller keeps the raw string so the failure surfaces with
 * a useful message instead of being silently replaced with {}. */
static void test_invalid_json_is_left_for_the_validator(void)
{
   cJSON *schema = cJSON_Parse("{\"type\":\"object\",\"properties\":{}}");
   assert(tool_args_canonicalize_json(schema, "{not json") == NULL);
   cJSON_Delete(schema);
   printf("  PASS: test_invalid_json_is_left_for_the_validator\n");
}

int main(void)
{
   printf("tool_args_coerce:\n");
   test_string_to_integer();
   test_string_to_number();
   test_string_to_boolean();
   test_scalar_to_array_wrap();
   test_json_string_to_object();
   test_already_correct_passthrough();
   test_null_schema_clones();
   test_failed_coercion_leaves_alone();
   test_canonicalize_is_idempotent();
   test_alias_rewrites_only_toward_a_declared_parameter();
   test_authorized_bytes_equal_executed_bytes();
   test_invalid_json_is_left_for_the_validator();
   printf("ok\n");
   return 0;
}
