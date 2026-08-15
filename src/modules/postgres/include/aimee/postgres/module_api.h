/* Wire contract for the PostgreSQL process's bounded health evidence. */
#ifndef AIMEE_POSTGRES_MODULE_API_H
#define AIMEE_POSTGRES_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>

#define AIMEE_POSTGRES_EVENT_HEALTH   11265u
#define AIMEE_POSTGRES_STAGE_HEALTH   1u
#define AIMEE_POSTGRES_REQUEST_MAGIC  0x51484750u /* "PGHQ" */
#define AIMEE_POSTGRES_RESPONSE_MAGIC 0x52484750u /* "PGHR" */
#define AIMEE_POSTGRES_WIRE_VERSION   1u
#define AIMEE_POSTGRES_REQUEST_LEN    8u
#define AIMEE_POSTGRES_RESPONSE_LEN   16u
#define AIMEE_POSTGRES_FLAG_SCHEMA    0x1u
#define AIMEE_POSTGRES_FLAG_PG_TRGM   0x2u
#define AIMEE_POSTGRES_FLAG_KB_TABLES 0x4u

static inline void aimee_postgres_put_u32(uint8_t *p, uint32_t value)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(value >> (i * 8u));
}

static inline uint32_t aimee_postgres_get_u32(const uint8_t *p)
{
   uint32_t value = 0;
   for (unsigned i = 0; i < 4; ++i)
      value |= (uint32_t)p[i] << (i * 8u);
   return value;
}

static inline int aimee_postgres_health_request_encode(uint8_t *out, size_t capacity)
{
   if (!out || capacity < AIMEE_POSTGRES_REQUEST_LEN)
      return -1;
   aimee_postgres_put_u32(out, AIMEE_POSTGRES_REQUEST_MAGIC);
   aimee_postgres_put_u32(out + 4, AIMEE_POSTGRES_WIRE_VERSION);
   return 0;
}

static inline int aimee_postgres_health_response_decode(const uint8_t *response,
                                                        size_t response_len, int *schema_ok,
                                                        int *have_pg_trgm, int *kb_tables_ok)
{
   if (schema_ok)
      *schema_ok = 0;
   if (have_pg_trgm)
      *have_pg_trgm = 0;
   if (kb_tables_ok)
      *kb_tables_ok = 0;
   if (!response || response_len != AIMEE_POSTGRES_RESPONSE_LEN ||
       aimee_postgres_get_u32(response) != AIMEE_POSTGRES_RESPONSE_MAGIC ||
       aimee_postgres_get_u32(response + 4) != AIMEE_POSTGRES_WIRE_VERSION)
      return -1;
   uint32_t flags = aimee_postgres_get_u32(response + 8);
   if ((flags & ~(AIMEE_POSTGRES_FLAG_SCHEMA | AIMEE_POSTGRES_FLAG_PG_TRGM |
                  AIMEE_POSTGRES_FLAG_KB_TABLES)) != 0 ||
       aimee_postgres_get_u32(response + 12) != 0)
      return -1;
   if (schema_ok)
      *schema_ok = (flags & AIMEE_POSTGRES_FLAG_SCHEMA) != 0;
   if (have_pg_trgm)
      *have_pg_trgm = (flags & AIMEE_POSTGRES_FLAG_PG_TRGM) != 0;
   if (kb_tables_ok)
      *kb_tables_ok = (flags & AIMEE_POSTGRES_FLAG_KB_TABLES) != 0;
   return 0;
}

#endif
