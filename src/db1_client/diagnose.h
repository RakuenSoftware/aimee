/* db1/diagnose.h: evidence-driven diagnosis.
 *
 * Pure domain API. No backend types in any signature.
 *
 * Separates observations, hypotheses, and evidence so investigations
 * can be ranked by the strength of their supporting evidence rather
 * than by gut feel. Evidence ranks (1=direct experiment, 4=speculation)
 * let a heuristic confidence score discriminate weakly-supported
 * guesses from well-probed hypotheses. */
#ifndef DEC_DB1_DIAGNOSE_H
#define DEC_DB1_DIAGNOSE_H 1

#ifdef __cplusplus
extern "C"
{
#endif

/* Evidence ranking: lower is stronger. */
#define DIAG_RANK_DIRECT      1
#define DIAG_RANK_LOG         2
#define DIAG_RANK_CODE        3
#define DIAG_RANK_SPECULATION 4

   typedef struct
   {
      int id;
      char symptom[512];
      char status[16]; /* active, concluded, abandoned */
      char conclusion[1024];
      double confidence;
      char created_at[32];
      char updated_at[32];
   } diagnosis_t;

   typedef struct
   {
      int id;
      int diagnosis_id;
      char kind[32]; /* observation, hypothesis, evidence_for, evidence_against, probe */
      int parent_id;
      char content[1024];
      char source[256];
      int evidence_rank;
      char created_at[32];
   } diagnosis_item_t;

   typedef struct
   {
      diagnosis_item_t hypothesis;
      int evidence_for_count;
      int evidence_against_count;
      int strongest_for_rank;
      int strongest_against_rank;
      double confidence;
   } diagnosis_ranking_t;

   int db1_diagnose_start(const char *symptom);

   int db1_diagnose_add_observation(int diag_id, const char *content, const char *source);
   int db1_diagnose_add_hypothesis(int diag_id, const char *content);
   int db1_diagnose_add_evidence(int diag_id, int hypothesis_id, const char *kind,
                                 const char *content, const char *source, int rank);
   int db1_diagnose_add_probe(int diag_id, int hypothesis_id, const char *content);

   int db1_diagnose_get(int diag_id, diagnosis_t *out);
   int db1_diagnose_list(diagnosis_t *out, int max);
   int db1_diagnose_list_items(int diag_id, diagnosis_item_t *out, int max);
   int db1_diagnose_list_hypotheses(int diag_id, diagnosis_item_t *out, int max);

   int db1_diagnose_rank_hypotheses(int diag_id, diagnosis_ranking_t *out, int max);

   int db1_diagnose_conclude(int diag_id, const char *conclusion, double confidence);
   int db1_diagnose_abandon(int diag_id);

   /* Returns malloc'd JSON (caller frees), NULL if not found. */
   char *db1_diagnose_json_full(int diag_id);
   /* Returns malloc'd JSON array (caller frees). */
   char *db1_diagnose_json_list(void);
   /* Returns malloc'd plain-text report (caller frees), NULL if not found. */
   char *db1_diagnose_render_status(int diag_id);

#define DIAG_MAX_SUGGEST          16
#define DIAG_PROBE_SUGGESTION_LEN 512
   typedef struct
   {
      int hypothesis_a_id;
      int hypothesis_b_id; /* 0 if single-hypothesis suggestion */
      char suggestion[DIAG_PROBE_SUGGESTION_LEN];
   } diagnosis_probe_suggestion_t;

#define DIAG_SUGGEST_BALANCE_THRESHOLD 0.15
   int db1_diagnose_suggest_probes(int diag_id, diagnosis_probe_suggestion_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_DIAGNOSE_H */
