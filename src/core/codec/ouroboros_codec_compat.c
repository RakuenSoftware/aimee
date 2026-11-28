/* Thin compatibility adapter forwarding aimee/core/codec/base64.h to
 * ouroboros-codec (ouroboros/codec/base64.h). */
#include <aimee/core/codec/base64.h>
#include <ouroboros/codec/base64.h>

size_t aimee_codec_base64_capacity(size_t length, aimee_base64_variant_t variant)
{
   return ouro_codec_base64_capacity(length, (ouro_base64_variant_t)variant);
}

int aimee_codec_base64_encode(const unsigned char *in, size_t length, char *out, size_t capacity,
                              aimee_base64_variant_t variant, size_t *written)
{
   return ouro_codec_base64_encode(in, length, out, capacity, (ouro_base64_variant_t)variant,
                                   written);
}

int aimee_codec_base64_decode(const char *in, size_t length, unsigned char *out, size_t capacity,
                              aimee_base64_profile_t profile, size_t *written)
{
   return ouro_codec_base64_decode(in, length, out, capacity, (ouro_base64_profile_t)profile,
                                   written);
}
