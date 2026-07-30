#ifndef DEC_EMBED_INPUT_TYPE_H
#define DEC_EMBED_INPUT_TYPE_H 1

/* Which side of an asymmetric retrieval embedder a text belongs to.
 *
 * Modern embedders are trained with a distinct prefix per side, and the prefixes live
 * with every other per-model fact in the embedder registry (scripts/embedders.json,
 * applied by the gateway) — callers only DECLARE polarity, they never spell a prefix,
 * so the registry stays the single source of truth. Serving nomic prefix-free measured
 * 0.5823 NDCG@10 against 0.6075 prefixed, which is why this is a required argument of
 * memory_embed_text rather than an option: the compiler makes every new embed call site
 * state which side it is on, and getting it wrong is invisible at runtime (the vectors
 * stay well-formed).
 *
 * The builtin lexical embedder has no prefixes and ignores this.
 *
 * This lives in its own header, not in aimee.h, so that a caller (notably a test that
 * stubs memory_embed_text and declares its own opaque memory types) can name the
 * polarity without pulling in the whole memory API. aimee.h includes it, so every
 * existing include site is unaffected. */
typedef enum
{
   EMBED_INPUT_DOCUMENT = 0, /* indexed content: chunks, memories, code units, claims */
   EMBED_INPUT_QUERY = 1     /* a search string being matched against those documents */
} embed_input_type_t;

#endif /* DEC_EMBED_INPUT_TYPE_H */
