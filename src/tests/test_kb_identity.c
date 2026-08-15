/* test_kb_identity.c: P1 identity handle + canonical key (slice 1, pure/no-DB). */

#include "kb_identity.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg)                                                                           \
   do                                                                                              \
   {                                                                                               \
      if (!(cond))                                                                                 \
      {                                                                                            \
         printf("FAIL: %s\n", msg);                                                                \
         fails++;                                                                                  \
      }                                                                                            \
   } while (0)

static void test_serial_normalize(void)
{
   char out[128];
   CHECK(kb_cert_serial_normalize("0A:1B:2C", out, sizeof(out)) == 0 && strcmp(out, "a1b2c") == 0,
         "colon+upper hex normalizes");
   CHECK(kb_cert_serial_normalize("0x00FF", out, sizeof(out)) == 0 && strcmp(out, "ff") == 0,
         "0x prefix + leading zeros stripped");
   CHECK(kb_cert_serial_normalize("00:00:00", out, sizeof(out)) == 0 && strcmp(out, "0") == 0,
         "all-zero collapses to single 0");
   CHECK(kb_cert_serial_normalize("DEADbeef", out, sizeof(out)) == 0 &&
             strcmp(out, "deadbeef") == 0,
         "case-folded");
   /* Two serials that differ only in separators/leading-zeros map to one key. */
   char a[128], b[128];
   kb_cert_serial_normalize("0a:0b", a, sizeof(a));
   kb_cert_serial_normalize("0A0B", b, sizeof(b));
   CHECK(strcmp(a, b) == 0, "separator-insensitive canonicalization");
}

static void test_identity_key(void)
{
   kb_verify_result_t v;
   memset(&v, 0, sizeof(v));
   snprintf(v.subject, sizeof(v.subject), "%s", "sub-123");

   kb_principal_t p;
   char key[600];

   /* OIDC: oidc:<enc(iss)>:<enc(sub)> — the ':' in https:// is percent-encoded so
    * the structural ':' delimiters are unambiguous. */
   CHECK(kb_principal_from_verify(&v, "https://idp.example", &p) == 0 && p.kind == KB_PRIN_OIDC &&
             p.authenticated,
         "oidc principal built");
   CHECK(kb_identity_key(&p, key, sizeof(key)) == 0 &&
             strcmp(key, "oidc:https%3A//idp.example:sub-123") == 0,
         "oidc identity key encodes issuer colon");

   /* Owner: no issuer -> owner principal, key "owner" */
   CHECK(kb_principal_from_verify(&v, "", &p) == 0 && p.kind == KB_PRIN_OWNER, "owner principal");
   CHECK(kb_identity_key(&p, key, sizeof(key)) == 0 && strcmp(key, "owner") == 0, "owner key");

   /* Cert: cert:<issuer>:<normalized serial>; CN is a label, not the key */
   CHECK(kb_principal_from_cert("CN=aimee-ca", "0A:1B", "server-7", &p) == 0 &&
             p.kind == KB_PRIN_CERT && p.authenticated,
         "cert principal built");
   CHECK(kb_identity_key(&p, key, sizeof(key)) == 0 && strcmp(key, "cert:CN=aimee-ca:a1b") == 0,
         "cert identity key uses normalized serial, not CN");
   CHECK(strcmp(p.label, "server-7") == 0, "cert CN kept only as label");
}

static void test_identity_key_parse(void)
{
   static const char *valid[] = {"owner", "aimee", "oidc:https%3A//idp.example:sub%3A42",
                                 "cert:CN=aimee-ca:a1b"};
   for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i)
   {
      kb_principal_t p;
      char roundtrip[600];
      CHECK(kb_principal_from_identity_key(valid[i], &p) == 0 && p.authenticated,
            "canonical identity parses");
      CHECK(kb_identity_key(&p, roundtrip, sizeof(roundtrip)) == 0 &&
                strcmp(roundtrip, valid[i]) == 0,
            "parsed identity round-trips exactly");
   }
   static const char *invalid[] = {"",           "uid:1000",   "webuser:alice", "oidc:x",
                                   "oidc:a:b:c", "oidc:a:%2f", "cert:a:XYZ",    "owner:extra"};
   for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
   {
      kb_principal_t p;
      CHECK(kb_principal_from_identity_key(invalid[i], &p) == -1 && !p.authenticated,
            "non-canonical asserted identity rejected");
   }
}

