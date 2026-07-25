#ifndef DEC_AIMEE_VERSION_H
#define DEC_AIMEE_VERSION_H 1

/* Version -- can be overridden at compile time via -DAIMEE_VERSION='"..."' */
#ifndef AIMEE_VERSION
#define AIMEE_VERSION "0.3.0"
#endif

/* HEAD commit timestamp -- embedded at build time for stale-binary detection.
 * Zero means the information was unavailable at build time. */
#ifndef AIMEE_GIT_COMMIT_TIME
#define AIMEE_GIT_COMMIT_TIME 0L
#endif

/* Build ID -- identifies a specific local build for diagnostics and tests. */
#ifndef AIMEE_BUILD_ID
#define AIMEE_BUILD_ID "dev"
#endif

#endif /* DEC_AIMEE_VERSION_H */
