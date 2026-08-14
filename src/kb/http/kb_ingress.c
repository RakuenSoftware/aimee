/* kb_ingress.c: deny-by-default identity-header ingress guard (P1 B5).
 *
 * Ordinary kb clients must NEVER be able to assert identity via a request header.
 * The sole exceptions are the bounded service-context headers on the dedicated
 * mTLS listener, where they are consumed only after the peer certificate,
 * rotating bearer, and service OIDC/PAM identity have all verified. */

#include "kb_ingress.h"

#include <ctype.h>
#include <string.h>

/* The spoofable identity headers (mirrors the server proxy-trust set that kb must
 * NOT honor: X-Aimee-Principal / -Actor / -Source / -Session-Key). Matched
 * case-insensitively at the start of a header line. */
static const char *const IDENTITY_HEADERS[] = {
    "x-aimee-principal",
    "x-aimee-actor",
    "x-aimee-source",
    "x-aimee-session-key",
    "x-aimee-user",
    "x-aimee-caller-subject",
    "x-aimee-caller-authorization",
    "x-aimee-server-id",
    "x-aimee-team-id",
    NULL,
};

/* Case-insensitive check whether `line` (a header line, up to CRLF) begins with
 * `name` followed by ':'. */
static int header_line_is(const char *line, const char *name)
{
   size_t i = 0;
   for (; name[i]; ++i)
      if (tolower((unsigned char)line[i]) != name[i])
         return 0;
   /* allow optional whitespace before the colon */
   while (line[i] == ' ' || line[i] == '\t')
      ++i;
   return line[i] == ':';
}

int kb_ingress_identity_header_present_ex(const char *raw_request, int allow_service_context)
{
   if (!raw_request)
      return 0;
   const char *p = raw_request;
   /* Walk each header line. Stop at the end of the header block (blank line). */
   while (p && *p)
   {
      if (p[0] == '\r' && p[1] == '\n')
         break; /* CRLFCRLF: end of headers */
      if (p[0] == '\n')
         break;
      for (int i = 0; IDENTITY_HEADERS[i]; ++i)
      {
         if (allow_service_context &&
             (strcmp(IDENTITY_HEADERS[i], "x-aimee-caller-subject") == 0 ||
              strcmp(IDENTITY_HEADERS[i], "x-aimee-caller-authorization") == 0 ||
              strcmp(IDENTITY_HEADERS[i], "x-aimee-server-id") == 0 ||
              strcmp(IDENTITY_HEADERS[i], "x-aimee-team-id") == 0))
            continue;
         if (header_line_is(p, IDENTITY_HEADERS[i]))
            return 1;
      }
      /* advance to the next line */
      const char *nl = strchr(p, '\n');
      if (!nl)
         break;
      p = nl + 1;
   }
   return 0;
}

int kb_ingress_identity_header_present(const char *raw_request)
{
   return kb_ingress_identity_header_present_ex(raw_request, 0);
}
