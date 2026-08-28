#ifndef AIMEE_EGRESS_CREDENTIAL_ENVELOPE_H
#define AIMEE_EGRESS_CREDENTIAL_ENVELOPE_H

#include "cJSON.h"

/* Bus client for the egress credential-key stage. It wraps one current forge
 * bearer for a single operation and owner/repo; callers receive ciphertext and
 * authenticated scope, never an egress-owned decryption key. */
cJSON *aimee_egress_wrap_forge_credential(const char *token, const char *operation,
                                          const char *resource);

#endif
