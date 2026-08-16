/* db2/feedback.h: feedback recording, DB2 subsystem.
 *
 * Records feedback as either a new rule or reinforcement of an existing one.
 * Pure domain API; SQL is encapsulated in db2/feedback.c. */
#ifndef DEC_DB2_FEEDBACK_H
#define DEC_DB2_FEEDBACK_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Parse polarity string (+, -, posi, negi, principle). Returns canonical form
    * or NULL on error. Result is a static string, do not free. */
   const char *db2_feedback_parse_polarity(const char *input);

   /* Record feedback. Creates or reinforces a rule.
    * weight_override: use -1 for default behavior.
    * Sets *reinforced to 1 if an existing rule was reinforced.
    * Returns the rule ID on success, -1 on error. */
   int db2_feedback_record(const char *polarity, const char *title, const char *description,
                           int weight_override, int *reinforced);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_FEEDBACK_H */
