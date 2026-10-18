#ifndef AIMEE_CORE_CODEC_BASE64_H
#define AIMEE_CORE_CODEC_BASE64_H
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      AIMEE_BASE64_STANDARD,
      AIMEE_BASE64_URL
   } aimee_base64_variant_t;

   typedef enum
   {
      /* Exact RFC 4648 spelling: required padding, no whitespace, zero unused bits. */
      AIMEE_BASE64_STANDARD_CANONICAL,
      /* JWT spelling: URL alphabet, no padding/whitespace, zero unused bits. */
      AIMEE_BASE64_URL_CANONICAL,
      /* Legacy text framing: skip ASCII whitespace, stop at the first '='. */
      AIMEE_BASE64_STANDARD_TEXT_COMPAT,
      /* Legacy URL callers: URL alphabet, no padding, unchecked unused bits. */
      AIMEE_BASE64_URL_COMPAT
   } aimee_base64_profile_t;

   /* Capacity includes the NUL terminator. Zero means size overflow/invalid variant. */
   size_t aimee_codec_base64_capacity(size_t length, aimee_base64_variant_t variant);
   /* Return 1 on success, 0 on invalid input or insufficient capacity. Encoding is
    * NUL-terminated; decoding is binary. written is zero on failure. Output may be
    * partially written on decode failure and must be discarded by the caller. */
   int aimee_codec_base64_encode(const unsigned char *in, size_t length, char *out, size_t capacity,
                                 aimee_base64_variant_t variant, size_t *written);
   int aimee_codec_base64_decode(const char *in, size_t length, unsigned char *out, size_t capacity,
                                 aimee_base64_profile_t profile, size_t *written);
#ifdef __cplusplus
}
#endif
#endif
