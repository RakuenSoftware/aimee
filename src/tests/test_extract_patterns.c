/* test_extract_patterns.c: typed-fact §6 pattern-first extraction + §4
 * retraction scan. Pure logic. P5. */
#include "modules/memory/memory_extract_patterns.h"
#include "modules/memory/memory_ontology.h"
#include <aimee/memory/module_api.h>
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

static void test_possessive_attr(void)
{
   char a[64];
   assert(memory_pattern_possessive_attr("forget my email", a, sizeof(a)) == 1);
   assert(strcmp(a, "email") == 0);
   assert(memory_pattern_possessive_attr("please forget my favorite color", a, sizeof(a)) == 1);
   assert(strcmp(a, "favorite color") == 0);
   /* the value clause after "is" is not part of the attribute. */
   assert(memory_pattern_possessive_attr("my email is wrong", a, sizeof(a)) == 1);
   assert(strcmp(a, "email") == 0);
   /* trailing punctuation / words capped. */
   assert(memory_pattern_possessive_attr("forget my city.", a, sizeof(a)) == 1);
   assert(strcmp(a, "city") == 0);
   /* no "my <attr>" possessive -> 0. */
   assert(memory_pattern_possessive_attr("that's wrong", a, sizeof(a)) == 0);
   assert(memory_pattern_possessive_attr("the army is here", a, sizeof(a)) ==
          0); /* word boundary */
   assert(memory_pattern_possessive_attr("", a, sizeof(a)) == 0);
   assert(memory_pattern_possessive_attr(NULL, a, sizeof(a)) == 0);
   printf("  PASS: test_possessive_attr\n");
}

/* --- the module seam ------------------------------------------------------
 * With an extractor registered the triples come from the memory module, not
 * from the local scan. A recorder stands in for that module, so the seam is
 * tested without a running bus. */

static struct
{
   int calls;
   int fail;
   char text[256];
   int max;
} g_extractor_state;

static int recording_extractor(const char *text, pattern_triple_t *out, int max, int *count)
{
   g_extractor_state.calls++;
   g_extractor_state.max = max;
   snprintf(g_extractor_state.text, sizeof(g_extractor_state.text), "%s", text ? text : "");
   if (g_extractor_state.fail)
      return -1;
   /* Deliberately not what the local scan would say, so a fallback is visible. */
   memset(&out[0], 0, sizeof(out[0]));
   snprintf(out[0].subject, sizeof(out[0].subject), "user");
   snprintf(out[0].rel_type, sizeof(out[0].rel_type), "from_the_module");
   snprintf(out[0].object, sizeof(out[0].object), "yes");
   out[0].subject_kind = NODE_PERSON;
   out[0].object_kind = NODE_SCALAR;
   *count = 1;
   return 0;
}

static void test_registered_extractor_decides(void)
{
   pattern_triple_t t[8];
   memset(&g_extractor_state, 0, sizeof(g_extractor_state));
   memory_extract_register_extractor(recording_extractor);

   /* Text the local scan would read as (user, name, Theo). */
   assert(memory_extract_patterns("my name is Theo", t, 8) == 1);
   assert(g_extractor_state.calls == 1);
   assert(strcmp(g_extractor_state.text, "my name is Theo") == 0);
   assert(g_extractor_state.max == 8);
   assert(strcmp(t[0].rel_type, "from_the_module") == 0);

   /* Bad args are still rejected before the module is troubled with them. */
   assert(memory_extract_patterns(NULL, t, 8) == -1);
   assert(memory_extract_patterns("my name is Theo", t, 0) == -1);
   assert(g_extractor_state.calls == 1);

   /* Unregistering restores the local scan. */
   memory_extract_register_extractor(NULL);
   assert(memory_extract_patterns("my name is Theo", t, 8) == 1);
   assert(strcmp(t[0].rel_type, "name") == 0);
   assert(g_extractor_state.calls == 1);
   printf("  PASS: test_registered_extractor_decides\n");
}

static void test_extractor_failure_is_not_no_facts(void)
{
   pattern_triple_t t[8];
   memset(&g_extractor_state, 0, sizeof(g_extractor_state));
   g_extractor_state.fail = 1;
   memory_extract_register_extractor(recording_extractor);

   /* Text the local scan would extract a fact from. -1, not 0: zero triples
    * means the text held no facts, and a broken module must not be able to say
    * that — nor may the gate fall back and make the module look healthy. */
   assert(memory_extract_patterns("my name is Theo", t, 8) == -1);
   assert(g_extractor_state.calls == 1);

   memory_extract_register_extractor(NULL);
   assert(memory_extract_patterns("my name is Theo", t, 8) == 1);
   printf("  PASS: test_extractor_failure_is_not_no_facts\n");
}

