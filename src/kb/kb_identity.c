/* kb_identity.c: the authenticated principal handle + canonical identity key.
 * See kb_identity.h. Composite resolution (kb_identity_resolve) lands in slice 2. */

#include "kb_identity.h"

#include "db2/management_intent_fields.h" /* db2_intent_bare_username (header-only) */

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
   /* collect lowercase hex, dropping ':' and whitespace separators. REJECT rather
    * than truncate an over-long serial — a truncated serial could collide two
    * distinct certificates onto one identity key. */
   char buf[512];
   size_t n = 0;
   for (; *s; ++s)
   {
      if (*s == ':' || *s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
         continue;
      if (n + 1 >= sizeof(buf))
         return -1; /* over-long serial: fail, never truncate */
      buf[n++] = (char)tolower((unsigned char)*s);
   }
   buf[n] = '\0';
   /* strip leading zeros, but keep a single "0" if the value is all zeros */
   const char *p = buf;
   while (p[0] == '0' && p[1] != '\0')
      ++p;
   if (p[0] == '\0')
      p = "0";
   size_t plen = strlen(p);
   if (plen >= cap)
      return -1; /* would not fit the output buffer: fail, never truncate */
   memcpy(out, p, plen + 1);
   return 0;
}

/* Copy src into dst[cap] but REJECT (return -1) rather than silently truncate — a
 * truncated issuer/subject could collide two distinct identities into one key. */
static int copy_strict(char *dst, size_t cap, const char *src)
{
   if (!src)
   {
      dst[0] = '\0';
      return 0;
   }
   size_t n = strlen(src);
   if (n >= cap)
      return -1;
   /* Reject control characters: a verified issuer/subject is printable text, and a
    * control char in an identity key would be confusing at best and a smuggling
    * vector at worst. */
   for (size_t i = 0; i < n; ++i)
      if ((unsigned char)src[i] < 0x20 || (unsigned char)src[i] == 0x7f)
         return -1;
   memcpy(dst, src, n + 1);
   return 0;
}

/* Percent-encode a key component so the ':' delimiters in an identity_key are
 * UNAMBIGUOUS: '%'->"%25" and ':'->"%3A", everything else verbatim. This makes
 * (kind, issuer, subject) -> key injective, so issuer "a:b"+subject "c" can never
 * collide with issuer "a"+subject "b:c". Returns 0, or -1 if the encoding would
 * overflow out[cap]. */
static int id_encode(const char *in, char *out, size_t cap)
{
   size_t o = 0;
   for (const char *p = in; *p; ++p)
   {
      const char *rep = NULL;
      if (*p == '%')
         rep = "%25";
      else if (*p == ':')
         rep = "%3A";
      if (rep)
      {
         if (o + 3 >= cap)
            return -1;
         out[o++] = rep[0];
         out[o++] = rep[1];
         out[o++] = rep[2];
      }
      else
      {
         if (o + 1 >= cap)
            return -1;
         out[o++] = *p;
      }
   }
   if (o >= cap)
      return -1;
   out[o] = '\0';
   return 0;
}

static int id_decode(const char *in, size_t len, char *out, size_t cap)
{
   if (!in || !len || !out || cap == 0)
      return -1;
   size_t o = 0;
   for (size_t i = 0; i < len; ++i)
   {
      unsigned char c = (unsigned char)in[i];
      if (c == '%')
      {
         if (i + 2 >= len)
            return -1;
         if (in[i + 1] == '2' && in[i + 2] == '5')
            c = '%';
         else if (in[i + 1] == '3' && in[i + 2] == 'A')
            c = ':';
         else
            return -1;
         i += 2;
      }
      if (c < 0x20 || c == 0x7f || o + 1 >= cap)
         return -1;
      out[o++] = (char)c;
   }
   out[o] = '\0';
   return o ? 0 : -1;
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
      if (copy_strict(out->issuer, sizeof(out->issuer), issuer) != 0 ||
          copy_strict(out->subject, sizeof(out->subject), v->subject) != 0)
         return -1;
   }
   else
   {
      out->kind = KB_PRIN_OWNER;
      if (copy_strict(out->subject, sizeof(out->subject), v->subject[0] ? v->subject : "owner") !=
          0)
         return -1;
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
   if (copy_strict(out->issuer, sizeof(out->issuer), cert_issuer) != 0)
      return -1;
   if (kb_cert_serial_normalize(cert_serial, out->subject, sizeof(out->subject)) != 0)
      return -1;
   if (cn && copy_strict(out->label, sizeof(out->label), cn) != 0)
      return -1;
   out->authenticated = 1;
   return 0;
}

int kb_principal_from_host_account(const char *username, kb_principal_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!username || !out)
      return -1;
   /* The same predicate the identity tables' subject CHECK mirrors, plus the
    * reserved name. Checked HERE and not only at the route: this constructor is
    * what stamps authenticated = 1, and a principal that could not be a legal
    * subject must not carry that stamp anywhere. */
   if (!db2_intent_bare_username(username) || strcmp(username, "owner") == 0)
      return -1;
   out->kind = KB_PRIN_HOST;
   snprintf(out->subject, sizeof(out->subject), "%s", username);
   out->authenticated = 1;
   return 0;
}

