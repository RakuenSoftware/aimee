/* Which files get a DENSE vector from the doc ingest.
 *
 * Source files were embedded twice: kb_build treated every indexable file as a
 * prose document and embedded its chunks, while kb_code_embed_refresh embedded
 * the same files again as code. Measured on the am_ corpus that prose pass over
 * source was 82% of the doc-embedding token budget --
 *
 *     non-prose  5333 chunks / 749 files / 1,985,386 tokens
 *     prose      1862 chunks / 137 files /   445,519 tokens
 *
 * -- i.e. ~76 minutes against ~14 at the host's measured 532 tok/s, for a second
 * prose-shaped vector of bytes that semantic CODE search already covers.
 *
 * Chunk rows are still written for every file, so this is a change to what gets
 * a vector, NOT to what is searchable lexically. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int kb_path_wants_dense_vector_for_test(const char *rel_path);

static void test_prose_is_embedded(void)
{
   assert(kb_path_wants_dense_vector_for_test("docs/architecture.md"));
   assert(kb_path_wants_dense_vector_for_test("README.MD")); /* case-insensitive */
   assert(kb_path_wants_dense_vector_for_test("notes.txt"));
   assert(kb_path_wants_dense_vector_for_test("guide.rst"));
   assert(kb_path_wants_dense_vector_for_test("book.adoc"));
}

/* The 82%: these are what made a pass five times longer than it needed to be. */
static void test_source_is_not_embedded(void)
{
   assert(!kb_path_wants_dense_vector_for_test("src/db1/session_paths.c"));
   assert(!kb_path_wants_dense_vector_for_test("src/headers/kb.h"));
   assert(!kb_path_wants_dense_vector_for_test("scripts/deploy.sh"));
   assert(!kb_path_wants_dense_vector_for_test("tools/gen.py"));
   assert(!kb_path_wants_dense_vector_for_test("server-go/main.go"));
}

/* README / LICENSE / CHANGELOG carry real prose and are few, so extensionless
 * files stay embedded. */
static void test_extensionless_files_stay_embedded(void)
{
   assert(kb_path_wants_dense_vector_for_test("README"));
   assert(kb_path_wants_dense_vector_for_test("LICENSE"));
   assert(kb_path_wants_dense_vector_for_test("docs/CHANGELOG"));
}

/* The escape hatch must restore the old behaviour exactly. */
static void test_env_override_restores_embedding_everything(void)
{
   setenv("AIMEE_KB_EMBED_ALL_FILES", "1", 1);
   assert(kb_path_wants_dense_vector_for_test("src/db1/session_paths.c"));
   assert(kb_path_wants_dense_vector_for_test("tools/gen.py"));
   unsetenv("AIMEE_KB_EMBED_ALL_FILES");
   assert(!kb_path_wants_dense_vector_for_test("src/db1/session_paths.c"));
}

static void test_null_and_empty_are_safe(void)
{
   assert(kb_path_wants_dense_vector_for_test(NULL));
   assert(kb_path_wants_dense_vector_for_test(""));
}

int main(void)
{
   printf("kb_dense_vector_scope: ");
   test_prose_is_embedded();
   test_source_is_not_embedded();
   test_extensionless_files_stay_embedded();
   test_env_override_restores_embedding_everything();
   test_null_and_empty_are_safe();
   printf("ok\n");
   return 0;
}