/* Build a one-triple response with a subject of exactly `subject_len` bytes,
 * all of them actually present. Returns the encoded length. */
static size_t build_one_triple_response(uint8_t *out, uint32_t subject_len)
{
   aimee_memory_put_u32(out, AIMEE_MEMORY_EXTRACT_RESPONSE_MAGIC);
   aimee_memory_put_u32(out + 4, 1);
   size_t offset = 8;
   aimee_memory_put_u32(out + offset, NODE_PERSON);
   aimee_memory_put_u32(out + offset + 4, NODE_SCALAR);
   offset += 8;
   aimee_memory_put_u32(out + offset, subject_len);
   memset(out + offset + 4, 'a', subject_len);
   offset += 4 + subject_len;
   const char *rest[2] = {"name", "Theo"};
   for (int i = 0; i < 2; i++)
   {
      uint32_t len = (uint32_t)strlen(rest[i]);
      aimee_memory_put_u32(out + offset, len);
      memcpy(out + offset + 4, rest[i], len);
      offset += 4 + len;
   }
   return offset;
}

/* The Go stage encodes and decodes these offsets independently; pinning them
 * here is what makes the two halves one contract rather than two guesses. */
static void test_extract_wire_layout(void)
{
   uint8_t request[AIMEE_MEMORY_EXTRACT_REQUEST_HEADER_LEN + 32];
   const char *text = "my name is Theo";
   assert(aimee_memory_extract_request_size(text) ==
          AIMEE_MEMORY_EXTRACT_REQUEST_HEADER_LEN + strlen(text));
   assert(aimee_memory_extract_request_encode(text, 16, request, sizeof(request)) == 0);
   assert(aimee_memory_get_u32(request) == AIMEE_MEMORY_EXTRACT_REQUEST_MAGIC);
   assert(aimee_memory_get_u32(request + 4) == AIMEE_MEMORY_WIRE_VERSION);
   assert(aimee_memory_get_u32(request + 8) == 16u);
   assert(aimee_memory_get_u32(request + 12) == (uint32_t)strlen(text));
   assert(memcmp(request + 16, text, strlen(text)) == 0);

   /* An empty text is a real turn shape, not a bad argument. Asking for zero
    * triples is the bad argument, since no answer could be returned. */
   assert(aimee_memory_extract_request_encode("", 16, request, sizeof(request)) == 0);
   assert(aimee_memory_get_u32(request + 12) == 0u);
   assert(aimee_memory_extract_request_encode(text, 0, request, sizeof(request)) == -1);
   assert(aimee_memory_extract_request_encode(text, 16, request, 4) == -1);

   /* A response built the way the Go stage builds one. */
   uint8_t response[AIMEE_MEMORY_EXTRACT_RESPONSE_MAX(2)];
   size_t offset = 0;
   aimee_memory_put_u32(response, AIMEE_MEMORY_EXTRACT_RESPONSE_MAGIC);
   aimee_memory_put_u32(response + 4, 1);
   offset = 8;
   aimee_memory_put_u32(response + offset, NODE_PERSON);
   aimee_memory_put_u32(response + offset + 4, NODE_SCALAR);
   offset += 8;
   const char *fields[3] = {"user", "name", "Theo"};
   for (int i = 0; i < 3; i++)
   {
      uint32_t len = (uint32_t)strlen(fields[i]);
      aimee_memory_put_u32(response + offset, len);
      memcpy(response + offset + 4, fields[i], len);
      offset += 4 + len;
   }

   aimee_memory_triple_t out[2];
   uint32_t count = 0;
   assert(aimee_memory_extract_response_decode(response, offset, out, 2, &count) == 0);
   assert(count == 1);
   assert(strcmp(out[0].subject, "user") == 0 && strcmp(out[0].rel_type, "name") == 0 &&
          strcmp(out[0].object, "Theo") == 0);
   assert(out[0].subject_kind == NODE_PERSON && out[0].object_kind == NODE_SCALAR);

   /* Trailing bytes mean the two sides disagree about the shape; the prefix that
    * happened to parse is not an answer. */
   response[offset] = 0;
   assert(aimee_memory_extract_response_decode(response, offset + 1, out, 2, &count) == -1);
   /* Truncation, a bad magic, and more triples than were asked for. */
   assert(aimee_memory_extract_response_decode(response, offset - 1, out, 2, &count) == -1);
   aimee_memory_put_u32(response, AIMEE_MEMORY_EXTRACT_RESPONSE_MAGIC + 1);
   assert(aimee_memory_extract_response_decode(response, offset, out, 2, &count) == -1);
   aimee_memory_put_u32(response, AIMEE_MEMORY_EXTRACT_RESPONSE_MAGIC);
   aimee_memory_put_u32(response + 4, 3);
   assert(aimee_memory_extract_response_decode(response, offset, out, 2, &count) == -1);

   /* A field longer than its destination is refused, never truncated: a
    * shortened relation label normalizes to a different name.
    *
    * The bytes have to actually be present, or the truncation check rejects the
    * response first and the capacity check is never reached — which is how this
    * assertion passed while the capacity check was disabled. */
   size_t at_bound = build_one_triple_response(response, AIMEE_MEMORY_TRIPLE_SUBJECT_MAX - 1u);
   assert(at_bound <= sizeof(response));
   assert(aimee_memory_extract_response_decode(response, at_bound, out, 2, &count) == 0);
   assert(count == 1 && strlen(out[0].subject) == AIMEE_MEMORY_TRIPLE_SUBJECT_MAX - 1u);

   size_t past_bound = build_one_triple_response(response, AIMEE_MEMORY_TRIPLE_SUBJECT_MAX);
   assert(past_bound <= sizeof(response));
   assert(aimee_memory_extract_response_decode(response, past_bound, out, 2, &count) == -1);
   printf("  PASS: test_extract_wire_layout\n");
}

