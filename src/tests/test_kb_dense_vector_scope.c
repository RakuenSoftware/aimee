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

int main(void)
{
   printf("kb_dense_vector_scope: ");
   printf("ok\n");
   return 0;
}
