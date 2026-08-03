/* Wire contract for the routing process's route-selection stage. */
#ifndef AIMEE_ROUTING_MODULE_API_H
#define AIMEE_ROUTING_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>

#define AIMEE_ROUTING_REQUEST_MAGIC  0x54554f52u /* "ROUT" little-endian */
#define AIMEE_ROUTING_RESPONSE_MAGIC 0x4c455352u /* "RSEL" little-endian */
#define AIMEE_ROUTING_WIRE_VERSION   1u
#define AIMEE_ROUTING_REQUEST_LEN    12u
#define AIMEE_ROUTING_RESPONSE_LEN   8u
#define AIMEE_ROUTING_EVENT_KIND     6401u
#define AIMEE_ROUTING_STAGE_SELECT   1u

typedef enum
{
   AIMEE_ROUTING_SELECT_BALANCED = 1,
   AIMEE_ROUTING_SELECT_RANDOMIZED = 2
} aimee_routing_select_mode_t;

static inline void aimee_routing_put_u16(uint8_t *p, uint16_t value)
{
   p[0] = (uint8_t)value;
   p[1] = (uint8_t)(value >> 8);
}

static inline void aimee_routing_put_u32(uint8_t *p, uint32_t value)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(value >> (8u * i));
}

static inline uint16_t aimee_routing_get_u16(const uint8_t *p)
{
   return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static inline uint32_t aimee_routing_get_u32(const uint8_t *p)
{
   uint32_t value = 0;
   for (unsigned i = 0; i < 4; ++i)
      value |= (uint32_t)p[i] << (8u * i);
   return value;
}

static inline int aimee_routing_request_encode(aimee_routing_select_mode_t mode,
                                                uint32_t candidate_count, uint8_t *output,
                                                size_t output_len)
{
   if (!output || output_len < AIMEE_ROUTING_REQUEST_LEN || candidate_count == 0 ||
       (mode != AIMEE_ROUTING_SELECT_BALANCED && mode != AIMEE_ROUTING_SELECT_RANDOMIZED))
      return -1;
   aimee_routing_put_u32(output, AIMEE_ROUTING_REQUEST_MAGIC);
   aimee_routing_put_u16(output + 4, AIMEE_ROUTING_WIRE_VERSION);
   aimee_routing_put_u16(output + 6, (uint16_t)mode);
   aimee_routing_put_u32(output + 8, candidate_count);
   return 0;
}

static inline int aimee_routing_request_decode(const uint8_t *input, size_t input_len,
                                                aimee_routing_select_mode_t *mode,
                                                uint32_t *candidate_count)
{
   if (!input || input_len != AIMEE_ROUTING_REQUEST_LEN || !mode || !candidate_count ||
       aimee_routing_get_u32(input) != AIMEE_ROUTING_REQUEST_MAGIC ||
       aimee_routing_get_u16(input + 4) != AIMEE_ROUTING_WIRE_VERSION)
      return -1;
   uint16_t decoded_mode = aimee_routing_get_u16(input + 6);
   uint32_t decoded_count = aimee_routing_get_u32(input + 8);
   if ((decoded_mode != AIMEE_ROUTING_SELECT_BALANCED &&
        decoded_mode != AIMEE_ROUTING_SELECT_RANDOMIZED) ||
       decoded_count == 0)
      return -1;
   *mode = (aimee_routing_select_mode_t)decoded_mode;
   *candidate_count = decoded_count;
   return 0;
}

static inline int aimee_routing_response_encode(uint32_t selected_index, uint8_t *output,
                                                 size_t output_len)
{
   if (!output || output_len < AIMEE_ROUTING_RESPONSE_LEN)
      return -1;
   aimee_routing_put_u32(output, AIMEE_ROUTING_RESPONSE_MAGIC);
   aimee_routing_put_u32(output + 4, selected_index);
   return 0;
}

static inline int aimee_routing_response_decode(const uint8_t *input, size_t input_len,
                                                 uint32_t candidate_count,
                                                 uint32_t *selected_index)
{
   if (!input || input_len != AIMEE_ROUTING_RESPONSE_LEN || !selected_index ||
       aimee_routing_get_u32(input) != AIMEE_ROUTING_RESPONSE_MAGIC)
      return -1;
   uint32_t decoded = aimee_routing_get_u32(input + 4);
   if (decoded >= candidate_count)
      return -1;
   *selected_index = decoded;
   return 0;
}

#endif /* AIMEE_ROUTING_MODULE_API_H */
