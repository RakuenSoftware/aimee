#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_test_shim.h"
#include "notes.h"
#include <aimee/tools/agent_tools.h>
#include "cJSON.h"

static void setup(void)
{
   db2_test_shim_open();
}

static void teardown(void)
{
   db2_test_shim_close();
}

static void test_slug_basic(void)
{
   char slug[NOTE_MAX_SLUG];

   db2_note_title_to_slug("Auth Middleware Investigation", slug, sizeof(slug));
   assert(strcmp(slug, "auth-middleware-investigation") == 0);

   db2_note_title_to_slug("Hello World", slug, sizeof(slug));
   assert(strcmp(slug, "hello-world") == 0);

   db2_note_title_to_slug("  leading spaces  ", slug, sizeof(slug));
   assert(strcmp(slug, "leading-spaces") == 0);

   db2_note_title_to_slug("UPPERCASE", slug, sizeof(slug));
   assert(strcmp(slug, "uppercase") == 0);

   db2_note_title_to_slug("special!@#chars$%^test", slug, sizeof(slug));
   assert(strcmp(slug, "specialcharstest") == 0);

   db2_note_title_to_slug("multiple---dashes", slug, sizeof(slug));
   assert(strcmp(slug, "multiple-dashes") == 0);

   db2_note_title_to_slug("under_scores_too", slug, sizeof(slug));
   assert(strcmp(slug, "under-scores-too") == 0);
}

static void test_slug_edge_cases(void)
{
   char slug[NOTE_MAX_SLUG];

   db2_note_title_to_slug("", slug, sizeof(slug));
   assert(slug[0] == '\0');

   db2_note_title_to_slug("!@#$%", slug, sizeof(slug));
   assert(slug[0] == '\0');

   /* Truncation */
   char long_title[512];
   memset(long_title, 'a', sizeof(long_title) - 1);
   long_title[sizeof(long_title) - 1] = '\0';
   db2_note_title_to_slug(long_title, slug, sizeof(slug));
   assert(strlen(slug) < sizeof(slug));

   /* NULL handling */
   db2_note_title_to_slug(NULL, slug, sizeof(slug));
   /* Should not crash */
}

static void test_create_note(void)
{
   setup();
   note_t n;

   int rc = db2_note_create("Auth Investigation", "Found the root cause in middleware",
                            "debugging,auth", "delegate:review", &n);
   assert(rc == 0);
   assert(n.id > 0);
   assert(strcmp(n.title, "Auth Investigation") == 0);
   assert(strcmp(n.slug, "auth-investigation") == 0);
   assert(strstr(n.content, "Found the root cause") != NULL);
   assert(strcmp(n.tags, "debugging,auth") == 0);
   assert(strcmp(n.author, "delegate:review") == 0);

   teardown();
}

static void test_create_append(void)
{
   setup();
   note_t n;

   int rc = db2_note_create("Debug Log", "First finding", "debug", "agent", &n);
   assert(rc == 0);
   int64_t first_id = n.id;

   rc = db2_note_create("Debug Log", "Second finding", NULL, "agent", &n);
   assert(rc == 0);
   assert(n.id == first_id);
   assert(strstr(n.content, "First finding") != NULL);
   assert(strstr(n.content, "Second finding") != NULL);

   teardown();
}

static void test_list_notes(void)
{
   setup();
   note_t notes[10];

   db2_note_create("Note A", "Content A", "tag1", "agent", NULL);
   db2_note_create("Note B", "Content B", "tag2", "agent", NULL);
   db2_note_create("Note C", "Content C", "tag1,tag2", "agent", NULL);

   /* List all */
   int count = db2_note_list(NULL, 10, notes, 10);
   assert(count == 3);

   /* Filter by tag */
   count = db2_note_list("tag1", 10, notes, 10);
   assert(count == 2);

   count = db2_note_list("tag2", 10, notes, 10);
   assert(count == 2);

   count = db2_note_list("nonexistent", 10, notes, 10);
   assert(count == 0);

   /* Limit */
   count = db2_note_list(NULL, 2, notes, 10);
   assert(count == 2);

   teardown();
}

