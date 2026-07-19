/* test_kb_identity.c: P1 identity handle + canonical key (slice 1, pure/no-DB). */

#include "kb_identity.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg)                                                                            \
   do                                                                                               \
   {                                                                                                \
      if (!(cond))                                                                                  \
      {                                                                                             \
         printf("FAIL: %s\n", msg);                                                                 \
         fails++;                                                                                   \
      }                                                                                             \
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
   CHECK(kb_cert_serial_normalize("DEADbeef", out, sizeof(out)) == 0 && strcmp(out, "deadbeef") == 0,
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

   /* OIDC: oidc:<iss>:<sub> */
   CHECK(kb_principal_from_verify(&v, "https://idp.example", &p) == 0 && p.kind == KB_PRIN_OIDC &&
             p.authenticated,
         "oidc principal built");
   CHECK(kb_identity_key(&p, key, sizeof(key)) == 0 &&
             strcmp(key, "oidc:https://idp.example:sub-123") == 0,
         "oidc identity key");

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

int main(void)
{
   test_serial_normalize();
   test_identity_key();
   test_unauthenticated_rejected();
   if (fails == 0)
      printf("test_kb_identity: all passed\n");
   return fails ? 1 : 0;
}
