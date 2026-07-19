/* test_openai_responses_store.c: unit tests for the in-process /v1/responses
 * continuation store (no sockets, no agent execution). */
#include "openai_responses_store.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
   printf("openai_responses_store: ");
   openai_responses_store_reset(); /* order-independent */

   char buf[256];

   /* unknown id -> 0, out cleared */
   {
      strcpy(buf, "dirty");
      assert(openai_responses_store_get("nope", buf, sizeof(buf)) == 0);
      assert(buf[0] == '\0');
   }

   /* store + retrieve exact bytes */
   {
      const char *t = "user: hi\nassistant: hello\n";
      openai_responses_store_put("resp_1", t);
      assert(openai_responses_store_get("resp_1", buf, sizeof(buf)) == 1);
      assert(strcmp(buf, t) == 0);
   }

   /* overwrite same id */
   {
      openai_responses_store_put("resp_1", "first");
      openai_responses_store_put("resp_1", "second");
      assert(openai_responses_store_get("resp_1", buf, sizeof(buf)) == 1);
      assert(strcmp(buf, "second") == 0);
   }

   /* two distinct ids are independent */
   {
      openai_responses_store_put("resp_a", "alpha");
      openai_responses_store_put("resp_b", "beta");
      assert(openai_responses_store_get("resp_a", buf, sizeof(buf)) == 1);
      assert(strcmp(buf, "alpha") == 0);
      assert(openai_responses_store_get("resp_b", buf, sizeof(buf)) == 1);
      assert(strcmp(buf, "beta") == 0);
   }

   /* NULL / empty id -> 0 */
   {
      assert(openai_responses_store_get(NULL, buf, sizeof(buf)) == 0);
      assert(openai_responses_store_get("", buf, sizeof(buf)) == 0);
   }

   /* truncation into a small buffer: found, NUL-terminated within bounds */
   {
      char small[8];
      openai_responses_store_put("resp_long", "0123456789abcdef");
      assert(openai_responses_store_get("resp_long", small, sizeof(small)) == 1);
      assert(small[sizeof(small) - 1] == '\0');
      assert(strncmp(small, "0123456789abcdef", sizeof(small) - 1) == 0);
   }

   /* overflow evicts the OLDEST entry (FIFO), not always slot 0. Fill the store
    * to capacity, then insert past it and confirm the earliest ids drop out in
    * insertion order while later ids stay retrievable. */
   {
      openai_responses_store_reset();
      const int cap = 256; /* OPENAI_RESPONSES_STORE_MAX */
      char id[32];
      for (int i = 0; i < cap; i++)
      {
         snprintf(id, sizeof(id), "of_%04d", i);
         openai_responses_store_put(id, "x");
      }
      /* full: every id 0..cap-1 is present */
      assert(openai_responses_store_get("of_0000", buf, sizeof(buf)) == 1);
      assert(openai_responses_store_get("of_0255", buf, sizeof(buf)) == 1);
      /* one more insert evicts the oldest (of_0000), keeps the rest + the new one */
      openai_responses_store_put("of_0256", "x");
      assert(openai_responses_store_get("of_0000", buf, sizeof(buf)) == 0);
      assert(openai_responses_store_get("of_0001", buf, sizeof(buf)) == 1);
      assert(openai_responses_store_get("of_0256", buf, sizeof(buf)) == 1);
      /* next insert evicts the next-oldest (of_0001), in order */
      openai_responses_store_put("of_0257", "x");
      assert(openai_responses_store_get("of_0001", buf, sizeof(buf)) == 0);
      assert(openai_responses_store_get("of_0002", buf, sizeof(buf)) == 1);
      assert(openai_responses_store_get("of_0257", buf, sizeof(buf)) == 1);
   }

   /* reset drops everything */
   {
      openai_responses_store_reset();
      assert(openai_responses_store_get("resp_1", buf, sizeof(buf)) == 0);
      assert(openai_responses_store_get("resp_a", buf, sizeof(buf)) == 0);
   }

   printf("ok\n");
   return 0;
}
