/* Panel resolution for roundtable.review, published as module API.
 *
 * The review bus lives in src/server, which must not reach into the optional
 * roundtable module's private headers. Resolving "which saved panel does this
 * request mean, and how long may it run" is roundtable's business, not the
 * transport's, so the whole decision sits behind this narrow surface and no
 * optional-module type crosses it.
 */
#ifndef AIMEE_ROUNDTABLE_REVIEW_PANEL_H
#define AIMEE_ROUNDTABLE_REVIEW_PANEL_H 1

#include <limits.h>
#include <stddef.h>

#define ROUNDTABLE_REVIEW_DEFAULT_DEADLINE_MS 600000
#define ROUNDTABLE_REVIEW_GRACE_MS            30000

/* Buffer a caller must provide for a resolved panel name. Published here so the
 * bus does not have to include the preset store's private header; the
 * implementation static-asserts it against that store's own limit. */
#define ROUNDTABLE_REVIEW_PANEL_NAME_MAX      64

/* A configured chairman gets its own full phase deadline, so the call has to
 * cover analysis plus chairman, followed by a small serialization grace. Sharing
 * one phase deadline starved the chairman to nothing whenever the seats ran
 * long, and it failed on the request that launches its own turn. */
static inline int roundtable_review_deadline_ms(int deadline_ms, int chairman_enabled)
{
   if (deadline_ms <= 0)
      deadline_ms = ROUNDTABLE_REVIEW_DEFAULT_DEADLINE_MS;
   long long phases = chairman_enabled ? 2 : 1;
   long long timeout = (long long)deadline_ms * phases + ROUNDTABLE_REVIEW_GRACE_MS;
   return timeout > INT_MAX ? INT_MAX : (int)timeout;
}

/* Resolve the C-configured default panel so the MCP schema's documented default
 * can reach Go, which deliberately requires a named saved panel.
 *
 * requested_preset may be NULL. out must be non-NULL with out_len > 0 and
 * receives the resolved panel name, or an empty string when nothing resolved.
 * timeout_ms must be non-NULL and always receives a usable deadline, including
 * on every failure path. Returns 1 when a panel was resolved, 0 when none was. */
int roundtable_review_resolve_panel(const char *requested_preset, char *out, size_t out_len,
                                    int *timeout_ms);

#endif
