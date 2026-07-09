/* test_curator_custom_stages.c: composed (custom) curator stage parsing +
 * validation — the guard for Phase D. Uses a mock base resolver (no DB, no real
 * stages) so it exercises only kb_curator_custom_stages_parse's contract: which
 * operator-supplied entries are accepted, defaulted, clamped, or rejected, and
 * that a derived descriptor reuses the base op's run fn on the base op's lane. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "kb/kb_curator_custom.h"

/* ── mock registry: two vetted base ops, one per lane ─────────────────────── */
static int run_idx(const kb_curator_extract_opts_t *o)
{
   (void)o;
   return 0;
}
static int run_llm(const kb_curator_extract_opts_t *o)
{
   (void)o;
   return 0;
}
static const kb_curator_stage_desc_t MOCK[] = {
    {"index_claims", "index claims", NULL, run_idx, 64, KB_CURATOR_LANE_INDEX},
    {"synthesize", "synthesize", NULL, run_llm, 1, KB_CURATOR_LANE_LLM},
};
static const kb_curator_stage_desc_t *mock_resolve(const char *name)
{
   if (!name)
      return NULL;
   for (size_t i = 0; i < sizeof(MOCK) / sizeof(MOCK[0]); i++)
      if (strcmp(MOCK[i].name, name) == 0)
         return &MOCK[i];
   return NULL;
}

/* Convenience: parse into a fresh buffer. */
static size_t parse(const char *json, kb_curator_custom_t *out, size_t max, int *nrej)
{
   return kb_curator_custom_stages_parse(json, mock_resolve, out, max, nrej);
}

