/* db1/clarify.h: planning-preparation clarification subsystem.
 *
 * Pure domain API. No backend types or handles in any signature.
 *
 * Vague tasks enter a structured Q&A flow: ambiguity dimensions are
 * scored, targeted questions are generated for the weakest dimension,
 * and answers are recorded until the clarity score crosses
 * CLARIFY_READY_SCORE, at which point a crystallized spec is generated
 * and the session is marked ready. */
#ifndef DEC_DB1_CLARIFY_H
#define DEC_DB1_CLARIFY_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Ambiguity dimensions scored per session */
#define CLARIFY_DIM_SCOPE       0 /* what is in/out of scope */
#define CLARIFY_DIM_SUCCESS     1 /* success criteria / definition of done */
#define CLARIFY_DIM_CONSTRAINTS 2 /* hard requirements or constraints */
#define CLARIFY_DIM_APPROACH    3 /* preferred approach or technology */
#define CLARIFY_DIM_CONTEXT     4 /* project context / existing system */
#define CLARIFY_NUM_DIMS        5

#define CLARIFY_MAX_QA       8     /* max Q&A rounds per session */
#define CLARIFY_READY_SCORE  0.70f /* clarity threshold to crystallize */
#define CLARIFY_DESC_LEN     512
#define CLARIFY_TEXT_LEN     512
#define CLARIFY_SPEC_LEN     4096
#define CLARIFY_DIM_NAME_LEN 32

   typedef enum
   {
      CLARIFY_OPEN = 0, /* awaiting more answers */
      CLARIFY_READY,    /* score >= threshold, spec generated */
      CLARIFY_CANCELLED
   } clarify_status_t;

   typedef struct
   {
      char dimension[CLARIFY_DIM_NAME_LEN];
      char question[CLARIFY_TEXT_LEN];
      char answer[CLARIFY_TEXT_LEN];
      int answered;
      int seq;
   } clarify_qa_t;

   typedef struct
   {
      int id;
      char description[CLARIFY_DESC_LEN];
      clarify_status_t status;
      float score;
      clarify_qa_t qa[CLARIFY_MAX_QA];
      int qa_count;
      char spec[CLARIFY_SPEC_LEN];
      char created_at[32];
      char updated_at[32];
   } clarify_session_t;

   /* --- Core operations --- */

   /* Create a new clarification session for `description`. Fills `out` (if
    * non-NULL) with the created session. Returns the session id, or -1. */
   int db1_clarify_start(const char *description, clarify_session_t *out);

   /* Load a session by id. Returns 0 on success, -1 if not found. */
   int db1_clarify_get(int id, clarify_session_t *out);

   /* Record an answer for the current open question of session `id`.
    * If the resulting score reaches CLARIFY_READY_SCORE, crystallizes the
    * spec and marks the session ready. Fills `out` (if non-NULL) with the
    * updated session. Returns 0 on success, -1 on failure. */
   int db1_clarify_answer(int id, const char *answer, clarify_session_t *out);

   /* --- Scoring and question generation --- */

   float db1_clarify_score(const clarify_session_t *s);

   void db1_clarify_weakest_dim(const clarify_session_t *s, char *dim_out, size_t len);

   /* Returns 0 if a question was generated, 1 if already ready, -1 if max
    * rounds reached. */
   int db1_clarify_next_question(const clarify_session_t *s, char *q_out, size_t q_len,
                                 char *dim_out, size_t dim_len);

   /* Returns malloc'd string (caller frees). */
   char *db1_clarify_crystallize(const clarify_session_t *s);

   /* --- Serialisation --- */

   /* Returns malloc'd JSON string (caller frees). */
   char *db1_clarify_to_json(const clarify_session_t *s);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_CLARIFY_H */
