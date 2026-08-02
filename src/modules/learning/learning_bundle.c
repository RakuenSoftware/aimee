/* learning_bundle.c: cross-source evidence neighbourhood builder.
 *
 * Embed the query, scan stored evidence vectors, rank by cosine similarity,
 * and return the top-K neighbours (which naturally span multiple evidence
 * kinds). DB2 only; no DB1 access from this file. */

#include "learning_bundle.h"

#include "aimee.h"
#include "db2/evidence_vectors.h"
#include "memory.h" /* memory_embed_text */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Buffer cap for query + stored evidence embeddings. The deployment's single
 * embedder sets the actual dimension (1024 pplx-0.6b / 2560 pplx-4b; 384 for
 * the legacy builtin); the cosine loop below already skips rows whose parsed
 * dimension differs from the query, so we just size for the largest. */
#define BUNDLE_EMBED_DIM EMBED_MAX_DIM
/* Upper bound on vectors pulled into the in-memory ranker per build. Plenty for
 * the labeled replay corpus; the pgvector-native path handles larger scales. */
#define BUNDLE_SCAN_MAX 512

/* Parse a pgvector/JSON text literal "[f0,f1,...]" into out[0..maxdim).
 * Tolerates surrounding brackets, spaces, and comma separators. Returns the
 * number of floats parsed. */
static int bundle_parse_vec(const char *text, float *out, int maxdim)
{
   if (!text)
      return 0;
   int n = 0;
   const char *p = text;
   while (*p && n < maxdim)
   {
      while (*p == '[' || *p == ']' || *p == ',' || *p == ' ')
         p++;
      if (!*p)
         break;
      char *end;
      float v = strtof(p, &end);
      if (end == p)
         break;
      out[n++] = v;
      p = end;
   }
   return n;
}

static double bundle_cosine(const float *a, const float *b, int dim)
{
   double dot = 0.0, na = 0.0, nb = 0.0;
   for (int i = 0; i < dim; i++)
   {
      dot += (double)a[i] * (double)b[i];
      na += (double)a[i] * (double)a[i];
      nb += (double)b[i] * (double)b[i];
   }
   double denom = sqrt(na) * sqrt(nb);
   return denom > 1e-9 ? dot / denom : 0.0;
}

typedef struct
{
   int row;
   double score;
} bundle_scored_t;

static int bundle_cmp_desc(const void *a, const void *b)
{
   double sa = ((const bundle_scored_t *)a)->score;
   double sb = ((const bundle_scored_t *)b)->score;
   if (sa < sb)
      return 1;
   if (sa > sb)
      return -1;
   return 0;
}

int learning_bundle_build(const char *query, const char *embed_cmd, int k, learning_bundle_t *out)
{
   if (!out || !query || !query[0])
      return -1;
   memset(out, 0, sizeof(*out));

   if (k < 1)
      k = 1;
   if (k > LEARNING_BUNDLE_MAX)
      k = LEARNING_BUNDLE_MAX;

   const char *model = config_embedder_command_current(embed_cmd);
   float qvec[BUNDLE_EMBED_DIM];
   int qdim = memory_embed_text(query, model, EMBED_INPUT_QUERY, qvec, BUNDLE_EMBED_DIM);
   if (qdim <= 0)
      return -1;

   db2_evidence_vector_row_t *rows = calloc(BUNDLE_SCAN_MAX, sizeof(*rows));
   if (!rows)
      return -1;

   int got = db2_evidence_vectors_list(rows, BUNDLE_SCAN_MAX);
   if (got < 0)
   {
      free(rows);
      return -1;
   }
   out->scanned = got;
   if (got == 0)
   {
      free(rows);
      return 0; /* empty corpus is a valid, empty bundle */
   }

   bundle_scored_t *scored = calloc((size_t)got, sizeof(*scored));
   if (!scored)
   {
      free(rows);
      return -1;
   }

   int nscored = 0;
   float vec[BUNDLE_EMBED_DIM];
   for (int i = 0; i < got; i++)
   {
      int dim = bundle_parse_vec(rows[i].embedding, vec, BUNDLE_EMBED_DIM);
      if (dim != qdim)
         continue; /* dimension mismatch — skip rather than mis-score */
      scored[nscored].row = i;
      scored[nscored].score = bundle_cosine(qvec, vec, qdim);
      nscored++;
   }

   qsort(scored, (size_t)nscored, sizeof(*scored), bundle_cmp_desc);

   int take = nscored < k ? nscored : k;
   for (int i = 0; i < take; i++)
   {
      db2_evidence_vector_row_t *r = &rows[scored[i].row];
      learning_bundle_item_t *it = &out->items[i];
      snprintf(it->artifact_id, sizeof(it->artifact_id), "%s", r->artifact_id);
      snprintf(it->kind, sizeof(it->kind), "%s", r->kind);
      snprintf(it->collection, sizeof(it->collection), "%s", r->collection);
      it->score = scored[i].score;
   }
   out->count = take;

   /* Count distinct kinds among the returned items. */
   for (int i = 0; i < take; i++)
   {
      int seen = 0;
      for (int j = 0; j < i; j++)
      {
         if (strcmp(out->items[i].kind, out->items[j].kind) == 0)
         {
            seen = 1;
            break;
         }
      }
      if (!seen)
         out->distinct_kinds++;
   }

   free(scored);
   free(rows);
   return 0;
}
