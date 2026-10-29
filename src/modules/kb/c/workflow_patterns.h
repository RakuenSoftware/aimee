/* kb_store/workflow_patterns.h: durable store for promoted `workflow` learning
 * candidates (the cross-source-learning-pipeline promotion target surface).
 * KB_STORE subsystem — written by learning_promote. */
#ifndef DEC_KB_STORE_WORKFLOW_PATTERNS_H
#define DEC_KB_STORE_WORKFLOW_PATTERNS_H 1
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

#ifdef __cplusplus
}
#endif
#endif