static void test_unauthenticated_rejected(void)
{
   /* A zero-initialized principal is unauthenticated and must never yield a key. */
   kb_principal_t p;
   memset(&p, 0, sizeof(p));
   char key[128];
   CHECK(kb_identity_key(&p, key, sizeof(key)) == -1, "unauthenticated principal rejected");
   p.authenticated = 1; /* forge the flag but leave kind NONE */
   CHECK(kb_identity_key(&p, key, sizeof(key)) == -1, "kind=NONE rejected even if flag set");
}

static void test_no_collision(void)
{
   /* (issuer "a:b", subject "c") and (issuer "a", subject "b:c") must NOT collide. */
   kb_verify_result_t v1, v2;
   memset(&v1, 0, sizeof(v1));
   memset(&v2, 0, sizeof(v2));
   snprintf(v1.subject, sizeof(v1.subject), "%s", "c");
   snprintf(v2.subject, sizeof(v2.subject), "%s", "b:c");
   kb_principal_t p1, p2;
   char k1[600], k2[600];
   kb_principal_from_verify(&v1, "a:b", &p1);
   kb_principal_from_verify(&v2, "a", &p2);
   CHECK(kb_identity_key(&p1, k1, sizeof(k1)) == 0, "k1 built");
   CHECK(kb_identity_key(&p2, k2, sizeof(k2)) == 0, "k2 built");
   CHECK(strcmp(k1, k2) != 0, "colon-in-component does not collide identities");
}

static void test_overlong_rejected(void)
{
   /* An issuer longer than the principal buffer must be REJECTED, not truncated. */
   char big[1024];
   memset(big, 'x', sizeof(big) - 1);
   big[sizeof(big) - 1] = '\0';
   kb_verify_result_t v;
   memset(&v, 0, sizeof(v));
   snprintf(v.subject, sizeof(v.subject), "%s", "s");
   kb_principal_t p;
   CHECK(kb_principal_from_verify(&v, big, &p) == -1,
         "over-long issuer rejected (no silent trunc)");

   /* A tiny output buffer must fail rather than emit a truncated (colliding) key. */
   kb_verify_result_t v2;
   memset(&v2, 0, sizeof(v2));
   snprintf(v2.subject, sizeof(v2.subject), "%s", "subject-value");
   kb_principal_t p2;
   char tiny[8];
   CHECK(kb_principal_from_verify(&v2, "issuer", &p2) == 0, "principal ok");
   CHECK(kb_identity_key(&p2, tiny, sizeof(tiny)) == -1, "undersized key buffer rejected");

   /* An over-long certificate serial must be rejected, not truncated (else two
    * distinct serials sharing a prefix would collapse to one identity). */
   char bigserial[2048];
   memset(bigserial, 'a', sizeof(bigserial) - 1);
   bigserial[sizeof(bigserial) - 1] = '\0';
   char norm[64];
   CHECK(kb_cert_serial_normalize(bigserial, norm, sizeof(norm)) == -1,
         "over-long cert serial rejected");
   /* A control character in issuer/subject is rejected. */
   kb_verify_result_t v3;
   memset(&v3, 0, sizeof(v3));
   snprintf(v3.subject, sizeof(v3.subject), "%s", "ab");
   kb_principal_t p3;
   CHECK(kb_principal_from_verify(&v3, "iss\nwith-newline", &p3) == -1,
         "control char in issuer rejected");
}

int main(void)
{
   test_serial_normalize();
   test_identity_key();
   test_identity_key_parse();
   test_unauthenticated_rejected();
   test_no_collision();
   test_overlong_rejected();
   if (fails == 0)
      printf("test_kb_identity: all passed\n");
   return fails ? 1 : 0;
}
