/* fact_grounding.h: is an extracted fact actually present in its source note?
 *
 * The typed-fact drain used to gate commits on the model's self-reported
 * confidence. Measured across 18 extraction models on the Tier-A benchmark, that
 * number carries almost no signal: most write exactly 0.0 or exactly 0.9 and
 * nothing between, several write 0.0 for every fact including the ones they get
 * right, and for one model the low-confidence facts are MORE accurate than the
 * confident ones. A fixed floor silently discarded everything four models
 * extracted.
 *
 * This checks the text instead of the model's opinion of itself: a fact commits
 * only if both endpoints trace back to the note. No calibration, identical
 * behaviour for every provider, and it catches the failure that matters for
 * something auto-injected into every turn — an invented entity, which carries a
 * confidence of 0.9 as happily as a real one.
 *
 * Pure and DB-free, so it unit-tests without libpq. */
#ifndef DEC_FACT_GROUNDING_H
#define DEC_FACT_GROUNDING_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Lowercase, punctuation to spaces, runs collapsed; dots and colons kept so
    * IPs and hostnames survive. Underscores and hyphens become spaces, so a
    * model writing kb_server matches a note saying "KB server". */
   void fact_norm_text(const char *in, char *out, size_t cap);

   /* Is `value` traceable to `note_norm` (already passed through
    * fact_norm_text)? Whole-string match, else a majority of its content words
    * must appear. "user"/"i"/"me" are grounded by convention — the extraction
    * prompt instructs the model to use "user" as the subject of a first-person
    * note, so it will never appear literally. */
   int fact_grounded(const char *value, const char *note_norm);

#ifdef __cplusplus
}
#endif

#endif /* DEC_FACT_GROUNDING_H */
