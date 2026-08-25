/* test_fact_identity.c: normalized assertion identity.
 *
 * These are the "normalization variants" assertions the correction-completeness
 * proposal requires: materially identical values must produce one key, and
 * materially different values must not. They drive the production normalizer
 * directly -- a harness that restates the folding rules proves only that the
 * restatement agrees with itself.
 *
 * The non-ASCII cases are the important ones. The documented way this pattern
 * fails elsewhere is a normalizer that keeps only ASCII alphanumerics: every
 * non-Latin value reduces to the empty string, so they all share one key and
 * collide into each other -- a far worse bug than the one normalization was
 * added to fix, and invisible to a suite that only ever feeds it ASCII. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "modules/db2/c/fact_identity.h"

static void key_of(const char *s, const char *r, const char *t, char *out, size_t n)
{
   size_t len = fact_identity_key(s, r, t, out, n);
   if (len == 0)
   {
      fprintf(stderr, "  fact_identity_key returned empty for (%s,%s,%s)\n", s ? s : "(null)",
              r ? r : "(null)", t ? t : "(null)");
      assert(0);
   }
}

static void assert_same(const char *s1, const char *r1, const char *t1, const char *s2,
                        const char *r2, const char *t2, const char *why)
{
   char a[FACT_IDENTITY_KEY_MAX], b[FACT_IDENTITY_KEY_MAX];
   key_of(s1, r1, t1, a, sizeof(a));
   key_of(s2, r2, t2, b, sizeof(b));
   if (strcmp(a, b) != 0)
   {
      fprintf(stderr, "  expected SAME key (%s): \"%s\" vs \"%s\"\n", why, a, b);
      assert(0);
   }
}

static void assert_differs(const char *s1, const char *r1, const char *t1, const char *s2,
                           const char *r2, const char *t2, const char *why)
{
   char a[FACT_IDENTITY_KEY_MAX], b[FACT_IDENTITY_KEY_MAX];
   key_of(s1, r1, t1, a, sizeof(a));
   key_of(s2, r2, t2, b, sizeof(b));
   if (strcmp(a, b) == 0)
   {
      fprintf(stderr, "  expected DIFFERENT keys (%s), both \"%s\"\n", why, a);
      assert(0);
   }
}

/* The case this whole mechanism exists for: an extractor re-emitting one claim
 * with a different surface form must not become a second fact. */
static void test_surface_variants_are_one_fact(void)
{
   assert_same("staging", "region", "eu-west-1", "Staging", "region", "EU-WEST-1", "case");
   assert_same("staging", "region", "eu-west-1", "  staging  ", "region", "eu-west-1 ", "padding");
   assert_same("staging", "region", "eu-west-1", "staging", "region", "eu-west-1", "identical");
   assert_same("the api server", "owned_by", "team a", "The   API   Server", "owned_by", "Team  A",
               "internal whitespace runs collapse");
   assert_same("staging", "worksFor", "acme", "staging", "works_for", "acme",
               "predicate goes through the shared relation normalizer");
   printf("  surface_variants_are_one_fact: ok\n");
}

/* Materially different values must stay different, or a tombstone silences
 * facts it was never meant to touch. */
static void test_distinct_facts_stay_distinct(void)
{
   assert_differs("staging", "region", "eu-west-1", "staging", "region", "eu-west-2", "object");
   assert_differs("staging", "region", "eu-west-1", "prod", "region", "eu-west-1", "subject");
   assert_differs("staging", "region", "eu-west-1", "staging", "zone", "eu-west-1", "predicate");
   /* The separator must not be forgeable by whitespace: otherwise ("a b","c")
    * and ("a","b c") would collapse into one key. */
   assert_differs("a b", "rel", "c", "a", "rel", "b c", "component boundary is not whitespace");
   printf("  distinct_facts_stay_distinct: ok\n");
}

/* Short values are exactly the ones corrected most often -- dates, versions,
 * identifiers, region names. A minimum-length exemption would exempt the cases
 * that matter, so there is not one. */
static void test_short_values_are_normalized(void)
{
   assert_same("a", "rel", "V2", "a", "rel", "v2", "two-character value still folds");
   assert_differs("a", "rel", "v2", "a", "rel", "v3", "and still discriminates");
   printf("  short_values_are_normalized: ok\n");
}

/* The collapse trap: non-Latin values must survive normalization as themselves,
 * not be filtered down to a shared empty key. */
