/* Versioned wire contract for the first DB2 C-process lifecycle operation. */
#ifndef AIMEE_DB2_MODULE_API_H
#define AIMEE_DB2_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>

#define AIMEE_DB2_EVENT_HEALTH   11521u
#define AIMEE_DB2_STAGE_HEALTH   1u
#define AIMEE_DB2_REQUEST_MAGIC  0x51483244u /* "D2HQ", little-endian */
#define AIMEE_DB2_RESPONSE_MAGIC 0x52483244u /* "D2HR", little-endian */
#define AIMEE_DB2_WIRE_VERSION   1u
#define AIMEE_DB2_REQUEST_LEN    8u
#define AIMEE_DB2_RESPONSE_LEN   16u

#define AIMEE_DB2_FLAG_SCHEMA    0x1u
#define AIMEE_DB2_FLAG_PG_TRGM   0x2u
#define AIMEE_DB2_FLAG_KB_TABLES 0x4u
#define AIMEE_DB2_FLAG_ALL       0x7u

static inline void aimee_db2_put_u32(uint8_t *output, uint32_t value)
{
   for (unsigned index = 0; index < 4; ++index)
      output[index] = (uint8_t)(value >> (index * 8u));
}

static inline uint32_t aimee_db2_get_u32(const uint8_t *input)
{
   uint32_t value = 0;
   for (unsigned index = 0; index < 4; ++index)
      value |= (uint32_t)input[index] << (index * 8u);
   return value;
}

static inline int aimee_db2_health_request_encode(uint8_t *output, size_t capacity)
{
   if (!output || capacity < AIMEE_DB2_REQUEST_LEN)
      return -1;
   aimee_db2_put_u32(output, AIMEE_DB2_REQUEST_MAGIC);
   aimee_db2_put_u32(output + 4, AIMEE_DB2_WIRE_VERSION);
   return 0;
}

static inline int aimee_db2_health_request_decode(const uint8_t *input, size_t input_len)
{
   return input && input_len == AIMEE_DB2_REQUEST_LEN &&
                  aimee_db2_get_u32(input) == AIMEE_DB2_REQUEST_MAGIC &&
                  aimee_db2_get_u32(input + 4) == AIMEE_DB2_WIRE_VERSION
              ? 0
              : -1;
}

static inline int aimee_db2_health_response_encode(uint32_t flags, uint8_t *output, size_t capacity)
{
   if (!output || capacity < AIMEE_DB2_RESPONSE_LEN || (flags & ~AIMEE_DB2_FLAG_ALL) != 0)
      return -1;
   aimee_db2_put_u32(output, AIMEE_DB2_RESPONSE_MAGIC);
   aimee_db2_put_u32(output + 4, AIMEE_DB2_WIRE_VERSION);
   aimee_db2_put_u32(output + 8, flags);
   aimee_db2_put_u32(output + 12, 0u);
   return 0;
}

static inline int aimee_db2_health_response_decode(const uint8_t *input, size_t input_len,
                                                   int *schema_ok, int *have_pg_trgm,
                                                   int *kb_tables_ok)
{
   if (schema_ok)
      *schema_ok = 0;
   if (have_pg_trgm)
      *have_pg_trgm = 0;
   if (kb_tables_ok)
      *kb_tables_ok = 0;
   if (!input || input_len != AIMEE_DB2_RESPONSE_LEN ||
       aimee_db2_get_u32(input) != AIMEE_DB2_RESPONSE_MAGIC ||
       aimee_db2_get_u32(input + 4) != AIMEE_DB2_WIRE_VERSION ||
       aimee_db2_get_u32(input + 12) != 0u)
      return -1;
   uint32_t flags = aimee_db2_get_u32(input + 8);
   if ((flags & ~AIMEE_DB2_FLAG_ALL) != 0)
      return -1;
   if (schema_ok)
      *schema_ok = (flags & AIMEE_DB2_FLAG_SCHEMA) != 0;
   if (have_pg_trgm)
      *have_pg_trgm = (flags & AIMEE_DB2_FLAG_PG_TRGM) != 0;
   if (kb_tables_ok)
      *kb_tables_ok = (flags & AIMEE_DB2_FLAG_KB_TABLES) != 0;
   return 0;
}

#endif /* AIMEE_DB2_MODULE_API_H */
