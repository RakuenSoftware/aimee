/* test_fold_register.c: unit tests for the register grammar (fold §6, P3). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "fold_register.h"

#define PASS(name) printf("  PASS: %s\n", name)

static void test_glyphs(void)
{
   assert(fold_register_parse("\xF0\x9F\x8F\x81 done it") == FOLD_REG_VERDICT);     /* 🏁 */
   assert(fold_register_parse("\xE2\x96\xB6 running") == FOLD_REG_EXECUTING);       /* ▶ */
   assert(fold_register_parse("\xE2\x9A\xA0 careful") == FOLD_REG_HAZARD);          /* ⚠ */
   assert(fold_register_parse("\xE2\x9D\x93 stuck") == FOLD_REG_BLOCKED);           /* ❓ */
   assert(fold_register_parse("\xF0\x9F\x94\x8D looking") == FOLD_REG_IN_PROGRESS); /* 🔍 */
   PASS("glyphs");
}

static void test_bracket_tags(void)
{
   assert(fold_register_parse("[verdict] the fix works") == FOLD_REG_VERDICT);
   assert(fold_register_parse("  [DONE] ok") == FOLD_REG_VERDICT); /* ws + case-insensitive */
   assert(fold_register_parse("[hazard] careful") == FOLD_REG_HAZARD);
   assert(fold_register_parse("[warning] x") == FOLD_REG_HAZARD);
   assert(fold_register_parse("[exec] building") == FOLD_REG_EXECUTING);
   assert(fold_register_parse("[blocked] need input") == FOLD_REG_BLOCKED);
   assert(fold_register_parse("[wip] thinking") == FOLD_REG_IN_PROGRESS);
   PASS("bracket_tags");
}

static void test_defaults_and_settled(void)
{
   assert(fold_register_parse(NULL) == FOLD_REG_IN_PROGRESS);
   assert(fold_register_parse("") == FOLD_REG_IN_PROGRESS);
   assert(fold_register_parse("just some prose") == FOLD_REG_IN_PROGRESS);
   assert(fold_register_is_settled(FOLD_REG_VERDICT) == 1);
   assert(fold_register_is_settled(FOLD_REG_HAZARD) == 1);
   assert(fold_register_is_settled(FOLD_REG_IN_PROGRESS) == 0);
   assert(fold_register_is_settled(FOLD_REG_EXECUTING) == 0);
   assert(strcmp(fold_register_label(FOLD_REG_VERDICT), "verdict") == 0);
   PASS("defaults_and_settled");
}

static void test_bracket_anchor(void)
{
   /* only exact, closing-bracket-anchored tags match — no prefix false-positives */
   assert(fold_register_parse("[executable] running") == FOLD_REG_IN_PROGRESS);
   assert(fold_register_parse("[verdicts] plural") == FOLD_REG_IN_PROGRESS);
   assert(fold_register_parse("[doner] kebab") == FOLD_REG_IN_PROGRESS);
   assert(fold_register_parse("[exec] x") == FOLD_REG_EXECUTING);      /* exact still matches */
   assert(fold_register_parse("[executing] x") == FOLD_REG_EXECUTING); /* exact still matches */
   PASS("bracket_anchor");
}

static void test_short_inputs_safe(void)
{
   /* Truncated glyph prefixes and partial bracket tags must not read past the
    * buffer; all classify as in-progress. */
   assert(fold_register_parse("\xF0") == FOLD_REG_IN_PROGRESS);     /* lone 4-byte lead */
   assert(fold_register_parse("\xF0\x9F") == FOLD_REG_IN_PROGRESS); /* partial 4-byte */
   assert(fold_register_parse("\xE2") == FOLD_REG_IN_PROGRESS);     /* lone 3-byte lead */
   assert(fold_register_parse("\xE2\x96") == FOLD_REG_IN_PROGRESS); /* partial 3-byte */
   assert(fold_register_parse("[") == FOLD_REG_IN_PROGRESS);        /* bare bracket */
   assert(fold_register_parse("[ver") == FOLD_REG_IN_PROGRESS);     /* partial tag */
   assert(fold_register_parse(" ") == FOLD_REG_IN_PROGRESS);        /* whitespace only */
   PASS("short_inputs_safe");
}

int main(void)
{
   printf("fold_register tests:\n");
   test_glyphs();
   test_bracket_tags();
   test_defaults_and_settled();
   test_bracket_anchor();
   test_short_inputs_safe();
   printf("ALL PASS\n");
   return 0;
}
