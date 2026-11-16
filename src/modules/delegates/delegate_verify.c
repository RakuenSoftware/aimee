/* delegate_verify.c: the seam to the delegates module's verification policy.
 *
 * Whether a verify run indicts the delegate's WORK or only the machine it ran
 * on is a judgement, so it is now server-go/modules/delegates/verify.go.
 *
 * The platform's signal ceiling stays here and travels with the request. It is
 * a property of the host whose `/bin/sh -c` produced the status, and it is a
 * compile-time constant only C can see; deriving it in the module would either
 * hardcode Linux's range or guess, and both misclassify a deliberate exit as
 * infrastructure wherever the real ceiling is lower.
 *
 * Fails closed as INFRA_ERROR with no escalation. Unable to judge, this must not
 * claim a work-product failure: that is the direction that blames the model for
 * its environment, which is the whole reason the classification exists.
 */
#include "delegate_verify.h"
#include <signal.h>

/* Highest status a POSIX shell can report as "command killed by signal N".
 * Derived from the PLATFORM's own signal range rather than hardcoded: Linux runs
 * to SIGRTMAX (64, so 192), while platforms without realtime signals top out far
 * lower (NSIG-1 ~ 31, so 159). */
#if defined(SIGRTMAX)
#define VERIFY_MAX_SIGNAL_STATUS (128 + SIGRTMAX)
#elif defined(NSIG)
#define VERIFY_MAX_SIGNAL_STATUS (128 + (NSIG - 1))
#else
#define VERIFY_MAX_SIGNAL_STATUS 159 /* 128 + 31, the conservative POSIX floor */
#endif

static delegate_verify_provider_fn g_verify_provider;

void delegate_register_verify_provider(delegate_verify_provider_fn provider)
{
   g_verify_provider = provider;
}
