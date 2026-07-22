/* kb_reasoning.h: Datalog-based graph reasoning over DB2 artifacts.
 * See
 * docs/proposals/accepted/graph-reasoning-case-based-recall-and-contradiction-logic.md
 */
#ifndef DEC_KB_REASONING_H
#define DEC_KB_REASONING_H 1

#include "config.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define KB_REASONING_MAX_BINDINGS 256
#define KB_REASONING_MAX_VARS     16
#define KB_REASONING_MAX_CITATION 512

   typedef struct
   {
      char var[64];
      char value[256];
   } kb_reasoning_var_t;

   typedef struct
   {
      kb_reasoning_var_t vars[KB_REASONING_MAX_VARS];
      int n_vars;
      char citations[KB_REASONING_MAX_CITATION];
   } kb_reasoning_binding_t;

   typedef struct
   {
      kb_reasoning_binding_t *rows;
      int n_rows;
      int facts_evaluated;
      int derived_facts;
      int elapsed_ms;
   } kb_reasoning_result_t;

   /* Run a Datalog query against the DB2 artifact graph.
    * query:          Datalog atom, e.g. "contradiction_ok(?a, ?b)"
    * bindings_json:  initial bindings as JSON object, or NULL
    * scope_kind / scope_id: narrow the fact snapshot; NULL = all scopes
    * result_out:     populated on success; caller frees with kb_reasoning_result_free
    * Returns 0 on success, -1 if disabled or error. */
   int kb_reasoning_query(const config_t *cfg, const char *query, const char *bindings_json,
                          const char *scope_kind, const char *scope_id,
                          kb_reasoning_result_t *result_out);

   void kb_reasoning_result_free(kb_reasoning_result_t *r);

   /* Write a case artifact (kind="case", state="proposed").
    * payload_json: full case payload (trigger_features, outcome, ...).
    * id_out:       receives UUID on success; may be NULL.
    * Returns 0 on success, -1 on error. */
   int kb_reasoning_case_write(const char *payload_json, char *id_out, int id_out_len);

   /* Structural contradiction check for a proposed contradicts link.
    * Returns 1 if the Datalog check passes, 0 if it fails, -1 if disabled/error. */
   int kb_reasoning_contradiction_check(const config_t *cfg, const char *artifact_a_id,
                                        const char *artifact_b_id);

   /* Seed the built-in ruleset-v1 record into DB2 reasoning_rulesets (idempotent). */
   void kb_reasoning_seed_ruleset(void);

   /* One recalled case result. */
   typedef struct
   {
      char artifact_id[37];
      char outcome[128];
      double score;
   } kb_reasoning_case_result_t;

   /* Composite case recall: kNN on exemplar_vectors + DB2 case query + rank by confidence.
    * trigger_json: JSON trigger features (may be NULL).
    * scope_kind / scope_id: narrow to project or workspace (NULL = all).
    * Returns number of results written into out, or -1 on error. */
   int kb_reasoning_case_recall(const config_t *cfg, const char *trigger_json,
                                const char *scope_kind, const char *scope_id,
                                kb_reasoning_case_result_t *out, int max_out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_REASONING_H */
