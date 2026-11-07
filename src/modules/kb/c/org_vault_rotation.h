#ifndef AIMEE_KB_STORE_ORG_VAULT_ROTATION_H
#define AIMEE_KB_STORE_ORG_VAULT_ROTATION_H 1

#include <stddef.h>
#include "sealed_envelope.h"
#include <stdint.h>

#define KB_STORE_VAULT_ROTATION_ATTEST_MAX 512
#define KB_STORE_VAULT_ROTATION_REF_MAX    512
#define KB_STORE_VAULT_ROTATION_OWNER_MAX  200
#define KB_STORE_VAULT_ROTATION_SECRET_MAX AIMEE_SEALED_ENVELOPE_CIPHERTEXT_MAX

typedef aimee_sealed_envelope_t kb_store_vault_rotation_envelope_t;

typedef struct
{
   int64_t id;
   int64_t team_id;
   int has_team;
   int64_t from_version;
   int64_t to_version;
   int compromise;
   char key_id[601];
   char principal[601];
   char agent[256];
   char cred[256];
   char state[16];
   char last_error[1001];
   char old_vendor_ref[KB_STORE_VAULT_ROTATION_REF_MAX + 1];
   char new_vendor_ref[KB_STORE_VAULT_ROTATION_REF_MAX + 1];
   char revoke_receipt[KB_STORE_VAULT_ROTATION_REF_MAX + 1];
   char failure_phase[33];
   char claim_owner[KB_STORE_VAULT_ROTATION_OWNER_MAX + 1];
   int64_t claim_token;
   char claim_until[64];
   uint8_t hwm_attestation[KB_STORE_VAULT_ROTATION_ATTEST_MAX];
   size_t hwm_attestation_len;
} kb_store_vault_rotation_row_t;

int kb_store_vault_rotation_start(const char *actor, const char *key_id, const char *principal,
                                  int has_team, int64_t team_id, const char *agent,
                                  const char *cred, int64_t from_version, int compromise,
                                  int64_t *out_id);
int kb_store_vault_rotation_stage(const char *actor, int64_t rotation_id,
                                  const uint8_t *wrapped_dek, size_t wrapped_dek_len,
                                  const uint8_t *nonce, size_t nonce_len, const uint8_t *ciphertext,
                                  size_t ciphertext_len, const uint8_t *tag, size_t tag_len,
                                  int64_t *out_version);
int kb_store_vault_rotation_transition(const char *actor, int64_t rotation_id, const char *expected,
                                       const char *next, const char *error);
int kb_store_vault_rotation_finalize(const char *actor, int64_t rotation_id,
                                     const uint8_t *attestation, size_t attestation_len);
int kb_store_vault_rotation_get(int64_t rotation_id, kb_store_vault_rotation_row_t *out);
int kb_store_vault_rotation_claim(const char *actor, int64_t rotation_id, const char *expected,
                                  const char *owner, int ttl_seconds, int64_t *token);
int kb_store_vault_rotation_heartbeat(const char *actor, int64_t rotation_id, const char *owner,
                                      int64_t token, int ttl_seconds);
int kb_store_vault_rotation_release(const char *actor, int64_t rotation_id, const char *owner,
                                    int64_t token);
int kb_store_vault_rotation_checkpoint_old_ref(const char *actor, int64_t rotation_id,
                                               const char *owner, int64_t token,
                                               const char *old_vendor_ref);
int kb_store_vault_rotation_stage_claimed(const char *actor, int64_t rotation_id, const char *owner,
                                          int64_t token, const char *new_vendor_ref,
                                          const kb_store_vault_rotation_envelope_t *envelope);
int kb_store_vault_rotation_probe_admit(const char *actor, int64_t rotation_id, const char *owner,
                                        int64_t token, const char *operation_key,
                                        kb_store_vault_rotation_envelope_t *envelope);
int kb_store_vault_rotation_transition_claimed(const char *actor, int64_t rotation_id,
                                               const char *owner, int64_t token,
                                               const char *expected, const char *next,
                                               const char *receipt);
int kb_store_vault_rotation_fail_claimed(const char *actor, int64_t rotation_id, const char *owner,
                                         int64_t token, const char *expected, const char *phase,
                                         const char *error);
int kb_store_vault_rotation_remediate(const char *actor, int64_t rotation_id, const char *owner,
                                      int64_t token, int64_t anchor_version, const char *evidence);

/* Rotation requests: an admin files one; the KB daemon drives it. */
#define KB_STORE_VAULT_ROTATION_REQUEST_UNKNOWN  (-2)
#define KB_STORE_VAULT_ROTATION_REQUEST_CONFLICT (-3)

typedef struct
{
   int64_t id;
   int64_t team_id;
   char kind[16];
   char model_id[201];
   int64_t rotation_id;
   int has_slot;
   char key_id[601];
   char principal[601];
   char agent[256];
   char cred[256];
} kb_store_vault_rotation_request_t;

typedef struct
{
   int64_t id;
   int64_t team_id;
} kb_store_vault_rotation_ref_t;

typedef struct
{
   int64_t id;
   int64_t team_id;
   char kind[16];
   char model_id[201];
   int64_t rotation_id;
   char state[16];
   char detail[501];
   char created_at[64];
   char rotation_state[16];
   char failure_phase[33];
   char last_error[1001];
   char slot_principal[601];
} kb_store_vault_rotation_request_row_t;

typedef struct
{
   int64_t team_id;
   char model_id[201];
   int interval_days;
   char last_retired_at[64];
} kb_store_vault_rotation_schedule_t;

/* Returns 0, KB_STORE_VAULT_ROTATION_REQUEST_UNKNOWN when the binding or the
 * rotation it names does not exist, or -1. Repeating a pending request returns
 * its id. */
int kb_store_vault_rotation_request_add(const char *actor, int64_t team_id, const char *kind,
                                        const char *model_id, int64_t rotation_id, int64_t *out_id);
/* 0 with the oldest pending request, 1 when none is pending, -1 on error. */
int kb_store_vault_rotation_request_next(const char *actor, kb_store_vault_rotation_request_t *out);
int kb_store_vault_rotation_request_settle(const char *actor, int64_t request_id, int accepted,
                                           int64_t rotation_id, const char *detail);
/* Unclaimed rotations short of retired or failed; returns the count or -1. */
int kb_store_vault_rotation_pending(const char *actor, kb_store_vault_rotation_ref_t *out, int cap);
int kb_store_vault_rotation_request_list(const char *actor, int64_t team_id,
                                         kb_store_vault_rotation_request_row_t *out, int cap);
/* Adopt a slot whose current version no rotation reached. 0, ..._UNKNOWN when the
 * team has no such slot, ..._CONFLICT when it is already usable or rotating, -1. */
int kb_store_vault_rotation_request_adopt(const char *actor, int64_t team_id, const char *key_id,
                                          const char *principal, const char *agent,
                                          const char *cred, int64_t *out_id);
/* interval_days 0 removes the schedule. ..._UNKNOWN when the binding is missing. */
int kb_store_vault_rotation_schedule_set(const char *actor, int64_t team_id, const char *model_id,
                                         int interval_days);
int kb_store_vault_rotation_schedule_list(const char *actor, int64_t team_id,
                                          kb_store_vault_rotation_schedule_t *out, int cap);
/* Due schedules (team_id, model_id); returns the count or -1. */
int kb_store_vault_rotation_schedule_due(const char *actor, kb_store_vault_rotation_schedule_t *out,
                                         int cap);

#endif
