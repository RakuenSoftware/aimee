/* kb_workload_provider.h: P5 per-instance workload identity/custody boundary. */
#ifndef DEC_KB_WORKLOAD_PROVIDER_H
#define DEC_KB_WORKLOAD_PROVIDER_H 1

#include <stddef.h>
#include <stdint.h>

#define KB_WORKLOAD_CHALLENGE_LEN  32
#define KB_WORKLOAD_BINDING_LEN    32
#define KB_WORKLOAD_ANCHOR_LEN     32
#define KB_WORKLOAD_TOKEN_HASH_LEN 32
#define KB_WORKLOAD_WRAP_CAP       32768U
#define KB_WORKLOAD_UNWRAP_CAP     16384U

typedef enum
{
   KB_WORKLOAD_PROVIDER_NONE = 0,
   KB_WORKLOAD_PROVIDER_KMS_SPIFFE_V1 = 1,
   KB_WORKLOAD_PROVIDER_TPM2_V1 = 2,
   KB_WORKLOAD_PROVIDER_PKCS11_V1 = 3
} kb_workload_provider_kind_t;

typedef enum
{
   KB_WORKLOAD_OK = 0,
   KB_WORKLOAD_DISABLED = 1,
   KB_WORKLOAD_UNAVAILABLE = 2,
   KB_WORKLOAD_INTEGRITY = 3,
   KB_WORKLOAD_INVALID = 4
} kb_workload_result_t;

typedef struct
{
   kb_workload_provider_kind_t kind;
   const char *helper_path;
   const char *jwks_path;
   const char *proof_spki_path;
   const char *expected_issuer;
   const char *expected_audience;
   uint32_t max_token_age_seconds;
   uint32_t helper_timeout_ms;
} kb_workload_provider_config_t;

typedef struct
{
   char issuer[601];
   char subject[601];
   uint64_t issued_at;
   uint64_t expires_at;
   unsigned char proof_anchor_id[KB_WORKLOAD_ANCHOR_LEN];
   unsigned char custody_anchor_id[KB_WORKLOAD_ANCHOR_LEN];
   unsigned char token_hash[KB_WORKLOAD_TOKEN_HASH_LEN];
} kb_workload_identity_t;

typedef struct kb_workload_provider kb_workload_provider_t;

#ifdef __cplusplus
extern "C"
{
#endif

   kb_workload_result_t kb_workload_provider_open(const kb_workload_provider_config_t *,
                                                  kb_workload_provider_t **);
   kb_workload_result_t kb_workload_attest(kb_workload_provider_t *,
                                           const unsigned char challenge[KB_WORKLOAD_CHALLENGE_LEN],
                                           const unsigned char binding[KB_WORKLOAD_BINDING_LEN],
                                           kb_workload_identity_t *);
   /* The output buffer must provide at least KB_WORKLOAD_WRAP_CAP bytes. Plain
    * and cipher may overlap (including exact in-place operation); identity and
    * len must not overlap any data buffer or one another. */
   kb_workload_result_t kb_workload_wrap(kb_workload_provider_t *,
                                         const unsigned char challenge[KB_WORKLOAD_CHALLENGE_LEN],
                                         const unsigned char binding[KB_WORKLOAD_BINDING_LEN],
                                         const void *plain, size_t plain_len,
                                         kb_workload_identity_t *, unsigned char *cipher,
                                         size_t cap, size_t *len);
   /* The output buffer must provide at least KB_WORKLOAD_UNWRAP_CAP bytes. The
    * same overlap restrictions as kb_workload_wrap apply. */
   kb_workload_result_t kb_workload_unwrap(kb_workload_provider_t *,
                                           const unsigned char challenge[KB_WORKLOAD_CHALLENGE_LEN],
                                           const unsigned char binding[KB_WORKLOAD_BINDING_LEN],
                                           const void *cipher, size_t cipher_len,
                                           kb_workload_identity_t *, unsigned char *plain,
                                           size_t cap, size_t *len);
   /* The caller must quiesce all calls using provider before close. */
   void kb_workload_provider_close(kb_workload_provider_t *);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_WORKLOAD_PROVIDER_H */
