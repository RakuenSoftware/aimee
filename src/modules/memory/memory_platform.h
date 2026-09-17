/* memory_platform.h: private declarations for platform-owned memory gates. */
#ifndef DEC_MEMORY_PLATFORM_H
#define DEC_MEMORY_PLATFORM_H 1

#include "aimee.h"

/* Sensitivity gate: detect secrets, API keys, PII.
 * Returns 0=clean, 1=redactable (redacted content written to redacted/redacted_cap),
 * 2=sensitive but not safely redactable (reject). */
int gate_check_sensitive(const char *content, char *redacted, size_t redacted_cap);

#endif /* DEC_MEMORY_PLATFORM_H */
