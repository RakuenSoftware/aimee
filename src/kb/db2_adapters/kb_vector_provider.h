#ifndef AIMEE_KB_VECTOR_PROVIDER_H
#define AIMEE_KB_VECTOR_PROVIDER_H 1

/* Subscribe to vector provider CAPABILITIES announcements and deliver them to
 * db2's memory route, so an attached provider can be selected.
 *
 * Returns 0, or -1 if the kind is already observed or the bus cannot take
 * another observer. Not fatal to a KB: without it, vector search stays on
 * pgvector, which is what every deployment without a provider does anyway. */
int kb_vector_provider_observe(void);

/* Install the KB's bus as the transport a selected provider is reached through.
 *
 * Separate from observing because they fail separately and mean different
 * things: without the observer nothing is ever selected, and without this a
 * selection exists and changes no query's answer.
 *
 * Returns 0, or -1 if db2 already has a transport or could not take one. */
int kb_vector_provider_install_transport(void);

/* Observe, install the transport, and log whichever did not work. What a daemon
 * calls at startup: the decision that these are non-fatal belongs with the
 * subsystem that knows why, not at the call site. */
void kb_vector_provider_start(void);

#endif
