/* session_degraded_notice.h: tell the agent when aimee is not fully functional.
 *
 * Split out of cli_session_start.c so the trigger conditions and the wording can
 * be tested without standing up a server: the message is the only thing that
 * stops an agent reading an empty search result as an authoritative "not found".
 */
#ifndef SESSION_DEGRADED_NOTICE_H
#define SESSION_DEGRADED_NOTICE_H

#include <stddef.h>

/* Render the notice into out[cap], or return 0 when nothing is wrong. `kb` and
 * `retrieval` use /v1/ready's dependency encoding ("ok" / "fail" / "unknown");
 * only "fail" alerts. Returns 1 when a notice was written. */
int ss_degraded_notice(const char *kb, const char *retrieval, char *out, size_t cap);

#endif /* SESSION_DEGRADED_NOTICE_H */
