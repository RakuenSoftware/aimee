/* Wire contract for the sandbox module's learned-toolchain and proxy-policy stages.
 *
 * All stages carry JSON in both directions: a shell command and a git root are
 * variable-length, as are proxy request lines, allowlists and addresses. The
 * kinds are fixed by the process contract at
 * 4096 + ordinal*256 + stage; sandbox is ordinal 26, so these are not a free
 * choice. */
#ifndef AIMEE_SANDBOX_MODULE_API_H
#define AIMEE_SANDBOX_MODULE_API_H 1

#define AIMEE_SANDBOX_EVENT_OBSERVE       10753u
#define AIMEE_SANDBOX_STAGE_OBSERVE       1u
#define AIMEE_SANDBOX_EVENT_LOAD          10754u
#define AIMEE_SANDBOX_STAGE_LOAD          2u
#define AIMEE_SANDBOX_EVENT_PROXY_REQUEST 10755u
#define AIMEE_SANDBOX_STAGE_PROXY_REQUEST 3u
#define AIMEE_SANDBOX_EVENT_PROXY_ADDRESS 10756u
#define AIMEE_SANDBOX_STAGE_PROXY_ADDRESS 4u

#endif