int kb_identity_key(const kb_principal_t *p, char *out, size_t cap)
{
   if (!p || !out || cap == 0 || !p->authenticated)
      return -1;
   char eiss[3 * sizeof(p->issuer)];
   char esub[3 * sizeof(p->subject)];
   int n;
   switch (p->kind)
   {
   case KB_PRIN_OIDC:
      if (!p->issuer[0] || !p->subject[0])
         return -1;
      if (id_encode(p->issuer, eiss, sizeof(eiss)) != 0 ||
          id_encode(p->subject, esub, sizeof(esub)) != 0)
         return -1;
      n = snprintf(out, cap, "oidc:%s:%s", eiss, esub);
      return (n > 0 && (size_t)n < cap) ? 0 : -1; /* reject truncation */
   case KB_PRIN_CERT:
      if (!p->issuer[0] || !p->subject[0])
         return -1;
      if (id_encode(p->issuer, eiss, sizeof(eiss)) != 0 ||
          id_encode(p->subject, esub, sizeof(esub)) != 0)
         return -1;
      n = snprintf(out, cap, "cert:%s:%s", eiss, esub);
      return (n > 0 && (size_t)n < cap) ? 0 : -1;
   case KB_PRIN_OWNER:
      n = snprintf(out, cap, "owner");
      return (n > 0 && (size_t)n < cap) ? 0 : -1;
   case KB_PRIN_HOST:
      /* The bare username, verbatim: no prefix and no encoding. The constructor
       * has already restricted it to the grammar's bare form, which contains no
       * character that would need escaping. */
      if (!p->subject[0])
         return -1;
      n = snprintf(out, cap, "%s", p->subject);
      return (n > 0 && (size_t)n < cap) ? 0 : -1;
   default:
      return -1;
   }
}

int kb_principal_from_identity_key(const char *identity_key, kb_principal_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!identity_key || !out)
      return -1;

   char canonical[DB2_INTENT_ACTOR_MAX + 1] = {0};
   size_t n = strnlen(identity_key, sizeof(canonical));
   if (!n || n >= sizeof(canonical))
      return -1;
   memcpy(canonical, identity_key, n);
   if (!db2_intent_canonical_actor(canonical, sizeof(canonical)))
      return -1;
   if (strcmp(canonical, "owner") == 0)
   {
      kb_verify_result_t verified;
      memset(&verified, 0, sizeof(verified));
      snprintf(verified.subject, sizeof(verified.subject), "%s", "owner");
      return kb_principal_from_verify(&verified, "", out);
   }
   if (!strchr(canonical, ':'))
      return kb_principal_from_host_account(canonical, out);

   int cert = strncmp(canonical, "cert:", 5) == 0;
   const char *first = canonical + 5;
   const char *middle = strchr(first, ':');
   if (!middle)
      return -1;
   char issuer[256] = "";
   char subject[256] = "";
   if (id_decode(first, (size_t)(middle - first), issuer, sizeof(issuer)) != 0 ||
       id_decode(middle + 1, strlen(middle + 1), subject, sizeof(subject)) != 0)
      return -1;
   if (cert)
      return kb_principal_from_cert(issuer, subject, NULL, out);

   kb_verify_result_t verified;
   memset(&verified, 0, sizeof(verified));
   if (copy_strict(verified.subject, sizeof(verified.subject), subject) != 0)
      return -1;
   return kb_principal_from_verify(&verified, issuer, out);
}

/* ---- Composite identity resolution (slice 2, I7) ------------------------- */

static int teams_contains(const int64_t *a, int n, int64_t v)
{
   for (int i = 0; i < n; ++i)
      if (a[i] == v)
         return 1;
   return 0;
}

kb_resolve_status_t kb_identity_combine(const int64_t *tteams, int n_t, int64_t tdefault,
                                        int has_transport, const int64_t *ateams, int n_a,
                                        int64_t adefault, int has_actor, int64_t named_team,
                                        int64_t *out_teams, int *out_n, int64_t *out_billing)
{
   if (out_n)
      *out_n = 0;
   if (out_billing)
      *out_billing = 0;
   if (!out_teams || !out_n || !out_billing)
      return KB_RESOLVE_CONFLICT;
   if (!has_transport && !has_actor)
      return KB_RESOLVE_NO_PRINCIPAL;

   int n = 0;
   if (has_transport && has_actor)
   {
      /* The billing team must be valid for BOTH principals: intersect. */
      for (int i = 0; i < n_t && n < KB_MAX_TEAMS; ++i)
         if (teams_contains(ateams, n_a, tteams[i]))
            out_teams[n++] = tteams[i];
      if (n == 0)
         return KB_RESOLVE_CONFLICT; /* both present, empty intersection: reject */
   }
   else
   {
      const int64_t *src = has_transport ? tteams : ateams;
      int ns = has_transport ? n_t : n_a;
      for (int i = 0; i < ns && n < KB_MAX_TEAMS; ++i)
         out_teams[n++] = src[i];
      /* A single principal with an empty team set is OK-with-no-teams (deny
       * downstream), not a conflict. */
   }
   *out_n = n;

   if (named_team != 0)
   {
      if (!teams_contains(out_teams, n, named_team))
         return KB_RESOLVE_CONFLICT; /* named a team outside the resolved set */
      *out_billing = named_team;
      return KB_RESOLVE_OK;
   }

   /* No team named: fall back to a default. */
   if (has_transport && has_actor)
   {
      /* Composite default only when both defaults agree AND lie in the set. */
      if (tdefault != 0 && tdefault == adefault && teams_contains(out_teams, n, tdefault))
      {
         *out_billing = tdefault;
         return KB_RESOLVE_OK;
      }
      return KB_RESOLVE_AMBIGUOUS_DEFAULT; /* defaults differ: must name a team */
   }
   int64_t d = has_transport ? tdefault : adefault;
   if (d != 0 && teams_contains(out_teams, n, d))
      *out_billing = d;
   return KB_RESOLVE_OK;
}