/* --- the turn-scan seam ---------------------------------------------------
 * The §4 correction pre-scan asks both of its questions in one call. */

static struct
{
   int calls;
   int fail;
   char text[256];
} g_scan_state;

static int recording_scanner(const char *text, memory_pattern_turn_t *out)
{
   g_scan_state.calls++;
   snprintf(g_scan_state.text, sizeof(g_scan_state.text), "%s", text ? text : "");
   if (g_scan_state.fail)
      return -1;
   /* Deliberately the opposite of the local scan on both counts. */
   out->is_retraction = 1;
   out->has_attr = 1;
   snprintf(out->attr, sizeof(out->attr), "from_the_module");
   return 0;
}

static void test_registered_turn_scanner_decides(void)
{
   memory_pattern_turn_t scan;

   /* With nothing registered the scan is the two local functions. */
   memory_extract_register_turn_scanner(NULL);
   assert(memory_pattern_scan_turn("forget my email", &scan) == 0);
   assert(scan.is_retraction == 1 && scan.has_attr == 1);
   assert(strcmp(scan.attr, "email") == 0);
   assert(memory_pattern_scan_turn("the weather is nice", &scan) == 0);
   assert(scan.is_retraction == 0 && scan.has_attr == 0 && scan.attr[0] == '\0');

   memset(&g_scan_state, 0, sizeof(g_scan_state));
   memory_extract_register_turn_scanner(recording_scanner);
   /* Text the local scan reads as neither a retraction nor a possessive. */
   assert(memory_pattern_scan_turn("the weather is nice", &scan) == 0);
   assert(g_scan_state.calls == 1);
   assert(strcmp(g_scan_state.text, "the weather is nice") == 0);
   assert(scan.is_retraction == 1 && scan.has_attr == 1);
   assert(strcmp(scan.attr, "from_the_module") == 0);

   /* Bad args never reach the module. */
   assert(memory_pattern_scan_turn(NULL, &scan) == -1);
   assert(memory_pattern_scan_turn("x", NULL) == -1);
   assert(g_scan_state.calls == 1);

   memory_extract_register_turn_scanner(NULL);
   assert(memory_pattern_scan_turn("the weather is nice", &scan) == 0);
   assert(scan.is_retraction == 0);
   printf("  PASS: test_registered_turn_scanner_decides\n");
}

