/* test_model_pricing.c: DB1 server-owned per-model price table roundtrip. */
#include <assert.h>
#include <stdio.h>
#include "db1.h"
#include "db1/model_pricing.h"

static int near_eq(double a, double b)
{
   double d = a - b;
   return (d < 0 ? -d : d) < 1e-9;
}

int main(void)
{
   assert(db1_init(":memory:") == 0);

   double in = -1, out = -1;

   /* absent → 0, out params cleared. */
   assert(db1_model_price_get("MiniMax-M3", &in, &out) == 0);
   assert(near_eq(in, 0.0) && near_eq(out, 0.0));

   /* set → get roundtrip. */
   assert(db1_model_price_set("MiniMax-M3", 5.0, 10.0) == 0);
   assert(db1_model_price_get("MiniMax-M3", &in, &out) == 1);
   assert(near_eq(in, 5.0) && near_eq(out, 10.0));

   /* upsert overwrites. */
   assert(db1_model_price_set("MiniMax-M3", 1.5, 2.5) == 0);
   assert(db1_model_price_get("MiniMax-M3", &in, &out) == 1);
   assert(near_eq(in, 1.5) && near_eq(out, 2.5));

   /* an explicitly-free (0) row is still "present" (returns 1). */
   assert(db1_model_price_set("free-model", 0.0, 0.0) == 0);
   assert(db1_model_price_get("free-model", &in, &out) == 1);
   assert(near_eq(in, 0.0) && near_eq(out, 0.0));

   /* a different model is still absent. */
   assert(db1_model_price_get("other-model", NULL, NULL) == 0);

   /* delete reverts to absent (returns 0); deleting again is still 0. */
   assert(db1_model_price_delete("MiniMax-M3") == 0);
   assert(db1_model_price_get("MiniMax-M3", &in, &out) == 0);
   assert(db1_model_price_delete("MiniMax-M3") == 0);

   /* the CHECK constraint rejects a negative rate (no row stored). */
   assert(db1_model_price_set("bad-model", -1.0, 2.0) == -1);
   assert(db1_model_price_get("bad-model", NULL, NULL) == 0);

   /* bad args. */
   assert(db1_model_price_get("", &in, &out) == -1);
   assert(db1_model_price_set(NULL, 1.0, 1.0) == -1);
   assert(db1_model_price_delete("") == -1);

   db1_shutdown();
   printf("model_pricing: all tests passed\n");
   return 0;
}
