#ifndef DEC_KB_CLIENT_PII_H
#define DEC_KB_CLIENT_PII_H 1

/* The aimee-server -> aimee-kb PII screen.
 *
 * The boundary rule is that free text authored in a session must not be
 * persisted into aimee-kb carrying a secret or a PII span. The screen runs
 * CLIENT-SIDE, before any request is issued: a gate that runs after the POST
 * has already moved the data it exists to contain, and "aimee-kb rejected it"
 * still means aimee-kb received it.
 *
 * These helpers live in their own translation unit because the same screen has
 * to guard every content-carrying write, not just the memory ones. Thirteen
 * request builders across six files persist session-authored prose; a screen
 * pasted into each of them is a screen that the fourteenth will omit.
 *
 * Scope: PERSISTED writes. Query paths (memory.search, notes.search,
 * memory.prospective_match) send text that aimee-kb matches and discards, and
 * screening them would redact the search terms out of a user's own query.
 * Widening this to queries is a separate decision, not an oversight.
 */

#include "cJSON.h"

/* 0 = `text` may be sent as-is (*out NULL), or a redacted form must be sent
 * instead (*out = heap copy the caller frees). -1 = the text must not leave
 * this process. An allocation failure returns -1: unable to classify is
 * unable to send. */
int kb_client_pii_screen(const char *text, char **out);

/* An identifier -- a lookup key, not prose. Redacting one in place would
 * silently change which record the caller is addressing, which is worse than
 * refusing, so ANY sensitivity withholds the write whether redactable or not.
 * Returns non-zero when the whole write must be withheld. */
int kb_client_pii_identifier_sensitive(const char *ident);

/* Add `text` to `obj` under `field`, redacted if it needs redacting. Returns
 * -1 without touching `obj` when the value must be withheld, so the caller's
 * one check is `if (... != 0) return withheld;` and the request it would have
 * built is never issued.
 *
 * The plain form skips a NULL or empty value, matching the `if (x && x[0])`
 * guard optional fields already used. The _required form adds the field even
 * when empty, preserving the wire shape of builders that always emit it. */
int kb_client_pii_add_string(cJSON *obj, const char *field, const char *text);
int kb_client_pii_add_string_required(cJSON *obj, const char *field, const char *text);

/* The refusal envelope for the wrappers that return a response document rather
 * than a status code. Shaped like any other kb error so a caller that parses
 * `status` sees a definite refusal instead of a NULL it would read as "the
 * knowledge service is down". Heap-allocated; the caller frees. */
char *kb_client_pii_withheld_json(void);

#endif /* DEC_KB_CLIENT_PII_H */
