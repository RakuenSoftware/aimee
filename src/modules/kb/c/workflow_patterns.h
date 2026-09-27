/* db2/workflow_patterns.h: durable store for promoted `workflow` learning
 * candidates (the cross-source-learning-pipeline promotion target surface).
 * DB2 subsystem — written by learning_promote. */
#ifndef DEC_DB2_WORKFLOW_PATTERNS_H
#define DEC_DB2_WORKFLOW_PATTERNS_H 1
#include <stdint.h>
#ifdef __cplusplus
extern "C"
{
#endif
   typedef struct
   {
      int64_t id;
      char pattern[512];
      char description[1024];
      char source[32];
      char source_ref[64];
      double confidence;
   } workflow_pattern_t;

   /* Insert a workflow pattern. out may be NULL. Returns 0 on success, -1 on
    * failure. */
   int db2_workflow_pattern_insert(const char *pattern, const char *description, const char *source,
                                   const char *source_ref, double confidence,
                                   workflow_pattern_t *out);
#ifdef __cplusplus
}
#endif
#endif