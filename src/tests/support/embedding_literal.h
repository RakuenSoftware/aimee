/* embedding_literal.h: build a pgvector literal of the configured embedding width.
 *
 * The vector columns (memory_embeddings.embedding, evidence_vectors.embedding,
 * kb_embeddings.embedding, ...) are halfvec(N) on Postgres, so a hand-written
 * "[0.1,0.2]" is rejected outright -- "expected N dimensions, not 2". Under the
 * sqlite shim the same column is plain TEXT and any literal round-trips, which is
 * why short literals survived in tests for as long as they did.
 *
 * Values must also survive the column's element type. halfvec is IEEE binary16, so
 * 0.1 and 0.3 do NOT round-trip: they come back as 0.099976 and 0.300049, and any
 * test comparing the stored literal to the one it wrote fails on the real engine.
 * Pass a value with an exact binary16 representation -- any multiple of a negative
 * power of two, e.g. 0.5, 0.25, 0.125 -- and the round trip is byte-exact.
 *
 * Header-only on purpose: every caller is a test binary with its own link line, and
 * a shared object would mean editing each of them.
 */
#ifndef AIMEE_TEST_EMBEDDING_LITERAL_H
#define AIMEE_TEST_EMBEDDING_LITERAL_H

#include <assert.h>
#include <stdio.h>
#include <string.h>

int db2_embedding_dim(void);

/* Write "[v,v,...,v]" of db2_embedding_dim() elements into `buf`. Returns `buf`. */
static inline char *embedding_literal(char *buf, size_t len, double value)
{
   int dim = db2_embedding_dim();
   assert(dim > 0);
   size_t o = 0;
   int n = snprintf(buf + o, len - o, "[");
   assert(n > 0 && (size_t)n < len - o);
   o += (size_t)n;
   for (int i = 0; i < dim; i++)
   {
      n = snprintf(buf + o, len - o, "%s%g", i ? "," : "", value);
      assert(n > 0 && (size_t)n < len - o);
      o += (size_t)n;
   }
   n = snprintf(buf + o, len - o, "]");
   assert(n > 0 && (size_t)n < len - o);
   return buf;
}

/* Enough room for the widest shipping dim (2560) at "0.0078125," per element. */
#define EMBEDDING_LITERAL_MAX 32768

#endif /* AIMEE_TEST_EMBEDDING_LITERAL_H */
