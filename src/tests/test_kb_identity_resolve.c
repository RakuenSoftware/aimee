/* test_kb_identity_resolve.c (slice 2, I7): the pure composite-identity
 * combination rule — fail-closed intersection, conflict, ambiguous default. */

#include "kb_identity.h"

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
   int64_t out[KB_MAX_TEAMS];
   int n = 0;
   int64_t bill = 0;
   kb_resolve_status_t st;

   /* Neither principal present -> NO_PRINCIPAL. */
   st = kb_identity_combine(NULL, 0, 0, 0, NULL, 0, 0, 0, 0, out, &n, &bill);
   CHECK(st == KB_RESOLVE_NO_PRINCIPAL, "no principal");

   /* Actor only: resolves to its teams; default used when no team named. */
   int64_t at[] = {1, 2, 3};
   st = kb_identity_combine(NULL, 0, 0, 0, at, 3, 2, 1, 0, out, &n, &bill);
   CHECK(st == KB_RESOLVE_OK && n == 3 && bill == 2, "actor-only default");

   /* Actor only, empty team set -> OK with 0 teams (deny downstream), billing 0. */
   st = kb_identity_combine(NULL, 0, 0, 0, at, 0, 0, 1, 0, out, &n, &bill);
   CHECK(st == KB_RESOLVE_OK && n == 0 && bill == 0, "actor-only empty set = deny");

   /* Both present: billing team must lie in the intersection. */
   int64_t tt[] = {2, 3, 4};
   st = kb_identity_combine(tt, 3, 3, 1, at, 3, 3, 1, 0, out, &n, &bill);
   CHECK(st == KB_RESOLVE_OK && n == 2 && bill == 3, "intersection {2,3}, agreed default 3");

   /* Both present, empty intersection -> CONFLICT (reject). */
   int64_t tt2[] = {5, 6};
   st = kb_identity_combine(tt2, 2, 5, 1, at, 3, 2, 1, 0, out, &n, &bill);
   CHECK(st == KB_RESOLVE_CONFLICT, "empty intersection rejected");

   /* Both present, named team OUTSIDE the intersection -> CONFLICT. */
   st = kb_identity_combine(tt, 3, 3, 1, at, 3, 2, 1, /*named*/ 4, out, &n, &bill);
   CHECK(st == KB_RESOLVE_CONFLICT, "named team outside intersection rejected");

   /* Both present, named team INSIDE the intersection -> OK, billed as named. */
   st = kb_identity_combine(tt, 3, 3, 1, at, 3, 2, 1, /*named*/ 2, out, &n, &bill);
   CHECK(st == KB_RESOLVE_OK && bill == 2, "named team in intersection billed");

   /* Both present, no named team, DEFAULTS DIFFER -> AMBIGUOUS (must name). */
   st = kb_identity_combine(tt, 3, 4, 1, at, 3, 2, 1, 0, out, &n, &bill);
   CHECK(st == KB_RESOLVE_AMBIGUOUS_DEFAULT, "differing defaults -> must name");

   /* Both present, agreed default NOT in intersection -> AMBIGUOUS. */
   st = kb_identity_combine(tt, 3, 4, 1, at, 3, 4, 1, 0, out, &n, &bill);
   CHECK(st == KB_RESOLVE_AMBIGUOUS_DEFAULT, "agreed default outside set -> must name");

   /* Transport only (a server acting for itself). */
   st = kb_identity_combine(tt, 3, 4, 1, NULL, 0, 0, 0, 0, out, &n, &bill);
   CHECK(st == KB_RESOLVE_OK && n == 3 && bill == 4, "transport-only default");

   if (fails == 0)
      printf("test_kb_identity_resolve: all passed\n");
   return fails ? 1 : 0;
}
