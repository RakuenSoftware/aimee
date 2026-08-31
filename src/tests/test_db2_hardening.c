/* test_db2_hardening.c: pure DSN verify-full parsing (I1). The runtime-role check
 * needs a live server and is covered by the PG-integration gate. */

#include "modules/db2/c/db2_hardening.h"

#include <stdio.h>

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

int main(void)
{
   /* Accept only verify-full, in URL query and keyword/value DSN forms. */
   CHECK(db2_hardening_dsn_verify_full("postgres://u:p@h:5432/db?sslmode=verify-full") == 1,
         "url query verify-full accepted");
   CHECK(db2_hardening_dsn_verify_full("host=h dbname=db sslmode=verify-full") == 1,
         "kv dsn verify-full accepted");
   CHECK(db2_hardening_dsn_verify_full("host=h dbname=db sslmode='verify-full'") == 1,
         "quoted verify-full accepted");
   CHECK(db2_hardening_dsn_verify_full(
             "postgres://u:p@h/db?sslmode=verify-full&connect_timeout=5") == 1,
         "verify-full with trailing params accepted");

   /* Reject weaker or absent modes — these must fail closed on a hardened tier. */
   CHECK(db2_hardening_dsn_verify_full("postgres://u:p@h:5432/db") == 0, "no sslmode rejected");
   CHECK(db2_hardening_dsn_verify_full("postgres://u:p@h/db?sslmode=require") == 0,
         "require rejected");
   CHECK(db2_hardening_dsn_verify_full("host=h sslmode=verify-ca") == 0, "verify-ca rejected");
   CHECK(db2_hardening_dsn_verify_full("host=h sslmode=disable") == 0, "disable rejected");
   CHECK(db2_hardening_dsn_verify_full(NULL) == 0, "null rejected");
   CHECK(db2_hardening_dsn_verify_full("") == 0, "empty rejected");

   if (fails == 0)
      printf("test_db2_hardening: all passed\n");
   return fails ? 1 : 0;
}
