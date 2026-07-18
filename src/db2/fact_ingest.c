/* fact_ingest.c: pattern-first typed-fact ingest pipeline (§6 -> §1) + the
 * per-turn ingress orchestration (§4/§6/§7). P5. See fact_ingest.h. */
#include "fact_ingest.h"
#include "fact_lifecycle.h"          /* db2_fact_retract */
#include "fact_recall.h"             /* db2_fact_recall_in_query */
#include "rel_types_store.h"         /* db2_fact_commit */
#include "../headers/aimee.h"        /* config_t */
#include "../headers/config.h"       /* config_load */
#include "memory_extract_patterns.h" /* memory_extract_patterns, retraction */
#include "memory_pii_gate.h"         /* memory_pii_turn_requests_sensitive */
#include "../headers/log.h"          /* LOG_WARN */

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
      /* Count the triples the gate let through when enabled: ACCEPT writes/bumps a
       * validated edge, NOVEL stages a provisional rel_type + a Class-C edge. A
       * re-ingest of a known triple still counts (it bumps weight, no new row).
       * REJECT_KIND / BADARG write nothing. */
      if (enabled && (v == FACT_GATE_ACCEPT || v == FACT_GATE_NOVEL))
         written++;
   }
   return written;
}

int db2_typed_fact_ingress(const char *query, char *facts_out, size_t facts_cap)
{
   if (facts_out && facts_cap)
      facts_out[0] = '\0';
   if (!query || !query[0])
      return 0;

   config_t cfg;
   config_load(&cfg);
   if (!cfg.typed_facts_enabled)
      return 0;

   int requests_sensitive = memory_pii_turn_requests_sensitive(query);

   /* §4: a retraction turn corrects rather than asserts — retract the named
    * attribute about the user (a user retraction always wins; an imprecise attr
    * safely no-ops). This stays synchronous: it is a cheap Postgres write, no LLM.
    * Fact EXTRACTION is offline-only (the memory_facts drain runs pattern + LLM),
    * so we do NOT run db2_fact_ingest_text() on the turn hot path. */
   if (memory_pattern_is_retraction(query))
   {
      char attr[128];
      if (memory_pattern_possessive_attr(query, attr, sizeof(attr)))
         (void)db2_fact_retract("user", attr, NULL, FACT_AUTHORITY_USER);
   }

   /* §7 read: the user's facts + facts about any entity named in the turn,
    * PII-gated, into the envelope. */
   if (!facts_out || !facts_cap)
      return 0;
   int fr = db2_fact_recall_in_query(query, requests_sensitive, facts_out, facts_cap);
   if (fr < 0) /* recall affects prompt content, so a persistent failure is worth surfacing */
      LOG_WARN("memory", "typed-fact recall failed (db2 unavailable?)");
   return fr < 0 ? 0 : fr;
}
