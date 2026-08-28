#ifndef AIMEE_KB_MGMT_OFFLINE_HARDENING_H
#define AIMEE_KB_MGMT_OFFLINE_HARDENING_H

#include <stddef.h>

/* Harden an offline custody process before it reads configuration or opens its
 * database. Returns NULL on success or a fixed, non-secret error class. */
const char *kb_mgmt_offline_harden_process(void);

/* Parse the bounded contents of /proc/swaps. Exposed only so the fail-closed
 * no-swap fallback has deterministic unit coverage. Returns 0 for no active
 * swap, 1 for at least one active swap, and -1 for malformed input. */
int kb_mgmt_offline_swaps_text_active(const char *text, size_t len);

/* Parse cgroup v2's memory.swap.max. Returns 1 only for the exact disabled
 * value (zero, optionally followed by whitespace); every other value fails
 * closed. Exposed for deterministic parser coverage. */
int kb_mgmt_offline_cgroup_swap_text_disabled(const char *text, size_t len);

#endif
