/* test_memory_assemble_util.c: unit tests for the pure string helpers
 * extracted from memory_assemble.c (xml_escape_text, context_xml_tag_for_header).
 * Header is static inline → this test links nothing extra. */
#include "modules/memory/memory_assemble_util.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_xml_escape(void)
{
   char out[256];

   /* each special char → entity */
   xml_escape_text("a&b<c>d\"e", out, sizeof(out));
   assert(strcmp(out, "a&amp;b&lt;c&gt;d&quot;e") == 0);

   /* no specials → passthrough */
   xml_escape_text("plain text 123", out, sizeof(out));
   assert(strcmp(out, "plain text 123") == 0);

   /* leading/standalone specials */
   xml_escape_text("<>&\"", out, sizeof(out));
   assert(strcmp(out, "&lt;&gt;&amp;&quot;") == 0);

   /* NULL / empty src → empty dst */
   xml_escape_text(NULL, out, sizeof(out));
   assert(out[0] == '\0');
   xml_escape_text("", out, sizeof(out));
   assert(out[0] == '\0');

   /* zero-length / NULL dst tolerated (no crash) */
   xml_escape_text("x", out, 0);
   xml_escape_text("x", NULL, sizeof(out));

   /* plain truncation: 4-char buffer holds 3 chars + NUL */
   char small[4];
   xml_escape_text("abcdef", small, sizeof(small));
   assert(strcmp(small, "abc") == 0);

   /* entity that would not fit is dropped whole (no partial "&am") */
   char tiny[4];
   xml_escape_text("&", tiny, sizeof(tiny)); /* "&amp;" is 5 > 3 free */
   assert(tiny[0] == '\0');

   /* entity exactly fits: 6-byte buffer fits "&amp;" (5) + NUL */
   char fit[6];
   xml_escape_text("&", fit, sizeof(fit));
   assert(strcmp(fit, "&amp;") == 0);

   printf("  xml_escape_text: ok\n");
}

static void test_xml_tag_for_header(void)
{
   assert(strcmp(context_xml_tag_for_header("Key Facts"), "historical_fact") == 0);
   assert(strcmp(context_xml_tag_for_header("Mental Models"), "mental_model") == 0);
   assert(strcmp(context_xml_tag_for_header("Constraints"), "constraint") == 0);
   assert(strcmp(context_xml_tag_for_header("Procedures"), "procedure_memory") == 0);
   assert(strcmp(context_xml_tag_for_header("Active Tasks"), "active_task") == 0);
   assert(strcmp(context_xml_tag_for_header("Recent Context"), "recent_event") == 0);
   /* unknown + NULL → generic fallback */
   assert(strcmp(context_xml_tag_for_header("Nope"), "memory_item") == 0);
   assert(strcmp(context_xml_tag_for_header(""), "memory_item") == 0);
   assert(strcmp(context_xml_tag_for_header(NULL), "memory_item") == 0);
   printf("  context_xml_tag_for_header: ok\n");
}

int main(void)
{
   test_xml_escape();
   test_xml_tag_for_header();
   printf("memory_assemble_util: all tests passed\n");
   return 0;
}
