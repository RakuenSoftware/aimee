/* cleartext_guard.h: one rule for "would this put a credential on the wire?".
 *
 * Two clients send bearer tokens: the thin client (aimee_client.c) to
 * aimee-server, and the kb client (kb_client.c) to aimee-kb. Both must refuse
 * plaintext to a non-loopback host, and they must agree on what loopback means
 * — a second, subtly different copy of this check is how one of them ends up
 * weaker than the other.
 *
 * Header-only for the same reason dependency_breaker.h is: the kb client links
 * into many focused unit-test binaries that do not carry aimee_client.o, and a
 * shared implementation must not drag a new link dependency into all of them.
 */
#ifndef DEC_CLEARTEXT_GUARD_H
#define DEC_CLEARTEXT_GUARD_H 1

#include <string.h>

/* localhost, 127.0.0.0/8, and ::1 are the only hosts where a cleartext bearer
 * stays on the machine; anything else puts it on the wire. */
static inline int cleartext_host_is_loopback(const char *host)
{
   if (!host || !host[0])
      return 0;
   if (strcmp(host, "localhost") == 0 || strcmp(host, "::1") == 0 || strcmp(host, "[::1]") == 0)
      return 1;
   return strncmp(host, "127.", 4) == 0; /* 127.0.0.0/8 */
}

/* 1 when sending |token| to a server at (|is_https|, |host|) would transmit the
 * credential in cleartext: a non-empty bearer over plaintext to a non-loopback
 * host. Callers refuse such requests rather than leak the bearer. */
static inline int cleartext_would_leak(int is_https, const char *host, const char *token)
{
   return (token && *token && !is_https && !cleartext_host_is_loopback(host)) ? 1 : 0;
}

#endif /* DEC_CLEARTEXT_GUARD_H */
