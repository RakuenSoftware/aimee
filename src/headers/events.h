#ifndef DEC_EVENTS_H
#define DEC_EVENTS_H 1

#include "aimee.h"
#include "delivery_target.h"

typedef enum
{
   AIMEE_EVENT_DELEGATE_COMPLETE = 0,
   AIMEE_EVENT_DELEGATE_FAILED,
   AIMEE_EVENT_VERIFY_PASS,
   AIMEE_EVENT_VERIFY_FAIL,
   AIMEE_EVENT_JOB_COMPLETE,
   AIMEE_EVENT_SESSION_IDLE,
   AIMEE_EVENT_COUNT
} aimee_event_t;

#define NOTIFY_MAX_TARGETS 8

typedef struct
{
   int is_webhook;           /* 0=shell command, 1=HTTP webhook */
   int is_delivery;          /* 1=typed delivery_target_t route */
   char command[512];        /* shell command; {{message}} and {{event}} are substituted */
   char url[512];            /* webhook URL (POST with JSON body) */
   char base_url[512];       /* typed delivery base URL (ntfy base, etc.) */
   delivery_target_t target; /* typed outbound target */
   unsigned int event_mask;  /* bitmask of events to match; 0 = all events */
} notify_target_t;

/* verbosity levels */
#define NOTIFY_VERBOSITY_MINIMAL  0 /* job_complete, delegate_failed, verify_fail, session_idle */
#define NOTIFY_VERBOSITY_STANDARD 1 /* + delegate_complete, verify_pass */
#define NOTIFY_VERBOSITY_VERBOSE  2 /* all events */

typedef struct
{
   int enabled;
   int verbosity;
   notify_target_t targets[NOTIFY_MAX_TARGETS];
   int target_count;
} notify_config_t;

/* Build an ntfy delivery command for tests and platform delivery. */
int notify_build_ntfy_command(const notify_target_t *target, const char *event_name,
                              const char *message, char *cmd, size_t cmd_len);

/* Deliver one typed target. Supports local and ntfy delivery targets.
 * Returns 0 when delivery was accepted, -1 when the target is unsupported or invalid. */
int notify_deliver_target(const notify_target_t *target, const char *event_name,
                          const char *message);

#endif /* DEC_EVENTS_H */