static void test_non_ascii_is_preserved_not_collapsed(void)
{
   char a[FACT_IDENTITY_KEY_MAX], b[FACT_IDENTITY_KEY_MAX], c[FACT_IDENTITY_KEY_MAX];
   key_of("\xe6\x9d\xb1\xe4\xba\xac", "region", "\xe6\x97\xa5\xe6\x9c\xac", a, sizeof(a));
   key_of("\xd0\x9c\xd0\xbe\xd1\x81\xd0\xba\xd0\xb2\xd0\xb0", "region",
          "\xd0\xa0\xd0\xbe\xd1\x81\xd1\x81\xd0\xb8\xd1\x8f", b, sizeof(b));
   key_of("\xce\x91\xce\xb8\xce\xae\xce\xbd\xce\xb1", "region", "\xce\x95\xce\xbb\xce\xbb\xce\xac",
          c, sizeof(c));

   /* Three unrelated non-Latin facts, three distinct keys. If normalization
    * filtered non-ASCII these would all be "\x1fregion\x1f" and every non-Latin
    * fact in the store would be the same fact. */
   assert(strcmp(a, b) != 0);
   assert(strcmp(b, c) != 0);
   assert(strcmp(a, c) != 0);

   /* And the bytes are actually carried through, not silently dropped. */
   assert(strstr(a, "\xe6\x9d\xb1\xe4\xba\xac") != NULL);
   assert(strstr(b, "\xd0\xbc\xd0\xbe\xd1\x81\xd0\xba\xd0\xb2\xd0\xb0") != NULL);

   /* Surrounding whitespace still normalizes around a non-Latin value. */
   assert_same("\xe6\x9d\xb1\xe4\xba\xac", "region", "\xe6\x97\xa5\xe6\x9c\xac",
               "  \xe6\x9d\xb1\xe4\xba\xac ", "region", " \xe6\x97\xa5\xe6\x9c\xac  ",
               "trim applies to non-Latin values too");
   printf("  non_ascii_is_preserved_not_collapsed: ok\n");
}

static void test_unicode_nfkc_and_full_casefold(void)
{
   /* Full-width compatibility characters converge under NFKC. */
   assert_same("API", "rel", "v2", "\xef\xbc\xa1\xef\xbc\xb0\xef\xbc\xa9", "rel",
               "\xef\xbd\x96\xef\xbc\x92", "full-width compatibility forms");
   /* Composition is sequence-aware: precomposed and combining spellings are
    * one identity, not merely mappings applied one codepoint at a time. */
   assert_same("caf\xc3\xa9", "rel", "ok", "cafe\xcc\x81", "rel", "ok", "canonical composition");
   /* Full case folding includes expansions and non-ASCII pairs. */
   assert_same("Stra\xc3\x9f"
               "e",
               "rel", "x", "STRASSE", "rel", "x", "sharp-s full casefold");
   assert_same("\xce\x9f\xce\x94\xce\x9f\xce\xa3", "rel", "x", "\xce\xbf\xce\xb4\xce\xbf\xcf\x82",
               "rel", "x", "Greek casefold including final sigma");
   assert_same("\xef\xac\x83", "rel", "x", "ffi", "rel", "x", "compatibility ligature");
   printf("  unicode_nfkc_and_full_casefold: ok\n");
}

static void test_empty_and_missing_components(void)
{
   char out[FACT_IDENTITY_KEY_MAX];
   /* No key rather than a partial one: callers read 0 as "fall back to literal
    * matching", which is correct, whereas a partial key would alias rows. */
   assert(fact_identity_key(NULL, "rel", "t", out, sizeof(out)) == 0);
   assert(fact_identity_key("s", "rel", NULL, out, sizeof(out)) == 0);
   assert(fact_identity_key("", "rel", "t", out, sizeof(out)) == 0);
   assert(fact_identity_key("s", "rel", "   ", out, sizeof(out)) == 0);
   assert(fact_identity_key("s", "", "t", out, sizeof(out)) == 0);

   /* A buffer too small yields no key rather than a truncated one, since a
    * truncated key would collide unrelated facts. */
   char tiny[4];
   assert(fact_identity_key("subject", "relation", "object", tiny, sizeof(tiny)) == 0);
   assert(tiny[0] == '\0');
   printf("  empty_and_missing_components: ok\n");
}

/* The functional-relation incumbent scan keys on (subject, predicate), and must
 * agree with the full key on how those two are spelled. */
static void test_subject_key_agrees_with_full_key(void)
{
   char sk[FACT_IDENTITY_KEY_MAX], full[FACT_IDENTITY_KEY_MAX];
   assert(fact_identity_subject_key("The API Server", "worksFor", sk, sizeof(sk)) > 0);
   key_of("the   api server", "works_for", "anything", full, sizeof(full));
   /* The full key must begin with the subject key followed by the separator. */
   size_t n = strlen(sk);
   assert(strncmp(full, sk, n) == 0);
   assert(full[n] == '\x1f');

   char other[FACT_IDENTITY_KEY_MAX];
   assert(fact_identity_subject_key("the api server", "reports_to", other, sizeof(other)) > 0);
   assert(strcmp(sk, other) != 0);
   printf("  subject_key_agrees_with_full_key: ok\n");
}

int main(void)
{
   printf("test_fact_identity:\n");
   test_surface_variants_are_one_fact();
   test_distinct_facts_stay_distinct();
   test_short_values_are_normalized();
   test_non_ascii_is_preserved_not_collapsed();
   test_unicode_nfkc_and_full_casefold();
   test_empty_and_missing_components();
   test_subject_key_agrees_with_full_key();
   printf("all fact_identity tests passed\n");
   return 0;
}
