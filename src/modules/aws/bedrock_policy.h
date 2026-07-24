/* modules/aws/bedrock_policy.h: least-privilege Bedrock session-policy derivation.
 *
 * Pure/offline: derives an EXACT, minimal IAM session policy (JSON) for a Bedrock
 * invocation target, FAIL-CLOSED. The action set is keyed ONLY on the streaming
 * flag (AWS authorizes Converse/ConverseStream via the InvokeModel IAM actions —
 * there is NO `bedrock:Converse` IAM action):
 *   non-streaming -> bedrock:InvokeModel
 *   streaming     -> bedrock:InvokeModelWithResponseStream
 *
 * The Resource ARN set is derived per target type. An unknown type, a missing
 * region set / account / id, or an empty underlying-FM set for a profile returns
 * ERROR — NEVER a broad Resource:"*" or "bedrock:InvokeModel*". The emitted JSON is
 * canonical + stable-ordered so its SHA-256 is a stable cache-key input.
 *
 * NOTE (P6b invariant): the target's type/ARNs/partition/region-set MUST be resolved
 * from primary-authoritative catalog config, never a client string. P6a proves the
 * DERIVATION is exact + fail-closed; it does not itself prove authoritative sourcing.
 *
 * Depends only on libc (no OpenSSL needed here). */
#ifndef DEC_BEDROCK_POLICY_H
#define DEC_BEDROCK_POLICY_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      BEDROCK_TARGET_FOUNDATION = 0,
      BEDROCK_TARGET_PROVISIONED,
      BEDROCK_TARGET_CUSTOM,
      BEDROCK_TARGET_APP_INFERENCE_PROFILE,
      BEDROCK_TARGET_CROSS_REGION_INFERENCE_PROFILE,
   } bedrock_target_type_t;

   /* A Bedrock invocation target. All string pointers are borrowed. region_set is
    * the set of regions the target spans (>=1). For the two profile types,
    * underlying_fm_arns are the destination-region foundation-model ARNs the profile
    * routes to (>=1) — added verbatim to the resource set. */
   typedef struct
   {
      bedrock_target_type_t type;
      const char *partition;     /* "aws" | "aws-us-gov" | "aws-cn" */
      const char *invoke_region; /* endpoint + SigV4 credential-scope region */
      const char *const *region_set;
      size_t n_regions;
      const char *account; /* required except for foundation-model ARNs */
      const char *id;      /* model id or profile id */
      const char *const *underlying_fm_arns;
      size_t n_underlying;
   } bedrock_target_t;

   /* Derive the session policy JSON into out[cap]. Returns 0 on success (out is a
    * NUL-terminated canonical policy), or -1 (FAIL-CLOSED) on any underivable/invalid
    * target. On -1, out is set to an empty string and is NEVER a wildcard policy. */
   int bedrock_session_policy(const bedrock_target_t *target, int is_streaming, char *out,
                              size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_BEDROCK_POLICY_H */