static void test_search_notes(void)
{
   setup();
   note_t notes[10];

   db2_note_create("Auth Bug", "The middleware timeout was 30s", "debug", "agent", NULL);
   db2_note_create("Performance", "Latency spike at 2pm", "perf", "agent", NULL);
   db2_note_create("Auth Config", "SPIRE SVID rotation", "auth", "agent", NULL);

   int count = db2_note_search("Auth", notes, 10);
   assert(count == 2);

   count = db2_note_search("middleware", notes, 10);
   assert(count == 1);

   count = db2_note_search("nonexistent", notes, 10);
   assert(count == 0);

   teardown();
}

static void test_get_note(void)
{
   setup();
   note_t n, fetched;

   db2_note_create("Test Note", "Some content", "tag", "agent", &n);

   int rc = db2_note_get(n.id, &fetched);
   assert(rc == 0);
   assert(fetched.id == n.id);
   assert(strcmp(fetched.title, "Test Note") == 0);

   rc = db2_note_get(99999, &fetched);
   assert(rc == -1);

   teardown();
}

static void test_delete_note(void)
{
   setup();
   note_t n;

   db2_note_create("To Delete", "Will be removed", NULL, NULL, &n);

   int rc = db2_note_delete(n.id);
   assert(rc == 0);

   rc = db2_note_get(n.id, &n);
   assert(rc == -1);

   teardown();
}

static void test_null_params(void)
{
   setup();

   /* NULL tags and author should work */
   note_t n;
   int rc = db2_note_create("Minimal", "Just content", NULL, NULL, &n);
   assert(rc == 0);
   assert(n.tags[0] == '\0');
   assert(n.author[0] == '\0');

   /* NULL title should fail */
   rc = db2_note_create(NULL, "content", NULL, NULL, &n);
   assert(rc == -1);

   /* NULL content should fail */
   rc = db2_note_create("title", NULL, NULL, NULL, &n);
   assert(rc == -1);

   teardown();
}

/* Verify that the delegate-facing tool definitions advertise the note tools.
 * This is the contract that proposal AC requires: "Note tools are available
 * in delegate sessions" — i.e. exposed via build_tools_array(). */
static int tools_array_has(cJSON *tools, const char *want_name)
{
   int n = cJSON_GetArraySize(tools);
   for (int i = 0; i < n; i++)
   {
      cJSON *t = cJSON_GetArrayItem(tools, i);
      cJSON *fn = cJSON_GetObjectItem(t, "function");
      cJSON *name = fn ? cJSON_GetObjectItem(fn, "name") : cJSON_GetObjectItem(t, "name");
      if (name && cJSON_IsString(name) && strcmp(name->valuestring, want_name) == 0)
         return 1;
   }
   return 0;
}

static void test_delegate_tools_advertised(void)
{
   cJSON *tools = build_tools_array();
   assert(tools_array_has(tools, "create_note"));
   assert(tools_array_has(tools, "list_notes"));
   assert(tools_array_has(tools, "search_notes"));
   cJSON_Delete(tools);

   cJSON *resp_tools = build_tools_array_responses();
   assert(tools_array_has(resp_tools, "create_note"));
   assert(tools_array_has(resp_tools, "list_notes"));
   assert(tools_array_has(resp_tools, "search_notes"));
   cJSON_Delete(resp_tools);

   cJSON *anthro_tools = build_tools_array_anthropic();
   assert(tools_array_has(anthro_tools, "create_note"));
   assert(tools_array_has(anthro_tools, "list_notes"));
   assert(tools_array_has(anthro_tools, "search_notes"));
   cJSON_Delete(anthro_tools);
}

int main(void)
{
   printf("notes: ");

   test_slug_basic();
   test_slug_edge_cases();
   test_create_note();
   test_create_append();
   test_list_notes();
   test_search_notes();
   test_get_note();
   test_delete_note();
   test_null_params();
   test_delegate_tools_advertised();

   printf("all tests passed\n");
   return 0;
}
