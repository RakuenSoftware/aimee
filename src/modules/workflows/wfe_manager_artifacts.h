/* wfe_manager_artifacts.h -- typed, versioned artifact schemas for the
 * primary-as-manager blocks (understand -> intent record, split -> packet plan,
 * review -> verdict).
 *
 * These are LOAD-BEARING workflow signals, so they are validated structurally
 * (not free-form prose): a schema-invalid artifact is a non-advancing failure,
 * never a silent pass. The validators REQUIRE the known fields and tolerate
 * unknown ones, so the S2 interactive slice can add fields (user_clarifications,
 * replay_anchor, with_user_session_ref, ...) without a serialization migration
 * and without breaking S1 validation -- the S1 delegate executor and the S2
 * primary-agent executor must both satisfy the SAME contract (see the contract
 * test). Design per the I1/I3 roundtable consult (2026-07-01).
 */
#ifndef DEC_WFE_MANAGER_ARTIFACTS_H
#define DEC_WFE_MANAGER_ARTIFACTS_H 1

#include <stddef.h>

#include "cJSON.h"

/* Current schema versions (bump on a breaking change; validators accept only
 * the version they know so an unversioned/old artifact fails closed). */
#define WFE_INTENT_SCHEMA_VERSION  1
#define WFE_PACKETS_SCHEMA_VERSION 1
#define WFE_REVIEW_SCHEMA_VERSION  1

/* An intent record's confirmation state. S1 (no user binding) emits UNCONFIRMED
 * so a guessed intent is visible to downstream blocks; S2 (with the user) may
 * emit CONFIRMED. */
typedef enum
{
   WFE_INTENT_UNCONFIRMED = 0,
   WFE_INTENT_CONFIRMED
} wfe_intent_status_t;

/* review verdict -> gate mapping. */
typedef enum
{
   WFE_REVIEW_PASS = 0,   /* -> ADVANCED / on_pass */
   WFE_REVIEW_CHANGES = 1 /* -> LOOPED / on_fail (re-delegate) */
} wfe_review_verdict_t;

/* Each validator returns 0 on a valid artifact, -1 otherwise (with `err` set).
 * Unknown fields are tolerated (forward-compat); known fields are required and
 * type-checked. */
int wfe_intent_validate(const cJSON *rec, char *err, size_t errlen);
int wfe_packets_validate(const cJSON *rec, char *err, size_t errlen);
int wfe_review_validate(const cJSON *rec, char *err, size_t errlen);

/* Read the parsed status/verdict from an already-validated record. Returns 0 and
 * fills *out on success; -1 if the record is invalid. */
int wfe_intent_status(const cJSON *rec, wfe_intent_status_t *out);
int wfe_review_verdict(const cJSON *rec, wfe_review_verdict_t *out);

#endif /* DEC_WFE_MANAGER_ARTIFACTS_H */
