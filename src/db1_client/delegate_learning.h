/* Delegate exit observations; independent task grading belongs to Learning. */
#ifndef DELEGATE_LEARNING_H
#define DELEGATE_LEARNING_H

#include <stdint.h>

/* Failure modes a delegate can exit with */
typedef enum
{
   DL_MODE_SUCCESS,           /* process completed; task correctness is ungraded */
   DL_MODE_STALL_NO_WRITES,   /* write_enforce fired, 0 writes */
   DL_MODE_STALL_SLOW_WRITES, /* write_enforce fired but eventually wrote */
   DL_MODE_MAX_TURNS,         /* max-turn budget exhausted */
   DL_MODE_NO_PROGRESS,       /* successful retrieval loop made no write progress */
   DL_MODE_DRIFT_PREFLIGHT,   /* pre-flight drift detected */
   DL_MODE_DRIFT_BRANCH,      /* branch drift detected */
   DL_MODE_CANCELLED,         /* delegate was cancelled */
   DL_MODE_UNKNOWN,           /* an exit without a specific host observation */
} dl_failure_mode_t;

/* Metrics captured at delegate exit for classification */
typedef struct
{
   const char *session_id;
   const char *role;
   int turns;
   int tool_calls;
   int success;             /* 1 if rc==0, 0 otherwise */
   int had_writes;          /* -1 unknown, 0 observed none, 1 observed writes */
   int write_enforce_fired; /* 1 if write_enforce triggered */
   int max_turns_limit;     /* the max_turns cap that was set */
   const char *error;       /* error string if failure */
} dl_exit_metrics_t;

/* Classified result from classify_delegate_exit */
typedef struct
{
   dl_failure_mode_t failure_mode;
   double confidence;
   char lesson[512];    /* human-readable lesson */
   char evidence[4096]; /* compact JSON, including worst-case escaping of a 512-byte error */
} dl_classification_t;

/*
 * classify_delegate_exit — analyse exit metrics and produce a classification.
 * Returns a filled dl_classification_t. Caller owns the struct (stack or heap).
 */
void classify_delegate_exit(const dl_exit_metrics_t *metrics, dl_classification_t *out);

/*
 * dl_failure_mode_to_string — convert enum to string for storage/logging.
 */
const char *dl_failure_mode_to_string(dl_failure_mode_t mode);

/*
 * dl_string_to_failure_mode — convert string back to enum.
 */
dl_failure_mode_t dl_string_to_failure_mode(const char *s);

/* DB1 eviction (called internally by delegate_learning_record). */

#endif /* DELEGATE_LEARNING_H */
