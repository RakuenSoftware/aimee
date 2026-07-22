/* Private binary protocol shared by kb_http_client and aimee-kb-resolver. */
#ifndef DEC_KB_HTTP_RESOLVER_PROTOCOL_H
#define DEC_KB_HTTP_RESOLVER_PROTOCOL_H 1

#include <stdint.h>

#define KB_RESOLVER_REQUEST_MAGIC  0x41525351U /* ARSQ */
#define KB_RESOLVER_RESPONSE_MAGIC 0x41525350U /* ARSP */
#define KB_RESOLVER_VERSION        1U
#define KB_RESOLVER_FLAG_HANG_TEST 0x01U
#define KB_RESOLVER_HOST_MAX       255U
#define KB_RESOLVER_SERVICE_MAX    15U
#define KB_RESOLVER_ADDRESS_MAX    16U
#define KB_RESOLVER_RECORD_MAX     16U
#define KB_RESOLVER_WIRE_MAX       512U

enum
{
   KB_RESOLVER_STATUS_OK = 0,
   KB_RESOLVER_STATUS_NOT_FOUND = 1,
   KB_RESOLVER_STATUS_PROTOCOL = 2
};

static inline void kb_resolver_put_u32(unsigned char *p, uint32_t value)
{
   p[0] = (unsigned char)(value >> 24);
   p[1] = (unsigned char)(value >> 16);
   p[2] = (unsigned char)(value >> 8);
   p[3] = (unsigned char)value;
}

static inline uint32_t kb_resolver_get_u32(const unsigned char *p)
{
   return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void kb_resolver_put_u16(unsigned char *p, uint16_t value)
{
   p[0] = (unsigned char)(value >> 8);
   p[1] = (unsigned char)value;
}

static inline uint16_t kb_resolver_get_u16(const unsigned char *p)
{
   return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

#endif