static void test_scan_turn_wire_layout(void)
{
   /* A failing scanner is reported, never guessed at: this drives a deletion. */
   memory_pattern_turn_t scan;
   memset(&g_scan_state, 0, sizeof(g_scan_state));
   g_scan_state.fail = 1;
   memory_extract_register_turn_scanner(recording_scanner);
   assert(memory_pattern_scan_turn("forget my email", &scan) == -1);
   assert(g_scan_state.calls == 1);
   memory_extract_register_turn_scanner(NULL);

   const char *turn = "forget my email";
   uint8_t request[AIMEE_MEMORY_SCAN_REQUEST_HEADER_LEN + 32];
   assert(aimee_memory_scan_request_size(turn) ==
          AIMEE_MEMORY_SCAN_REQUEST_HEADER_LEN + strlen(turn));
   assert(aimee_memory_scan_request_encode(turn, request, sizeof(request)) == 0);
   assert(aimee_memory_get_u32(request) == AIMEE_MEMORY_SCAN_REQUEST_MAGIC);
   assert(aimee_memory_get_u32(request + 4) == AIMEE_MEMORY_WIRE_VERSION);
   assert(aimee_memory_get_u32(request + 8) == (uint32_t)strlen(turn));
   assert(memcmp(request + 12, turn, strlen(turn)) == 0);
   assert(aimee_memory_scan_request_encode(turn, request, 4) == -1);

   uint8_t response[AIMEE_MEMORY_SCAN_RESPONSE_MAX];
   int retraction = -1, has_attr = -1;
   char attr[AIMEE_MEMORY_SCAN_ATTR_MAX];
   aimee_memory_put_u32(response, AIMEE_MEMORY_SCAN_RESPONSE_MAGIC);
   aimee_memory_put_u32(response + 4, 1);
   aimee_memory_put_u32(response + 8, 1);
   aimee_memory_put_u32(response + 12, 5);
   memcpy(response + 16, "email", 5);
   assert(aimee_memory_scan_response_decode(response, 21, &retraction, &has_attr, attr,
                                            sizeof(attr)) == 0);
   assert(retraction == 1 && has_attr == 1 && strcmp(attr, "email") == 0);

   /* No possessive: no attribute, and the two must agree. */
   aimee_memory_put_u32(response + 8, 0);
   aimee_memory_put_u32(response + 12, 0);
   assert(aimee_memory_scan_response_decode(response, 16, &retraction, &has_attr, attr,
                                            sizeof(attr)) == 0);
   assert(has_attr == 0 && attr[0] == '\0');
   /* A flag without an attribute, and an attribute without the flag, are both a
    * disagreement about what was found. */
   aimee_memory_put_u32(response + 8, 1);
   assert(aimee_memory_scan_response_decode(response, 16, &retraction, &has_attr, attr,
                                            sizeof(attr)) == -1);
   aimee_memory_put_u32(response + 8, 0);
   aimee_memory_put_u32(response + 12, 5);
   assert(aimee_memory_scan_response_decode(response, 21, &retraction, &has_attr, attr,
                                            sizeof(attr)) == -1);

   /* A truthy-looking 2 is a broken module, not a yes: this deletes. */
   aimee_memory_put_u32(response + 4, 2);
   aimee_memory_put_u32(response + 8, 1);
   assert(aimee_memory_scan_response_decode(response, 21, &retraction, &has_attr, attr,
                                            sizeof(attr)) == -1);
   aimee_memory_put_u32(response + 4, 1);

   /* An attribute that does not fit is refused, never shortened: a shortened
    * attribute normalizes to a different relation and would retract that one. */
   aimee_memory_put_u32(response + 12, AIMEE_MEMORY_SCAN_ATTR_MAX);
   assert(aimee_memory_scan_response_decode(response, 16u + AIMEE_MEMORY_SCAN_ATTR_MAX, &retraction,
                                            &has_attr, attr, sizeof(attr)) == -1);
   aimee_memory_put_u32(response + 12, AIMEE_MEMORY_SCAN_ATTR_MAX - 1u);
   memset(response + 16, 'a', AIMEE_MEMORY_SCAN_ATTR_MAX - 1u);
   assert(aimee_memory_scan_response_decode(response, 16u + AIMEE_MEMORY_SCAN_ATTR_MAX - 1u,
                                            &retraction, &has_attr, attr, sizeof(attr)) == 0);
   assert(strlen(attr) == AIMEE_MEMORY_SCAN_ATTR_MAX - 1u);

   /* Length disagreeing with the message, and a bad magic. */
   assert(aimee_memory_scan_response_decode(response, 20, &retraction, &has_attr, attr,
                                            sizeof(attr)) == -1);
   aimee_memory_put_u32(response, AIMEE_MEMORY_SCAN_RESPONSE_MAGIC + 1);
   assert(aimee_memory_scan_response_decode(response, 16u + AIMEE_MEMORY_SCAN_ATTR_MAX - 1u,
                                            &retraction, &has_attr, attr, sizeof(attr)) == -1);
   printf("  PASS: test_scan_turn_wire_layout\n");
}

int main(void)
{
   test_classify();
   test_retraction();
   test_extract();
   test_possessive_attr();
   test_registered_extractor_decides();
   test_extractor_failure_is_not_no_facts();
   test_extract_wire_layout();
   test_registered_turn_scanner_decides();
   test_scan_turn_wire_layout();
   printf("extract_patterns: all tests passed\n");
   return 0;
}
