/* kb_identity.c: the authenticated principal handle + canonical identity key.
 * See kb_identity.h. Composite resolution (kb_identity_resolve) lands in slice 2. */

#include "kb_identity.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

int kb_cert_serial_normalize(const char *serial, char *out, size_t cap)
{
   if (!out || cap == 0)
      return -1;
   out[0] = '\0';
   if (!serial)
      return -1;
   const char *s = serial;
   if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
      s += 2;
   /* collect lowercase hex, dropping ':' and whitespace separators */
   char buf[512];
   size_t n = 0;
   for (; *s && n + 1 < sizeof(buf); ++s)
   {
      if (*s == ':' || *s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
         continue;
      buf[n++] = (char)tolower((unsigned char)*s);
   }
   buf[n] = '\0';
   /* strip leading zeros, but keep a single "0" if the value is all zeros */
   const char *p = buf;
   while (p[0] == '0' && p[1] != '\0')
      ++p;
   if (p[0] == '\0')
      p = "0";
   snprintf(out, cap, "%s", p);
   return 0;
}

int kb_principal_from_verify(const kb_verify_result_t *v, const char *issuer, kb_principal_t *out)
{
   if (!v || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   /* An issuer distinguishes an OIDC subject from the owner/kb-token principal. */
   if (issuer && issuer[0])
   {
      out->kind = KB_PRIN_OIDC;
      snprintf(out->issuer, sizeof(out->issuer), "%s", issuer);
      snprintf(out->subject, sizeof(out->subject), "%s", v->subject);
   }
   else
   {
      out->kind = KB_PRIN_OWNER;
      snprintf(out->subject, sizeof(out->subject), "%s", v->subject[0] ? v->subject : "owner");
   }
   out->authenticated = 1;
   return 0;
}

int kb_principal_from_cert(const char *cert_issuer, const char *cert_serial, const char *cn,
                           kb_principal_t *out)
{
   if (!out || !cert_issuer || !cert_serial)
      return -1;
   memset(out, 0, sizeof(*out));
   out->kind = KB_PRIN_CERT;
   snprintf(out->issuer, sizeof(out->issuer), "%s", cert_issuer);
   if (kb_cert_serial_normalize(cert_serial, out->subject, sizeof(out->subject)) != 0)
      return -1;
   if (cn)
      snprintf(out->label, sizeof(out->label), "%s", cn);
   out->authenticated = 1;
   return 0;
}

int kb_identity_key(const kb_principal_t *p, char *out, size_t cap)
{
   if (!p || !out || cap == 0 || !p->authenticated)
      return -1;
   switch (p->kind)
   {
   case KB_PRIN_OIDC:
      if (!p->issuer[0] || !p->subject[0])
         return -1;
      snprintf(out, cap, "oidc:%s:%s", p->issuer, p->subject);
      return 0;
   case KB_PRIN_CERT:
      if (!p->issuer[0] || !p->subject[0])
         return -1;
      snprintf(out, cap, "cert:%s:%s", p->issuer, p->subject);
      return 0;
   case KB_PRIN_OWNER:
      snprintf(out, cap, "owner");
      return 0;
   default:
      return -1;
   }
}
