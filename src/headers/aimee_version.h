#ifndef DEC_AIMEE_VERSION_H
#define DEC_AIMEE_VERSION_H 1

/* Release series, MAJOR.MINOR. The only version component declared in the tree.
 *
 * The PATCH is never written here. It is inferred at release time from the
 * highest v<series>.* tag, so shipping 0.4.1 after 0.4.0 needs no commit to
 * this file and cannot drift from what was actually tagged. Moving to a new
 * series IS a decision, so it is an explicit edit of this one line and nothing
 * infers it. */
#ifndef AIMEE_VERSION_SERIES
#define AIMEE_VERSION_SERIES "0.4"
#endif

/* Full version -- can be overridden at compile time via -DAIMEE_VERSION='"..."'
 * (release and image builds inject the resolved version this way). The patch
 * component of this fallback is a placeholder for local builds, not a release
 * number; nothing reads it to decide what to publish. */
#ifndef AIMEE_VERSION
#define AIMEE_VERSION AIMEE_VERSION_SERIES ".0"
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
