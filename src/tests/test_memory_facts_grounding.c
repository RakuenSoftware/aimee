/* test_memory_facts_grounding.c: the grounding gate on typed-fact extraction.
 *
 * A fact commits only if both endpoints trace back to the note. This replaced a
 * floor on the model's self-reported confidence, which measured across 18
 * extraction models turned out to carry almost no signal — several write 0.0 for
 * every fact including the correct ones, so the floor silently discarded
 * everything they extracted.
 *
 * The static helpers are exercised by including the translation unit, matching
 * the pattern in test_server_compute.c. */
#include "kb/fact_grounding.c"

#include <assert.h>
#include <stdio.h>

static void test_norm_text(void)
{
   char out[256];
   fact_norm_text("The KB server has hostname aimee-kb!", out, sizeof(out));
   assert(strcmp(out, "the kb server has hostname aimee kb") == 0);
   /* Dots survive so an IP stays intact. */
   fact_norm_text("pve answers on 192.168.1.253.", out, sizeof(out));
   assert(strstr(out, "192.168.1.253") != NULL);
   fact_norm_text("", out, sizeof(out));
   assert(out[0] == '\0');
   fact_norm_text(NULL, out, sizeof(out));
   assert(out[0] == '\0');
   printf("  PASS: test_norm_text\n");
}

static void test_grounded(void)
{
   char note[512];
   fact_norm_text("The KB server has hostname aimee-kb and IP 10.20.0.15.", note, sizeof(note));

   assert(fact_grounded("KB server", note) == 1);
   assert(fact_grounded("kb_server", note) == 1); /* snake_case meets "KB server" */
   assert(fact_grounded("aimee-kb", note) == 1);
   assert(fact_grounded("10.20.0.15", note) == 1);
   /* An entity the note never mentions is exactly what this gate is for. */
   assert(fact_grounded("Rakuen Software", note) == 0);
   assert(fact_grounded("Jonathan Bailes", note) == 0);

   /* "user" is grounded by convention: the prompt instructs the model to use it
    * as the subject of a first-person note, so it will not appear literally. */
   fact_norm_text("I work for Rakuen Software as of this month.", note, sizeof(note));
   assert(fact_grounded("user", note) == 1);
   assert(fact_grounded("Rakuen Software", note) == 1);
   assert(fact_grounded("Siemens", note) == 0);

   /* Majority-of-words, so a longer rendering of a present entity still counts
    * while an invented one does not. */
   fact_norm_text("Ingrid mentors two of the junior engineers.", note, sizeof(note));
   assert(fact_grounded("two of the junior engineers", note) == 1);
   assert(fact_grounded("junior engineers", note) == 1);
   assert(fact_grounded("senior architects", note) == 0);

   assert(fact_grounded("", note) == 1); /* empty is filtered earlier, not here */
   printf("  PASS: test_grounded\n");
}

int main(void)
{
   test_norm_text();
   test_grounded();
   printf("memory_facts grounding: all tests passed\n");
   return 0;
}
