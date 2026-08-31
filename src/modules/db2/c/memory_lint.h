/* memory_lint.h: read-only checks for memory consistency issues. */
#ifndef DEC_MEMORY_LINT_H
#define DEC_MEMORY_LINT_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define MEMORY_LINT_TYPE_LEN    32
#define MEMORY_LINT_KEY_LEN     512
#define MEMORY_LINT_MESSAGE_LEN 256
#define MEMORY_LINT_MAX_ISSUES  256

   typedef struct
   {
      char type[MEMORY_LINT_TYPE_LEN]; /* orphan | concept_gap | stale_ref */
      int64_t memory_id;
      char key[MEMORY_LINT_KEY_LEN];
      char message[MEMORY_LINT_MESSAGE_LEN];
   } memory_lint_issue_t;

   /* Run all lint checks.  Returns the number of issues found (capped at max).
    * DB2 must be reachable; returns 0 on connection failure. */
   int memory_lint_run(memory_lint_issue_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_MEMORY_LINT_H */