int main(void)
{
   kb_curator_custom_t c[KB_CURATOR_MAX_CUSTOM];
   int nrej;

   /* 1. Valid entry: reuses base run, custom name/budget, default enabled. */
   {
      size_t n = parse("[{\"name\":\"claims_fast\",\"base_op\":\"index_claims\",\"budget\":128}]",
                       c, KB_CURATOR_MAX_CUSTOM, &nrej);
      assert(n == 1 && nrej == 0);
      assert(strcmp(c[0].name, "claims_fast") == 0);
      assert(strcmp(c[0].base_op, "index_claims") == 0);
      assert(c[0].base == &MOCK[0]);
      assert(c[0].budget == 128 && c[0].enabled == 1);

      kb_curator_stage_desc_t d;
      kb_curator_custom_to_desc(&c[0], &d);
      assert(d.run == run_idx);                /* base op's run reused */
      assert(d.lane == KB_CURATOR_LANE_INDEX); /* base op's native lane */
      assert(d.enabled == NULL);               /* presence == enabled */
      assert(d.budget == 128);
      assert(d.name == c[0].name && d.label == c[0].name);
   }

   /* 2. Budget defaults to the base op's when omitted. */
   {
      size_t n =
          parse("[{\"name\":\"a\",\"base_op\":\"index_claims\"}]", c, KB_CURATOR_MAX_CUSTOM, &nrej);
      assert(n == 1 && c[0].budget == 64);
   }

   /* 3. Non-positive budget clamps to 1. */
   {
      size_t n = parse("[{\"name\":\"a\",\"base_op\":\"synthesize\",\"budget\":0}]", c,
                       KB_CURATOR_MAX_CUSTOM, &nrej);
      assert(n == 1 && c[0].budget == 1);
      n = parse("[{\"name\":\"a\",\"base_op\":\"synthesize\",\"budget\":-9}]", c,
                KB_CURATOR_MAX_CUSTOM, &nrej);
      assert(n == 1 && c[0].budget == 1);
   }

   /* 4. enabled:false is surfaced (returned) but flagged disabled. */
   {
      size_t n = parse("[{\"name\":\"a\",\"base_op\":\"index_claims\",\"enabled\":false}]", c,
                       KB_CURATOR_MAX_CUSTOM, &nrej);
      assert(n == 1 && c[0].enabled == 0 && nrej == 0);
   }

   /* 5. Non-bool enabled is rejected (not coerced). */
   {
      size_t n = parse("[{\"name\":\"a\",\"base_op\":\"index_claims\",\"enabled\":\"yes\"}]", c,
                       KB_CURATOR_MAX_CUSTOM, &nrej);
      assert(n == 0 && nrej == 1);
   }

   /* 6. Unknown base_op is rejected (injection guard). */
   {
      size_t n = parse("[{\"name\":\"a\",\"base_op\":\"rm_rf\"}]", c, KB_CURATOR_MAX_CUSTOM, &nrej);
      assert(n == 0 && nrej == 1);
   }

   /* 7. A name colliding with a built-in is rejected. */
   {
      size_t n = parse("[{\"name\":\"synthesize\",\"base_op\":\"index_claims\"}]", c,
                       KB_CURATOR_MAX_CUSTOM, &nrej);
      assert(n == 0 && nrej == 1);
   }

   /* 8. A duplicate custom name is rejected (first kept, second dropped). */
   {
      size_t n = parse("[{\"name\":\"dup\",\"base_op\":\"index_claims\"},"
                       "{\"name\":\"dup\",\"base_op\":\"synthesize\"}]",
                       c, KB_CURATOR_MAX_CUSTOM, &nrej);
      assert(n == 1 && nrej == 1 && c[0].base == &MOCK[0]);
   }

   /* 9. Names outside [A-Za-z0-9_-] are rejected (log/GUI injection guard). */
   {
      assert(parse("[{\"name\":\"bad name\",\"base_op\":\"index_claims\"}]", c,
                   KB_CURATOR_MAX_CUSTOM, &nrej) == 0);
      assert(parse("[{\"name\":\"a.b\",\"base_op\":\"index_claims\"}]", c, KB_CURATOR_MAX_CUSTOM,
                   &nrej) == 0);
      assert(parse("[{\"name\":\"\",\"base_op\":\"index_claims\"}]", c, KB_CURATOR_MAX_CUSTOM,
                   &nrej) == 0);
   }

   /* 10. Re-laning (a lane != the base op's native lane) is rejected in v1. */
   {
      size_t n = parse("[{\"name\":\"a\",\"base_op\":\"index_claims\",\"lane\":\"llm\"}]", c,
                       KB_CURATOR_MAX_CUSTOM, &nrej);
      assert(n == 0 && nrej == 1);
   }

   /* 11. Restating the native lane is accepted (forward-compat, no re-lane). */
   {
      size_t n = parse("[{\"name\":\"a\",\"base_op\":\"index_claims\",\"lane\":\"index\"}]", c,
                       KB_CURATOR_MAX_CUSTOM, &nrej);
      assert(n == 1 && nrej == 0);
   }

   /* 12. Unknown fields are ignored (forward-compat). */
   {
      size_t n = parse("[{\"name\":\"a\",\"base_op\":\"index_claims\",\"future\":42}]", c,
                       KB_CURATOR_MAX_CUSTOM, &nrej);
      assert(n == 1 && nrej == 0);
   }

   /* 13. Malformed / non-array JSON => nothing composed, input flagged. */
   {
      assert(parse("not json", c, KB_CURATOR_MAX_CUSTOM, &nrej) == 0 && nrej == 1);
      assert(parse("{\"name\":\"a\"}", c, KB_CURATOR_MAX_CUSTOM, &nrej) == 0 && nrej == 1);
   }

   /* 14. Empty / null input => nothing, not an error. */
   {
      assert(parse("", c, KB_CURATOR_MAX_CUSTOM, &nrej) == 0 && nrej == 0);
      assert(parse("[]", c, KB_CURATOR_MAX_CUSTOM, &nrej) == 0 && nrej == 0);
   }

   /* 15. Entries beyond `max` are rejected, not silently dropped. */
   {
      size_t n = parse("[{\"name\":\"a\",\"base_op\":\"index_claims\"},"
                       "{\"name\":\"b\",\"base_op\":\"index_claims\"},"
                       "{\"name\":\"cc\",\"base_op\":\"index_claims\"}]",
                       c, 2, &nrej);
      assert(n == 2 && nrej == 1);
   }

   printf("curator_custom_stages: all tests passed\n");
   return 0;
}
