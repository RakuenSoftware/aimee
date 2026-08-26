/* db2/fact_identity.h: normalized identity for typed-fact assertions.
 *
 * An assertion's *identity* and its *presentation* are different things. The
 * literal source/relation/target text is what a person reads and what the audit
 * must preserve; the normalized key is what rejection, supersession and
 * deduplication bind to.
 *
 * Why this exists: the exact-value lookup used to match the raw triple text, so
 * ("staging", "region", "eu-west-1") and ("Staging", "region", "EU West 1") were
 * two different facts. Rejecting the first left the second free to be inserted
 * clean, carrying none of the rejection and no prior version. The realistic
 * trigger is not an adversary -- it is the extractor emitting the same claim
 * with a different surface form on the next pass, which is the ordinary case,
 * not the edge case.
 */
#ifndef DEC_DB2_FACT_IDENTITY_H
#define DEC_DB2_FACT_IDENTITY_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Bound for a normalized triple key. Sized for the concatenation of a
 * normalized subject, relation and object plus separators. */
#define FACT_IDENTITY_KEY_MAX 1024

   /* Normalize one component of an assertion (a subject or an object) into
    * `out`.
    *
    * What it does: Unicode NFKC, full case folding, then trimming and collapse
    * of Unicode whitespace runs to a single ASCII space.
    *
    * What it deliberately does NOT do: filter character classes. Every byte that
    * is not whitespace survives. This is the important half. An implementation
    * that keeps only ASCII alphanumerics reduces a non-Latin value to the empty
    * string, and then every such fact shares one key and they all collide into
    * each other -- a far worse failure than the one the normalization was added
    * to fix, and one that unit tests never see unless they feed it non-ASCII
    * input. Multi-byte sequences are passed through untouched.
    *
    * There is also no minimum length below which normalization is skipped.
    * Short values -- dates, versions, identifiers, region names -- are exactly
    * the values that get corrected most often, so exempting them would exempt
    * the cases that matter.
    *
    * Returns the length written, or 0 for a NULL/empty input or a bad buffer. */
   size_t fact_identity_normalize_component(const char *in, char *out, size_t out_len);

   /* Build the full normalized identity key for one assertion.
    *
    * The relation goes through the existing relation-name normalizer so the
    * predicate is keyed the same way everywhere; subject and object go through
    * fact_identity_normalize_component above. Components are joined with a
    * separator that cannot appear in a normalized component, so ("a b", "c") and
    * ("a", "b c") cannot produce the same key.
    *
    * Returns the length written, or 0 when any component is missing/empty or the
    * buffer is too small (callers treat 0 as "no normalized key available" and
    * fall back to literal matching). */
   size_t fact_identity_key(const char *source, const char *relation, const char *target, char *out,
                            size_t out_len);

   /* Normalized (subject, predicate) key, without the object.
    *
    * This is what a functional-relation incumbent scan has to match on. A
    * functional relation holds one current object per subject, so correcting it
    * means superseding whatever is currently there -- and if that scan matches
    * literal subject text, a rephrased incumbent is simply not seen, and two
    * spellings of one functional fact end up both current at once. Keyed here
    * the same way as fact_identity_key so the two agree by construction.
    *
    * Returns the length written, or 0 when a component is missing/empty. */
   size_t fact_identity_subject_key(const char *source, const char *relation, char *out,
                                    size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_FACT_IDENTITY_H */
