/* Wire contract for the DB1 process's bounded stages.
 *
 * GENERATED from src/modules/db1/eventcontract/operations.json by
 * scripts/gen_db1_contract.py. Do not edit: add a family or an operation to the
 * catalog and regenerate, so the numbering and the wire cannot drift apart.
 *
 * DB1 is the server's SQLite store. It is becoming a module so that callers
 * reach it over the event bus instead of linking it, which is what the module
 * doctrine requires of state. The C implementation stays for now; only the
 * boundary is new. See docs/proposals/pending/db1-as-a-go-module.md.
 *
 * Event kinds are fixed by the process contract at 4096 + ref*256 + stage. DB1
 * declares principal ref 30, so these are not a free choice. */
#ifndef AIMEE_DB1_MODULE_API_H
#define AIMEE_DB1_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>

/* Family 1: the economizer's per-conversation reducer state. Chosen as the
 * first family because it has exactly one production caller, so it proved the
 * boundary without a wide cutover.
 *
 * Request:  op(u32) | key_len(u32) | key | json_len(u32) | json
 * Response: status(u32) | json_len(u32) | json
 * Lengths are little-endian, matching the rest of the bus surface. */

#define AIMEE_DB1_EVENT_ECONOMIZER_STATE 11777u
#define AIMEE_DB1_STAGE_ECONOMIZER_STATE 1u

#define AIMEE_DB1_OP_STATE_LOAD 1u
#define AIMEE_DB1_OP_STATE_SAVE 2u

/* Family 2: branch ownership for the MCP git flows. Rows say which session
 * owns which branch, so concurrent local sessions do not stomp on each other.
 *
 * Request:  op(u32) | field_count(u32) | (len(u32) | bytes) * field_count
 * Response: status(u32) | field_count(u32) | (len(u32) | bytes) * field_count
 *
 * Counted in both directions. The first family fixed its request at exactly two
 * fields, which suits a keyed blob and suits nothing with three, so the count is
 * explicit here rather than implied by the op.
 *
 * The reply counts for the same reason the request does: an operation that
 * answers with a row, or with a list of them, has somewhere to put the values.
 * A reply carrying nothing sends a count of zero, and one carrying a single
 * value sends a count of one -- the shape does not change with the arity. */

#define AIMEE_DB1_EVENT_GIT_OWNERSHIP 11778u
#define AIMEE_DB1_STAGE_GIT_OWNERSHIP 2u

#define AIMEE_DB1_OP_OWNERSHIP_UPSERT             1u
#define AIMEE_DB1_OP_OWNERSHIP_DELETE             2u
#define AIMEE_DB1_OP_OWNERSHIP_OWNER_GET          3u
#define AIMEE_DB1_OP_OWNERSHIP_BRANCH_FOR_SESSION 4u
#define AIMEE_DB1_OP_OWNERSHIP_SESSION_BY_PREFIX  5u
#define AIMEE_DB1_OP_FEATURE_BRANCH_UPSERT        6u
#define AIMEE_DB1_OP_FEATURE_BRANCH_GET           7u

/* Family 3: per-conversation context, clarifications and working memory. */

#define AIMEE_DB1_EVENT_CONVERSATION 11779u
#define AIMEE_DB1_STAGE_CONVERSATION 3u

#define AIMEE_DB1_OP_REWRITE_STATE_GET 1u
#define AIMEE_DB1_OP_REWRITE_STATE_SET 2u

/* Wire bounds, carried from the catalog. VALUE_MAX is the widest
   reply a stage may build; FIELDS_MAX is the widest request arity, and
   sizes the decoder's pointer array. Requests are NOT capped: they carry
   prompts and documents, an in-process caller passes those whole, and the
   frame already bounds what arrived. */
#define AIMEE_DB1_STATE_MAX  6144u
#define AIMEE_DB1_VALUE_MAX  512u
#define AIMEE_DB1_FIELDS_MAX 11u

#define AIMEE_DB1_STATUS_OK       0u
#define AIMEE_DB1_STATUS_MISSING  1u
#define AIMEE_DB1_STATUS_INVALID  2u
#define AIMEE_DB1_STATUS_TOO_LONG 3u
#define AIMEE_DB1_STATUS_FAILED   4u

static inline void aimee_db1_put_u32(uint8_t *p, uint32_t value)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(value >> (i * 8u));
}

static inline uint32_t aimee_db1_get_u32(const uint8_t *p)
{
   uint32_t value = 0;
   for (unsigned i = 0; i < 4; ++i)
      value |= (uint32_t)p[i] << (i * 8u);
   return value;
}

#endif /* AIMEE_DB1_MODULE_API_H */
