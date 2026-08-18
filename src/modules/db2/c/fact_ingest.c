/* fact_ingest.c: pattern-first typed-fact ingest pipeline (§6 -> §1) + the
 * per-turn ingress orchestration (§4/§6/§7). P5. See fact_ingest.h. */
#include "fact_ingest.h"
#include "fact_lifecycle.h"   /* db2_fact_retract */
#include "fact_recall.h"      /* db2_fact_recall_in_query */
#include "rel_types_store.h"  /* db2_fact_commit */
#include "../headers/aimee.h" /* config_t */
#include "../support/db2_runtime_config.h"
#include "modules/memory/memory_pii_gate.h" /* memory_pii_turn_requests_sensitive */
#include "../support/db2_log.h"             /* LOG_WARN */

#define FI_MAX_TRIPLES 16

static db2_fact_extract_fn g_fact_extract_provider;
static db2_fact_scan_fn g_fact_scan_provider;

void aimee_db2_register_fact_extract_provider(db2_fact_extract_fn provider)
{
   g_fact_extract_provider = provider;
}

void aimee_db2_register_fact_scan_provider(db2_fact_scan_fn provider)
{
   g_fact_scan_provider = provider;
}

static int fact_kind_valid(int kind)
{
   return (kind >= NODE_FILE && kind <= NODE_SCALAR) || kind == NODE_OTHER;
}

static int fact_field_terminated(const char *field, size_t capacity)
{
   for (size_t i = 0; i < capacity; ++i)
      if (field[i] == '\0')
         return 1;
   return 0;
}

static int fact_candidate_valid(const db2_fact_candidate_t *candidate)
{
   return candidate && candidate->subject[0] && candidate->rel_type[0] && candidate->object[0] &&
          fact_field_terminated(candidate->subject, sizeof(candidate->subject)) &&
          fact_field_terminated(candidate->rel_type, sizeof(candidate->rel_type)) &&
          fact_field_terminated(candidate->object, sizeof(candidate->object)) &&
          fact_kind_valid(candidate->subject_kind) && fact_kind_valid(candidate->object_kind);
}

int db2_fact_ingest_text(const char *text, fact_authority_t authority, int enabled)
{
   if (!text)
      return -1;

   db2_fact_candidate_t triples[FI_MAX_TRIPLES] = {0};
   int nt = 0;
   if (!g_fact_extract_provider ||
       g_fact_extract_provider(text, triples, FI_MAX_TRIPLES, &nt) != 0 || nt < 0 ||
       nt > FI_MAX_TRIPLES)
      return -1;
   for (int i = 0; i < nt; ++i)
      if (!fact_candidate_valid(&triples[i]))
         return -1;

   int written = 0;
   for (int i = 0; i < nt; i++)
   {
      const db2_fact_candidate_t *t = &triples[i];
      fact_gate_verdict_t v =
          db2_fact_commit(t->subject, (memory_node_kind_t)t->subject_kind, t->rel_type, t->object,
                          (memory_node_kind_t)t->object_kind, authority, enabled);
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

   if (!config_typed_facts_enabled())
      return 0;

   int requests_sensitive = memory_pii_turn_requests_sensitive(query);

   /* §4: a retraction turn corrects rather than asserts — retract the named
    * attribute about the user (a user retraction always wins; an imprecise attr
    * safely no-ops). This stays synchronous: it is a cheap Postgres write, no LLM.
    * Fact EXTRACTION is offline-only (the memory_facts drain runs pattern + LLM),
    * so we do NOT run db2_fact_ingest_text() on the turn hot path. */
   int is_retraction = 0;
   int has_attr = 0;
   char attr[DB2_FACT_ATTR_MAX] = "";
   if (!g_fact_scan_provider || g_fact_scan_provider(query, &is_retraction, &has_attr, attr) != 0 ||
       is_retraction < 0 || is_retraction > 1 || has_attr < 0 || has_attr > 1 ||
       !fact_field_terminated(attr, sizeof(attr)) || ((attr[0] != '\0') != (has_attr == 1)))
      /* No answer from the scanner. Do NOT retract: this path deletes, and
       * leaving a fact the user asked to forget is recoverable (they can ask
       * again) where deleting one they did not name is not. */
      LOG_WARN("memory", "retraction scan gave no answer; not retracting this turn");
   else if (is_retraction && has_attr)
      (void)db2_fact_retract("user", attr, NULL, FACT_AUTHORITY_USER);

   /* §7 read: the user's facts + facts about any entity named in the turn,
    * PII-gated, into the envelope. */
   if (!facts_out || !facts_cap)
      return 0;
   int fr = db2_fact_recall_in_query(query, requests_sensitive, facts_out, facts_cap);
   if (fr < 0) /* recall affects prompt content, so a persistent failure is worth surfacing */
      LOG_WARN("memory", "typed-fact recall failed (db2 unavailable?)");
   return fr < 0 ? 0 : fr;
}
