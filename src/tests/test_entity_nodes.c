/* test_entity_nodes.c: unit tests for entity_nodes canonical key helpers. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../modules/db2/c/entity_nodes.h"

static void test_encode_alphanumeric(void)
{
   char out[128];
   int n = db2_entity_node_encode_component("abc123", out, sizeof(out));
   assert(n == 6);
   assert(strcmp(out, "abc123") == 0);
}

static void test_encode_unescaped_set(void)
{
   char out[64];
   int n = db2_entity_node_encode_component("A.Z_a-z~0/9", out, sizeof(out));
   assert(n > 0);
   assert(strcmp(out, "A.Z_a-z~0/9") == 0);
}

static void test_encode_colon(void)
{
   char out[64];
   int n = db2_entity_node_encode_component("a:b", out, sizeof(out));
   assert(n == 5);
   assert(strcmp(out, "a%3Ab") == 0);
}

static void test_encode_percent(void)
{
   char out[64];
   int n = db2_entity_node_encode_component("50%", out, sizeof(out));
   assert(n == 5);
   assert(strcmp(out, "50%25") == 0);
}

static void test_encode_space(void)
{
   char out[64];
   int n = db2_entity_node_encode_component("a b", out, sizeof(out));
   assert(n == 5);
   assert(strcmp(out, "a%20b") == 0);
}

static void test_encode_control_byte(void)
{
   char in[] = {'a', 0x01, 'b', '\0'};
   char out[64];
   int n = db2_entity_node_encode_component(in, out, sizeof(out));
   assert(n == 5);
   assert(strcmp(out, "a%01b") == 0);
}

static void test_encode_utf8(void)
{
   /* UTF-8 for U+00E9 (e with acute): 0xC3 0xA9 */
   char in[] = {(char)0xC3, (char)0xA9, '\0'};
   char out[64];
   int n = db2_entity_node_encode_component(in, out, sizeof(out));
   assert(n == 6);
   assert(strcmp(out, "%C3%A9") == 0);
}

static void test_encode_null_input(void)
{
   char out[64];
   int n = db2_entity_node_encode_component(NULL, out, sizeof(out));
   assert(n == -1);
}

static void test_key_file(void)
{
   char out[GRAPH_ENDPOINT_MAX];
   int rc = db2_entity_node_key_file("aimee", "src/memory.c", out, sizeof(out));
   assert(rc == 0);
   assert(strncmp(out, "file:", 5) == 0);
   assert(strstr(out, "aimee") != NULL);
}

static void test_key_symbol(void)
{
   char out[GRAPH_ENDPOINT_MAX];
   int rc = db2_entity_node_key_symbol("aimee", "memory_graph_boost", out, sizeof(out));
   assert(rc == 0);
   assert(strncmp(out, "symbol:", 7) == 0);
}

static void test_key_concept(void)
{
   char out[GRAPH_ENDPOINT_MAX];
   int rc = db2_entity_node_key_concept("deploy", out, sizeof(out));
   assert(rc == 0);
   assert(strncmp(out, "concept:", 8) == 0);
   assert(strstr(out, "deploy") != NULL);
}

static void test_key_project(void)
{
   char out[GRAPH_ENDPOINT_MAX];
   int rc = db2_entity_node_key_project("aimee", out, sizeof(out));
   assert(rc == 0);
   assert(strncmp(out, "project:", 8) == 0);
}

static void test_key_null_inputs(void)
{
   char out[GRAPH_ENDPOINT_MAX];
   assert(db2_entity_node_key_file(NULL, "path", out, sizeof(out)) == -1);
   assert(db2_entity_node_key_file("proj", NULL, out, sizeof(out)) == -1);
   assert(db2_entity_node_key_symbol("proj", NULL, out, sizeof(out)) == -1);
   assert(db2_entity_node_key_concept(NULL, out, sizeof(out)) == -1);
   assert(db2_entity_node_key_project(NULL, out, sizeof(out)) == -1);
}

