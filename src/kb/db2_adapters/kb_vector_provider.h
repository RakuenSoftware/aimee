#ifndef AIMEE_KB_VECTOR_PROVIDER_H
#define AIMEE_KB_VECTOR_PROVIDER_H 1

/* Subscribe to vector provider CAPABILITIES announcements and deliver them to
 * db2's memory route, so an attached provider can be selected.
 *
 * Returns 0, or -1 if the kind is already observed or the bus cannot take
 * another observer. Not fatal to a KB: without it, vector search stays on
 * pgvector, which is what every deployment without a provider does anyway. */
int kb_vector_provider_observe(void);

/* Observe, and log if it did not work. What a daemon calls at startup: the
 * decision that this is non-fatal belongs with the subsystem that knows why,
 * not at the call site. */
void kb_vector_provider_start(void);

#endif
