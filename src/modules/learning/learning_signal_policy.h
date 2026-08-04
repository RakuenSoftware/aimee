#ifndef AIMEE_LEARNING_SIGNAL_POLICY_H
#define AIMEE_LEARNING_SIGNAL_POLICY_H 1

#include <stdint.h>

/* Pure process-side policy shared by the C parity handler and focused tests. */
int learning_signal_policy_sink_mask(const char *signal, uint32_t *sink_mask);

#endif
