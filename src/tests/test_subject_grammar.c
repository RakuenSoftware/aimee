/* test_subject_grammar.c — every C copy of the subject grammar, against one corpus.
 *
 * The grammar is encoded FOUR times and the copies cannot share an
 * implementation (see tests/subject_corpus.h). This test covers the three C ones;
 * the Postgres regex is covered by the same corpus through
 * scripts/gen-subject-corpus-sql.py.
 *
 * It asserts two separate things, and the second is the one that matters:
 *   - each copy matches the corpus, and
 *   - the copies AGREE WITH EACH OTHER, reported as a disagreement rather than
 *     two independent failures, because "these two disagree" is the actual defect
 *     and it is what a reader needs to see first.
 */
#include "subject_corpus.h"

#include "modules/db2/c/management_intent_fields.h"
#include "kb_mgmt_token_authority.h"
#include "server_identity_token.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* db2_intent_canonical_actor validates a canonical STORED record: NUL-terminated
 * in a fixed buffer with an all-zero tail. Build that shape so the comparison is
 * against the same input the server predicate sees. */
static int db2_says(const char *subject)
{
   char rec[577];
   if (strlen(subject) >= sizeof(rec))
      return 0;
   memset(rec, 0, sizeof(rec));
   memcpy(rec, subject, strlen(subject));
   return db2_intent_canonical_actor(rec, sizeof(rec)) ? 1 : 0;
}

static int server_says(const char *subject)
{
   return server_identity_subject_valid(subject) ? 1 : 0;
}

/* The authority's own copy. It reached the corpus late and for the worst reason:
 * it rejected a bare name the database had already admitted, so the mint refused
 * with INTEGRITY after every one of its eleven gates had passed — a failure that
 * pointed at the signing path rather than at a grammar. */
static int authority_says(const char *subject)
{
   char rec[577];
   if (strlen(subject) >= sizeof(rec))
      return 0;
   memset(rec, 0, sizeof(rec));
   memcpy(rec, subject, strlen(subject));
   return kb_identity_token_authority_subject_valid(rec) ? 1 : 0;
}

int main(void)
{
   int failures = 0;

   for (size_t i = 0; i < SUBJECT_CORPUS_N; ++i)
   {
      const subject_case_t *c = &SUBJECT_CORPUS[i];
      int db2 = db2_says(c->subject);
      int srv = server_says(c->subject);
      int aut = authority_says(c->subject);

      /* Disagreement first: it is the defect the four-copy design risks, and it is
       * invisible in production because the stricter copy just wins. */
      if (db2 != srv || db2 != aut)
      {
         printf("  DISAGREE [%s] (%s): db2=%d server=%d authority=%d\n", c->subject, c->why, db2,
                srv, aut);
         failures++;
         continue;
      }
      if (db2 != c->accept)
      {
         printf("  WRONG    [%s] (%s): both say %d, corpus says %d\n", c->subject, c->why, db2,
                c->accept);
         failures++;
      }
   }

   if (failures)
   {
      printf("test_subject_grammar: %d case(s) failed across %zu\n", failures, SUBJECT_CORPUS_N);
      return 1;
   }

   /* The corpus has to actually exercise both answers, or a validator that
    * returned a constant would pass it. */
   size_t accepts = 0;
   for (size_t i = 0; i < SUBJECT_CORPUS_N; ++i)
      accepts += (size_t)SUBJECT_CORPUS[i].accept;
   assert(accepts >= 8 && accepts <= SUBJECT_CORPUS_N - 8);

   printf("test_subject_grammar: ok (%zu cases, %zu accept / %zu reject, 3 implementations)\n",
          SUBJECT_CORPUS_N, accepts, SUBJECT_CORPUS_N - accepts);
   return 0;
}
