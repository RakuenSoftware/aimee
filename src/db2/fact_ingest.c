/* fact_ingest.c: pattern-first typed-fact ingest pipeline (§6 -> §1). P5.
 * See fact_ingest.h. */
#include "fact_ingest.h"
#include "rel_types_store.h"                    /* db2_fact_commit */
#include "../headers/memory_extract_patterns.h" /* memory_extract_patterns */

#define FI_MAX_TRIPLES 16

int db2_fact_ingest_text(const char *text, fact_authority_t authority, int enabled)
{
   if (!text)
      return -1;

   pattern_triple_t triples[FI_MAX_TRIPLES];
   int nt = memory_extract_patterns(text, triples, FI_MAX_TRIPLES);
   if (nt <= 0)
      return nt < 0 ? -1 : 0;

   int written = 0;
   for (int i = 0; i < nt; i++)
   {
      const pattern_triple_t *t = &triples[i];
      fact_gate_verdict_t v = db2_fact_commit(t->subject, t->subject_kind, t->rel_type, t->object,
                                              t->object_kind, authority, enabled);
      /* Count only what the gate actually persists when enabled: ACCEPT writes a
       * validated edge, NOVEL stages a provisional rel_type + a Class-C edge.
       * REJECT_KIND / BADARG write nothing. */
      if (enabled && (v == FACT_GATE_ACCEPT || v == FACT_GATE_NOVEL))
         written++;
   }
   return written;
}
