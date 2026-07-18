/* memory_platform.h: private declarations for platform-owned memory gates. */
#ifndef DEC_MEMORY_PLATFORM_H
#define DEC_MEMORY_PLATFORM_H 1

#include "aimee.h"

/* Sensitivity gate: detect secrets, API keys, PII.
 * Returns 0=clean, 1=redactable (redacted content written to redacted/redacted_cap),
 * 2=sensitive but not safely redactable (reject). */
int gate_check_sensitive(const char *content, char *redacted, size_t redacted_cap);

/* Stability gate: returns 1 if content is ephemeral (should not persist at L1+). */
int gate_check_ephemeral(const char *content);

/* Source gate: returns 1 if content contains evidence markers. */
int gate_has_evidence_markers(const char *content);

/* Background embedding: POSIX forks a child to run memory_embed; Windows is a no-op.
 * Only supported when the active DB can be reopened in the child. Otherwise,
 * foreground embedding still runs. */
void platform_memory_background_embed(int64_t memory_id, const char *command);

/* Per-thread suppression for paths that require deterministic single-writer behavior.
 * Returns the previous suppression state. */
int platform_memory_background_embed_set_suppressed(int suppressed);

#endif /* DEC_MEMORY_PLATFORM_H */