static void test_key_compact_long(void)
{
   /* A path long enough to exceed GRAPH_ENDPOINT_MAX-1 must compact. */
   char long_path[800];
   memset(long_path, 'a', sizeof(long_path) - 1);
   long_path[sizeof(long_path) - 1] = '\0';
   char out[GRAPH_ENDPOINT_MAX];
   int rc = db2_entity_node_key_file("aimee", long_path, out, sizeof(out));
   assert(rc == 0);
   assert(strncmp(out, "file:h:", 7) == 0);
   assert(strlen(out) == 7 + 32);
}

static void test_key_deterministic(void)
{
   char out1[GRAPH_ENDPOINT_MAX], out2[GRAPH_ENDPOINT_MAX];
   db2_entity_node_key_file("proj", "src/foo.c", out1, sizeof(out1));
   db2_entity_node_key_file("proj", "src/foo.c", out2, sizeof(out2));
   assert(strcmp(out1, out2) == 0);
}

static void test_node_origin_constants(void)
{
   assert(strcmp(ENTITY_NODE_ORIGIN_CODE_PROJECTION, "code_projection") == 0);
   assert(strcmp(ENTITY_NODE_ORIGIN_MEMORY_EXTRACTION, "memory_extraction") == 0);
   assert(strcmp(ENTITY_NODE_ORIGIN_SESSION, "session") == 0);
   assert(strcmp(ENTITY_NODE_ORIGIN_CONCEPT, "concept") == 0);
   assert(strcmp(ENTITY_NODE_ORIGIN_MANUAL, "manual") == 0);
}

static void test_alias_resolve_null_alias(void)
{
   char out[4][GRAPH_ENDPOINT_MAX];
   int n = db2_entity_node_resolve_alias(NULL, "aimee", out, 4);
   assert(n == 0);
}

static void test_alias_resolve_empty_alias(void)
{
   char out[4][GRAPH_ENDPOINT_MAX];
   int n = db2_entity_node_resolve_alias("", "aimee", out, 4);
   assert(n == 0);
}

static void test_alias_resolve_null_project(void)
{
   /* NULL project is valid — unscoped lookup. No DB in unit tests → 0. */
   char out[4][GRAPH_ENDPOINT_MAX];
   int n = db2_entity_node_resolve_alias("src/foo.c", NULL, out, 4);
   assert(n == 0);
}

int main(void)
{
   printf("test_encode_alphanumeric... ");
   test_encode_alphanumeric();
   printf("ok\n");
   printf("test_encode_unescaped_set... ");
   test_encode_unescaped_set();
   printf("ok\n");
   printf("test_encode_colon... ");
   test_encode_colon();
   printf("ok\n");
   printf("test_encode_percent... ");
   test_encode_percent();
   printf("ok\n");
   printf("test_encode_space... ");
   test_encode_space();
   printf("ok\n");
   printf("test_encode_control_byte... ");
   test_encode_control_byte();
   printf("ok\n");
   printf("test_encode_utf8... ");
   test_encode_utf8();
   printf("ok\n");
   printf("test_encode_null_input... ");
   test_encode_null_input();
   printf("ok\n");
   printf("test_key_file... ");
   test_key_file();
   printf("ok\n");
   printf("test_key_symbol... ");
   test_key_symbol();
   printf("ok\n");
   printf("test_key_concept... ");
   test_key_concept();
   printf("ok\n");
   printf("test_key_project... ");
   test_key_project();
   printf("ok\n");
   printf("test_key_null_inputs... ");
   test_key_null_inputs();
   printf("ok\n");
   printf("test_key_compact_long... ");
   test_key_compact_long();
   printf("ok\n");
   printf("test_key_deterministic... ");
   test_key_deterministic();
   printf("ok\n");
   printf("test_node_origin_constants... ");
   test_node_origin_constants();
   printf("ok\n");
   printf("test_alias_resolve_null_alias... ");
   test_alias_resolve_null_alias();
   printf("ok\n");
   printf("test_alias_resolve_empty_alias... ");
   test_alias_resolve_empty_alias();
   printf("ok\n");
   printf("test_alias_resolve_null_project... ");
   test_alias_resolve_null_project();
   printf("ok\n");
   printf("entity_nodes: all tests passed\n");
   return 0;
}
