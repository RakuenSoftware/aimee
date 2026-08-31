/* Parity tests for descriptor-owned DB2 code-search line enrichment. */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int code_match_line(const char *content, const char *marked_snippet);
int db2_support_code_match_line(const char *content, const char *marked_snippet);

static void assert_parity(const char *content, const char *snippet)
{
   assert(code_match_line(content, snippet) == db2_support_code_match_line(content, snippet));
}

static void test_marker_and_match_parity(void)
{
   const char *content = "alpha\nbeta token\ngamma token\ndelta\n";
   const char *const snippets[] = {NULL,
                                   "",
                                   "plain token",
                                   ">>token<<<",
                                   ">>>token<<",
                                   ">>><<<",
                                   ">>>token<<<",
                                   "prefix >>>token<<< suffix",
                                   ">>>missing<<<",
                                   ">>>beta token<<<",
                                   ">>>>>token<<<<<<",
                                   ">>>token<<< >>>gamma<<<"};
   assert_parity(NULL, ">>>token<<<");
   assert_parity("", ">>>token<<<");
   for (size_t i = 0; i < sizeof(snippets) / sizeof(snippets[0]); i++)
      assert_parity(content, snippets[i]);
}

static void test_every_byte(void)
{
   char content[] = {'a', '\n', 'x', '\n', 'b', '\0'};
   char snippet[] = {'>', '>', '>', 'x', '<', '<', '<', '\0'};
   for (int byte = 1; byte <= 255; byte++)
   {
      content[2] = (char)byte;
      snippet[3] = (char)byte;
      assert_parity(content, snippet);
   }
}

static void test_length_and_line_boundaries(void)
{
   char content[4096];
   char snippet[2100];
   size_t offset = 0;
   for (int line = 1; line <= 256; line++)
   {
      int written = snprintf(content + offset, sizeof(content) - offset, "line-%03d\n", line);
      assert(written > 0);
      offset += (size_t)written;
   }
   assert_parity(content, ">>>line-001<<<");
   assert_parity(content, ">>>line-128<<<");
   assert_parity(content, ">>>line-256<<<");
   /* The authoritative helper performs a bounded prefix comparison, not a
    * token-boundary check. Preserve that behavior during the process move. */
   assert_parity("token\n", ">>>tok<<<");

   memset(content, 'q', 2048);
   content[2048] = '\0';
   for (size_t length = 1; length <= 2048; length++)
   {
      memcpy(snippet, ">>>", 3);
      memset(snippet + 3, 'q', length);
      memcpy(snippet + 3 + length, "<<<", 4);
      assert_parity(content, snippet);
   }
}

int main(void)
{
   test_marker_and_match_parity();
   test_every_byte();
   test_length_and_line_boundaries();
   return 0;
}
