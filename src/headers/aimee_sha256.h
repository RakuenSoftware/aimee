/* aimee_sha256.h: the tree's shared SHA-256 primitive.
 *
 * A hash function is substrate, not a module service: it has no state, no
 * policy, and nothing to route. Before this existed the only implementation in
 * the tree was wfe_sha256_raw() inside the workflows module, so every other
 * consumer had to reach across a module boundary for it — audit (a core module)
 * included workflows' private wfe_def.h purely to hash bytes.
 *
 * Backed by OpenSSL, which both aimee-server and aimee-kb already link
 * (L_SERVER / L_KB carry -lssl -lcrypto), rather than a hand-rolled block
 * function.
 *
 * NULL data hashes the empty input, matching the wfe_sha256_raw() contract this
 * replaces. Returns 0 on success, -1 when out is NULL.
 */
#ifndef AIMEE_SHA256_H
#define AIMEE_SHA256_H

#include <stddef.h>

/* Raw 32-byte digest. */
int aimee_sha256_raw(const void *data, size_t len, unsigned char out[32]);

/* Lowercase hex digest, NUL-terminated: 64 chars + NUL. */
int aimee_sha256_hex(const void *data, size_t len, char out_hex[65]);

#endif /* AIMEE_SHA256_H */
