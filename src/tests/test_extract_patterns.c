/* test_extract_patterns.c: typed-fact §6 pattern-first extraction + §4
 * retraction scan. Pure logic. P5. */
#include "../headers/memory_extract_patterns.h"
#include "../headers/memory_ontology.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_classify(void)
{
   assert(memory_pattern_classify_value("192.168.1.254") == PAT_VAL_IPV4);
   assert(memory_pattern_classify_value("10.0.0.1") == PAT_VAL_IPV4);
   assert(memory_pattern_classify_value("256.0.0.1") == PAT_VAL_NONE); /* octet > 255 */
   assert(memory_pattern_classify_value("1.2.3") == PAT_VAL_NONE);     /* only 3 octets */
   assert(memory_pattern_classify_value("1.2.3.4.5") == PAT_VAL_NONE);

   assert(memory_pattern_classify_value("aa:bb:cc:dd:ee:ff") == PAT_VAL_MAC);
   assert(memory_pattern_classify_value("AA-BB-CC-DD-EE-FF") == PAT_VAL_MAC);
   assert(memory_pattern_classify_value("aa:bb:cc:dd:ee") == PAT_VAL_NONE); /* 5 groups */

   assert(memory_pattern_classify_value("fe80::1") == PAT_VAL_IPV6);
   assert(memory_pattern_classify_value("2001:db8:0:0:0:0:0:1") == PAT_VAL_IPV6);
   assert(memory_pattern_classify_value("::1") == PAT_VAL_IPV6);
   /* a 6-group colon form is a MAC, not IPv6 (MAC checked first). */
   assert(memory_pattern_classify_value("12:34:56:78:9a:bc") == PAT_VAL_MAC);

   assert(memory_pattern_classify_value("theo@example.com") == PAT_VAL_EMAIL);
   assert(memory_pattern_classify_value("a.b+c@sub.example.co") == PAT_VAL_EMAIL);
   assert(memory_pattern_classify_value("not-an-email") == PAT_VAL_NONE);
   assert(memory_pattern_classify_value("a@b") == PAT_VAL_NONE); /* no domain dot */

   assert(memory_pattern_classify_value("2026-06-13") == PAT_VAL_DATE);
   assert(memory_pattern_classify_value("2026-13-01") == PAT_VAL_NONE); /* month 13 */
   assert(memory_pattern_classify_value("2026-06-32") == PAT_VAL_NONE); /* day 32 */
   assert(memory_pattern_classify_value("06/13/2026") == PAT_VAL_NONE);

   assert(memory_pattern_classify_value("hello") == PAT_VAL_NONE);
   assert(memory_pattern_classify_value("") == PAT_VAL_NONE);
   assert(memory_pattern_classify_value(NULL) == PAT_VAL_NONE);

   /* value -> node kind mapping. */
   assert(memory_pattern_value_node_kind(PAT_VAL_IPV4) == NODE_IP);
   assert(memory_pattern_value_node_kind(PAT_VAL_IPV6) == NODE_IP);
   assert(memory_pattern_value_node_kind(PAT_VAL_MAC) == NODE_SCALAR);
   assert(memory_pattern_value_node_kind(PAT_VAL_EMAIL) == NODE_SCALAR);
   assert(memory_pattern_value_node_kind(PAT_VAL_DATE) == NODE_SCALAR);
   assert(memory_pattern_value_node_kind(PAT_VAL_NONE) == NODE_OTHER);
   printf("  PASS: test_classify\n");
}

static void test_retraction(void)
{
   assert(memory_pattern_is_retraction("forget that I have a dog") == 1);
   assert(memory_pattern_is_retraction("Actually, that's wrong") == 1);
   assert(memory_pattern_is_retraction("I NO LONGER work there") == 1);
   assert(memory_pattern_is_retraction("scratch that") == 1);
   assert(memory_pattern_is_retraction("please disregard the last thing") == 1);
   assert(memory_pattern_is_retraction("my name is Theo") == 0);
   /* tightened cues: a bare "forget" reminder is not a retraction. */
   assert(memory_pattern_is_retraction("don't forget to call mom") == 0);
   assert(memory_pattern_is_retraction("") == 0);
   assert(memory_pattern_is_retraction(NULL) == 0);
   printf("  PASS: test_retraction\n");
}

static void test_extract(void)
{
   pattern_triple_t t[8];

   /* canonical personal-fact template, value typed by its shape. */
   int n = memory_extract_patterns("my email is theo@example.com.", t, 8);
   assert(n == 1);
   assert(strcmp(t[0].subject, "user") == 0 && t[0].subject_kind == NODE_PERSON);
   assert(strcmp(t[0].rel_type, "email") == 0);
   assert(strcmp(t[0].object, "theo@example.com") == 0);
   assert(t[0].object_kind == NODE_SCALAR);

   /* multi-word attribute -> snake_case rel_type; IPv4 object kind. */
   n = memory_extract_patterns("My home ip is 192.168.1.254", t, 8);
   assert(n == 1);
   assert(strcmp(t[0].rel_type, "home_ip") == 0);
   assert(strcmp(t[0].object, "192.168.1.254") == 0);
   assert(t[0].object_kind == NODE_IP);

   /* free-text value -> NODE_OTHER, still a triple. */
   n = memory_extract_patterns("my name is Theo", t, 8);
   assert(n == 1 && strcmp(t[0].rel_type, "name") == 0 && strcmp(t[0].object, "Theo") == 0 &&
          t[0].object_kind == NODE_OTHER);

   /* two facts in one input. */
   n = memory_extract_patterns("my dog is Rex. my city is Berlin.", t, 8);
   assert(n == 2);
   assert(strcmp(t[0].rel_type, "dog") == 0 && strcmp(t[0].object, "Rex") == 0);
   assert(strcmp(t[1].rel_type, "city") == 0 && strcmp(t[1].object, "Berlin") == 0);

   /* "army" must not match the "my" word boundary. */
   assert(memory_extract_patterns("the army is large", t, 8) == 0);
   /* empty attr / empty value yield nothing. */
   assert(memory_extract_patterns("my is here", t, 8) == 0);
   assert(memory_extract_patterns("my car is .", t, 8) == 0);
   /* no template -> no triple (left for the model). */
   assert(memory_extract_patterns("the server crashed last night", t, 8) == 0);

   /* bad args. */
   assert(memory_extract_patterns(NULL, t, 8) == -1);
   assert(memory_extract_patterns("my x is y", NULL, 8) == -1);
   assert(memory_extract_patterns("my x is y", t, 0) == -1);
   printf("  PASS: test_extract\n");
}

int main(void)
{
   test_classify();
   test_retraction();
   test_extract();
   printf("extract_patterns: all tests passed\n");
   return 0;
}
