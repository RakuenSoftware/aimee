#ifndef AIMEE_KB_MGMT_TOKEN_PUBLIC_H
#define AIMEE_KB_MGMT_TOKEN_PUBLIC_H

#include <stddef.h>
#include <stdint.h>

#define KB_MGMT_TOKEN_MODULUS_LEN  384
#define KB_MGMT_TOKEN_KID_MAX      64
#define KB_MGMT_TOKEN_JWK_MAX      768
#define KB_MGMT_TOKEN_ROOT_AAD_MAX 128

#ifdef __cplusplus
extern "C"
{
#endif

   int kb_mgmt_token_kid(const uint8_t *modulus, size_t modulus_len, char *out, size_t cap);
   int kb_mgmt_token_jwk(const uint8_t *modulus, size_t modulus_len, char *out, size_t cap,
                         size_t *out_len);
   int kb_mgmt_token_jwk_validate(const char *jwk, size_t jwk_len, uint8_t *modulus,
                                  size_t modulus_cap, size_t *modulus_len);
   int kb_mgmt_token_root_aad(int64_t version, uint8_t *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
