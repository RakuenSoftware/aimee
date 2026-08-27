#ifndef AIMEE_ARTIFACT_TRUST_H
#define AIMEE_ARTIFACT_TRUST_H 1

#include <stddef.h>

/* Verify an executable instruction artifact. Standard mode pins the first
 * observed digest and rejects later changes; hardened mode requires the exact
 * digest in an offline Ed25519-signed operator manifest. */
int artifact_trust_verify_bytes(const char *artifact_class, const char *artifact_id,
                                const char *canonical_path, const void *bytes, size_t len,
                                char digest_hex[65], char *err, size_t errlen);

/* Symlink/hardlink-safe bounded read which returns exactly the bytes that were
 * verified, closing the verification/use race at parsers. Caller frees *out. */
int artifact_trust_read_file(const char *artifact_class, const char *artifact_id, const char *path,
                             size_t max_len, char **out, size_t *out_len, char digest_hex[65],
                             char *err, size_t errlen);

#endif
