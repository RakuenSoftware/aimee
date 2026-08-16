/* Wire contract for the DB1 process's bounded stages.
 *
 * DB1 is the server's SQLite store. It is becoming a module so that callers
 * reach it over the event bus instead of linking it, which is what the module
 * doctrine requires of state. The C implementation stays for now; only the
 * boundary is new. See docs/proposals/pending/db1-as-a-go-module.md.
 *
 * Event kinds are fixed by the process contract at 4096 + ref*256 + stage. DB1
 * declares principal ref 29, so these are not a free choice. */
#ifndef AIMEE_DB1_MODULE_API_H
#define AIMEE_DB1_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>

/* Stage 1: the economizer's per-conversation reducer state. Chosen as the first
 * stage because it has exactly one production caller, so it proves the boundary
 * without a wide cutover. */
#define AIMEE_DB1_EVENT_ECONOMIZER_STATE 11777u
#define AIMEE_DB1_STAGE_ECONOMIZER_STATE 1u

/* Request:  op(u32) | key_len(u32) | key | json_len(u32) | json
   Response: status(u32) | json_len(u32) | json
   Lengths are little-endian, matching the rest of the bus surface. */
#define AIMEE_DB1_OP_STATE_LOAD 1u
#define AIMEE_DB1_OP_STATE_SAVE 2u

/* A reducer state blob is bounded by the caller's buffer today
   (ECON_MODULE_STATE_MAX). The wire cap is stated here so the module can refuse
   an over-long value rather than truncate one. */
#define AIMEE_DB1_STATE_MAX 6144u

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
