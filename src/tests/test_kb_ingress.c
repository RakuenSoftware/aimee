/* test_kb_ingress.c (B5): identity-header ingress detection. */
#include "kb_ingress.h"
#include <stdio.h>

static int fails = 0;
#define CHECK(c, m)                                                                                \
   do                                                                                              \
   {                                                                                               \
      if (!(c))                                                                                    \
      {                                                                                            \
         printf("FAIL: %s\n", m);                                                                  \
         fails++;                                                                                  \
      }                                                                                            \
   } while (0)

int main(void)
{
   CHECK(kb_ingress_identity_header_present(
             "GET /v1/x HTTP/1.1\r\nHost: h\r\nX-Aimee-Principal: user:evil\r\n\r\n") == 1,
         "detects X-Aimee-Principal");
   CHECK(kb_ingress_identity_header_present("GET / HTTP/1.1\r\nx-aimee-actor: a\r\n\r\n") == 1,
         "case-insensitive X-Aimee-Actor");
   CHECK(kb_ingress_identity_header_present("GET / HTTP/1.1\r\nX-Aimee-Session-Key:k\r\n\r\n") == 1,
         "no-space colon");
   CHECK(kb_ingress_identity_header_present("GET / HTTP/1.1\r\nX-Aimee-Source : s\r\n\r\n") == 1,
         "space before colon");
   const char *caller = "GET / HTTP/1.1\r\nX-Aimee-Caller-Subject: oidc:https%3A//idp:user\r\n\r\n";
   CHECK(kb_ingress_identity_header_present(caller) == 1,
         "caller assertion rejected on ordinary ingress");
   CHECK(kb_ingress_identity_header_present_ex(caller, 1) == 0,
         "authenticated service boundary may consume caller assertion");
   CHECK(kb_ingress_identity_header_present_ex(
             "GET / HTTP/1.1\r\nX-Aimee-Caller-Subject: aimee\r\nX-Aimee-Principal: evil\r\n\r\n",
             1) == 1,
         "service exception does not permit another identity header");
   CHECK(kb_ingress_identity_header_present(
             "GET / HTTP/1.1\r\nHost: h\r\nAuthorization: Bearer x\r\n\r\n") == 0,
         "benign headers pass");
   CHECK(kb_ingress_identity_header_present("GET / HTTP/1.1\r\nX-Request-ID: r\r\n\r\n") == 0,
         "non-identity X-* passes");
   /* An X-Aimee-* string in a later BODY (after the blank line) must not trip it. */
   CHECK(kb_ingress_identity_header_present(
             "POST / HTTP/1.1\r\nHost: h\r\n\r\nX-Aimee-Principal: in-body") == 0,
         "body content ignored");
   CHECK(kb_ingress_identity_header_present(NULL) == 0, "null safe");
   if (fails == 0)
      printf("test_kb_ingress: all passed\n");
   return fails ? 1 : 0;
}
