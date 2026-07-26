/* Exercises db2_management_identity_authority_admit against a REAL Postgres as
 * the real aimee_kb_token_authority_runtime role. Built and run on CT 301. */
#include "management_token_authority.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* oauth_pkce.c pulls this in for verifier generation, which nothing on the
 * identity path calls. Abort rather than return weak bytes if that ever changes. */
int platform_random_bytes(void *buf, size_t n);
int platform_random_bytes(void *buf, size_t n)
{
   (void)buf;
   (void)n;
   fprintf(stderr, "platform_random_bytes called on a path that should not need it\n");
   abort();
}

static int fails = 0;
#define CHECK(cond, name)                                                                          \
   do                                                                                              \
   {                                                                                               \
      if (!(cond))                                                                                 \
      {                                                                                            \
         printf("FAIL: %s\n", name);                                                               \
         fails++;                                                                                  \
      }                                                                                            \
      else                                                                                         \
         printf("ok: %s\n", name);                                                                 \
   } while (0)

int main(int argc, char **argv)
{
   const char *conninfo = argc > 1 ? argv[1] : "";
   db2_management_token_authority_ctx_t ctx;
   memset(&ctx, 0, sizeof(ctx));
   char err[512] = "";
   if (db2_management_token_authority_open(&ctx, conninfo, err, sizeof(err)) != 0)
   {
      printf("FAIL: could not open authority ctx: %s\n", err);
      return 1;
   }
   printf("ok: opened authority connection (role_assert passed)\n");

   kb_identity_token_authority_record_t rec;
   char c64[65], j64[65];
   memset(c64, 'a', 64);
   c64[64] = 0;
   memset(j64, 'b', 64);
   j64[64] = 0;

   /* Absent intent must NOT be OK and must leave the record zeroed. */
   db2_management_token_authority_result_t rc =
       db2_management_identity_authority_admit(&ctx, c64, j64, &rec);
   CHECK(rc != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK, "absent intent is refused");
   CHECK(rec.correlation_id[0] == 0 && rec.team_id == 0, "refused admit leaves the record zeroed");
   printf("   (absent-intent result code = %d)\n", (int)rc);

   /* Malformed identifiers are rejected before any SQL runs. The buffers stay
    * char[65] because that is what the signature promises; only the contents
    * are wrong. */
   char cshort[65] = "short";
   char jbad[65];
   memset(jbad, 'Z', 64);
   jbad[64] = 0; /* right length, not lowercase hex */
   rc = db2_management_identity_authority_admit(&ctx, cshort, j64, &rec);
   CHECK(rc == DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY, "short correlation_id refused");
   rc = db2_management_identity_authority_admit(&ctx, c64, jbad, &rec);
   CHECK(rc == DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY, "non-hex jti refused");
   rc = db2_management_identity_authority_admit(&ctx, c64, j64, NULL);
   CHECK(rc == DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY, "NULL out refused");

   /* The identity kind resolves through the shared namespace resolver. */
   db2_management_token_intent_kind_t kind = 0;
   rc = db2_management_token_authority_kind(&ctx, c64, j64, &kind);
   CHECK(rc != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK, "kind of an absent intent is not OK");

   db2_management_token_authority_close(&ctx);
   printf(fails ? "IDENTITY FACADE: FAILED\n" : "IDENTITY FACADE: ALL PASSED\n");
   return fails ? 1 : 0;
}
