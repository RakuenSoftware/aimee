/* kb_vault_rotation_worker.c: see kb_vault_rotation_worker.h. */
#include "kb_vault_rotation_worker.h"

#include "kb_identity.h"
#include "kb_vault_policy.h"
#include "kb_vault_rotation.h"
#include "kb_vault_rotation_aws.h"
#include "kb_vault_rotation_ops.h"
#include "modules/kb/c/kb_store.h"
#include "log.h"
#include "modules/kb/c/kb_store_tenant.h"
#include "modules/vault/vault_crypto.h"
#include "modules/vault/vault_store.h"
#include "org_vault_rotation.h"
#include "vault_server_key.h"

#include <limits.h>
#include <openssl/crypto.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ROTATION_LEASE_SECONDS     60
#define ROTATION_TICK_SECONDS      15
#define ROTATION_REQUESTS_PER_TICK 8
#define ROTATION_PENDING_PER_TICK  16
#define ROTATION_STEPS_PER_TICK    8
#define ROTATION_SCHEDULE_SECONDS  300
#define ROTATION_SECRET_MAX        4096

static kb_vault_rotation_aws_t g_aws;
static pthread_t g_thread;
static int g_running;
static volatile int g_stop;

static int owner_principal(kb_principal_t *owner, char actor[576])
{
   kb_verify_result_t v;
   memset(&v, 0, sizeof(v));
   return kb_principal_from_verify(&v, "", owner) == 0 && kb_identity_key(owner, actor, 576) == 0
              ? 0
              : -1;
}

static int settle(const kb_principal_t *owner, const char *actor, int64_t request_id, int accepted,
                  int64_t rotation_id, const char *detail)
{
   if (kb_store_tenant_scope_begin(owner, 0) != 0)
      return -1;
   if (kb_store_vault_rotation_request_settle(actor, request_id, accepted, rotation_id, detail) !=
       0)
   {
      kb_store_tenant_scope_rollback();
      return -1;
   }
   return kb_store_tenant_scope_commit() == 0 ? 0 : -1;
}

static void start_rotation(const kb_principal_t *owner, const char *actor,
                           const kb_store_vault_rotation_request_t *q)
{
   if (!q->has_slot)
   {
      (void)settle(owner, actor, q->id, 0, 0, "the team's egress binding for that model is gone");
      return;
   }
   uint8_t att[KB_STORE_VAULT_ROTATION_ATTEST_MAX];
   size_t att_len = 0;
   uint64_t anchor = 0;
   int rc = vault_hwm_read(q->key_id, &anchor, att, sizeof(att), &att_len);
   OPENSSL_cleanse(att, sizeof(att));
   if (rc != 0 || anchor < 1 || anchor >= INT64_MAX)
   {
      (void)settle(owner, actor, q->id, 0, 0,
                   "the vault custody keeps no signed high-water mark for this slot; rotation "
                   "needs KMS custody");
      return;
   }
   int64_t id = 0;
   if (kb_vault_rotation_start(owner, q->team_id, q->key_id, q->principal, q->agent, q->cred,
                               (int64_t)anchor, 0, &id) != 0)
   {
      (void)settle(owner, actor, q->id, 0, 0,
                   "could not start: the slot's stored version does not match its high-water "
                   "mark, or a different rotation of it is active");
      return;
   }
   aimee_log(LOG_INFO, "kb.vault.rotation", "request %lld started rotation %lld (team %lld)",
             (long long)q->id, (long long)id, (long long)q->team_id);
   (void)settle(owner, actor, q->id, 1, id, "");
}

static void remediate_rotation(const kb_principal_t *owner, const char *actor, const char *worker,
                               const kb_store_vault_rotation_request_t *q)
{
   kb_vault_rotation_aws_set_caller(owner, q->team_id);
   int rc = kb_vault_rotation_ops_remediate(owner, q->team_id, q->rotation_id, worker,
                                            ROTATION_LEASE_SECONDS);
   kb_vault_rotation_aws_set_caller(NULL, 0);
   aimee_log(rc == 0 ? LOG_INFO : LOG_WARN, "kb.vault.rotation", "remediation of rotation %lld %s",
             (long long)q->rotation_id, rc == 0 ? "retired it" : "did not complete");
   (void)settle(owner, actor, q->id, rc == 0, q->rotation_id,
                rc == 0 ? ""
                        : "remediation did not complete: the rotation is not failed, is claimed, "
                          "or the vendor could not confirm the revoke; check and file it again");
}

/* Adopt a slot whose current version was written by an ordinary vault write:
 * no rotation reached it, so key use refuses it. Re-stage the same credential
 * as the next version through the fenced rotation (HWM-attested activation),
 * after STS confirms it is a working key. This is the one read of a credential
 * before admission, because it is what creates admission; the plaintext stays
 * in this function and is cleansed. The driver then retires the rotation with
 * nothing to revoke, since the predecessor is the same credential. */
static const char *adopt_slot(const kb_principal_t *owner,
                              const kb_store_vault_rotation_request_t *q, int64_t *rotation_id)
{
   uint8_t att[KB_STORE_VAULT_ROTATION_ATTEST_MAX];
   size_t att_len = 0;
   uint64_t anchor = 0;
   int rc = vault_hwm_read(q->key_id, &anchor, att, sizeof(att), &att_len);
   OPENSSL_cleanse(att, sizeof(att));
   if (rc != 0 || anchor < 1 || anchor >= INT64_MAX)
      return "the vault custody keeps no signed high-water mark for this slot; adoption needs "
             "KMS custody";

   /* Read and confirm the credential before any rotation exists, so a refusal
    * here leaves nothing behind for the driver. */
   static char plain[ROTATION_SECRET_MAX];
   static uint8_t ciphertext[ROTATION_SECRET_MAX];
   uint8_t kek[VAULT_KEK_LEN], dek[VAULT_DEK_LEN], wrapped[VAULT_WRAPPED_DEK_LEN];
   uint8_t nonce[VAULT_GCM_NONCE_LEN], tag[VAULT_GCM_TAG_LEN], aad[VAULT_ENVELOPE_AAD_MAX];
   size_t aad_len = 0, len = 0;
   char arn[512] = "";
   const char *failure = NULL;
   if (vault_server_kek(kek) != 0 ||
       vault_store_get(q->principal, kek, q->agent, q->cred, plain, sizeof(plain)) != 0)
      failure = "the slot's current credential could not be read";
   else
   {
      len = strlen(plain);
      int id = kb_vault_rotation_aws_identity(&g_aws, (const unsigned char *)plain, len, arn,
                                              sizeof(arn));
      if (id == KB_VAULT_OP_UNCERTAIN)
         failure = "STS could not be reached to confirm the credential; file the adoption again";
      else if (id != KB_VAULT_OP_OK)
         failure = "the stored credential is not a working AWS access key";
   }

   kb_store_vault_rotation_row_t row;
   memset(&row, 0, sizeof(row));
   if (!failure && kb_vault_rotation_start(owner, q->team_id, q->key_id, q->principal, q->agent,
                                           q->cred, (int64_t)anchor, 0, rotation_id) != 0)
      failure = "could not start: the slot's stored version does not match its high-water mark, "
                "or a rotation of it is active";
   if (!failure)
   {
      if (kb_store_tenant_scope_begin(owner, q->team_id) != 0)
         failure = "the adoption's rotation could not be read";
      else if (kb_store_vault_rotation_get(*rotation_id, &row) != 0)
      {
         kb_store_tenant_scope_rollback();
         failure = "the adoption's rotation could not be read";
      }
      else if (kb_store_tenant_scope_commit() != 0)
         failure = "the adoption's rotation could not be read";
   }
   /* kb_vault_rotation_start returns the slot's existing rotation for a repeat,
    * so a retried adoption resumes where the last attempt stopped. */
   if (!failure && !strcmp(row.state, "provision"))
   {
      if (vault_crypto_random(dek, sizeof(dek)) != 0 || vault_dek_wrap(kek, dek, wrapped) != 0 ||
          vault_aad_build_v2(q->principal, q->agent, q->cred, row.to_version, aad, sizeof(aad),
                             &aad_len) != 0 ||
          vault_secret_encrypt(dek, aad, aad_len, (const uint8_t *)plain, len, nonce, ciphertext,
                               tag) != 0 ||
          kb_vault_rotation_stage(owner, q->team_id, *rotation_id, wrapped, sizeof(wrapped), nonce,
                                  sizeof(nonce), ciphertext, len, tag, sizeof(tag)) != 0)
         failure = "the credential could not be staged; file the adoption again";
      else
         snprintf(row.state, sizeof(row.state), "staged");
   }
   OPENSSL_cleanse(plain, sizeof(plain));
   OPENSSL_cleanse(ciphertext, sizeof(ciphertext));
   OPENSSL_cleanse(kek, sizeof(kek));
   OPENSSL_cleanse(dek, sizeof(dek));
   if (!failure && !strcmp(row.state, "staged"))
   {
      if (kb_vault_rotation_mark_probed(owner, q->team_id, *rotation_id) != 0)
         failure = "the adopted version could not be marked probed; file the adoption again";
      else
         snprintf(row.state, sizeof(row.state), "probed");
   }
   if (!failure && (!strcmp(row.state, "probed") || !strcmp(row.state, "activating")) &&
       kb_vault_rotation_activate_or_resume(owner, q->team_id, *rotation_id) !=
           KB_VAULT_ROTATION_COMPLETE)
      failure = "the adopted version could not be activated; file the adoption again";
   if (!failure && !strcmp(row.state, "failed"))
      failure = "the adoption's rotation failed; remediate it, then file the adoption again";
   if (!failure)
      aimee_log(LOG_INFO, "kb.vault.rotation", "rotation %lld adopted the key of %s",
                (long long)*rotation_id, arn);
   OPENSSL_cleanse(&row, sizeof(row));
   return failure;
}

static void take_requests(const kb_principal_t *owner, const char *actor, const char *worker)
{
   for (int i = 0; i < ROTATION_REQUESTS_PER_TICK && !g_stop; i++)
   {
      kb_store_vault_rotation_request_t q;
      if (kb_store_tenant_scope_begin(owner, 0) != 0)
         return;
      int rc = kb_store_vault_rotation_request_next(actor, &q);
      if (rc != 0)
      {
         kb_store_tenant_scope_rollback();
         return;
      }
      if (kb_store_tenant_scope_commit() != 0)
         return;
      if (!strcmp(q.kind, "rotate"))
         start_rotation(owner, actor, &q);
      else if (!strcmp(q.kind, "adopt"))
      {
         int64_t id = 0;
         const char *failure = adopt_slot(owner, &q, &id);
         if (failure)
            aimee_log(LOG_WARN, "kb.vault.rotation", "adoption request %lld: %s", (long long)q.id,
                      failure);
         (void)settle(owner, actor, q.id, !failure, id, failure ? failure : "");
      }
      else
         remediate_rotation(owner, actor, worker, &q);
      OPENSSL_cleanse(&q, sizeof(q));
   }
}

static const char *result_name(int rc)
{
   switch (rc)
   {
   case KB_VAULT_OP_COMPLETE:
      return "retired";
   case KB_VAULT_OP_UNCERTAIN:
      return "uncertain; will retry";
   case KB_VAULT_OP_REMEDIATION_REQUIRED:
      return "failed; needs `aimee-kb vault remediate`";
   default:
      return "failed";
   }
}

static int rotation_state(const kb_principal_t *owner, int64_t team, int64_t id, char state[16])
{
   kb_store_vault_rotation_row_t row;
   if (kb_store_tenant_scope_begin(owner, team) != 0)
      return -1;
   int rc = kb_store_vault_rotation_get(id, &row);
   if (rc != 0)
   {
      kb_store_tenant_scope_rollback();
      return -1;
   }
   snprintf(state, 16, "%s", row.state);
   OPENSSL_cleanse(&row, sizeof(row));
   return kb_store_tenant_scope_commit() == 0 ? 0 : -1;
}

/* A rotation that makes no progress (a vendor that keeps refusing a revoke, or
 * an IAM key that never propagates) is retried each tick but reported only
 * once an hour, so the log says it is stuck without repeating it. */
typedef struct
{
   int64_t id;
   char state[16];
   time_t since;
   time_t reported;
} stall_t;

static stall_t g_stalls[ROTATION_PENDING_PER_TICK];

static void note_progress(int64_t id, int64_t team, const char *state, time_t now)
{
   stall_t *free_slot = NULL, *oldest = &g_stalls[0];
   for (int i = 0; i < ROTATION_PENDING_PER_TICK; i++)
   {
      stall_t *s = &g_stalls[i];
      if (s->id == id)
      {
         if (strcmp(s->state, state))
         {
            snprintf(s->state, sizeof(s->state), "%s", state);
            s->since = s->reported = now;
         }
         else if (now - s->since >= 300 && now - s->reported >= 3600)
         {
            s->reported = now;
            aimee_log(LOG_WARN, "kb.vault.rotation",
                      "rotation %lld (team %lld) has been %s for %lld min without progress; "
                      "check the vendor and the credential's IAM policy",
                      (long long)id, (long long)team, state, (long long)((now - s->since) / 60));
         }
         return;
      }
      if (!s->id && !free_slot)
         free_slot = s;
      if (s->since < oldest->since)
         oldest = s;
   }
   stall_t *s = free_slot ? free_slot : oldest;
   s->id = id;
   snprintf(s->state, sizeof(s->state), "%s", state);
   s->since = now;
   s->reported = now - 3600 + 300;
}

static void drive(const kb_principal_t *owner, const char *actor, const char *worker)
{
   kb_store_vault_rotation_ref_t refs[ROTATION_PENDING_PER_TICK];
   if (kb_store_tenant_scope_begin(owner, 0) != 0)
      return;
   int n = kb_store_vault_rotation_pending(actor, refs, ROTATION_PENDING_PER_TICK);
   if (n < 0)
   {
      kb_store_tenant_scope_rollback();
      return;
   }
   if (kb_store_tenant_scope_commit() != 0)
      return;
   for (int i = 0; i < n && !g_stop; i++)
   {
      int rc = KB_VAULT_OP_RETRY;
      char before[16] = "", after[16] = "";
      if (rotation_state(owner, refs[i].team_id, refs[i].id, before) != 0)
         continue;
      kb_vault_rotation_aws_set_caller(owner, refs[i].team_id);
      for (int s = 0; s < ROTATION_STEPS_PER_TICK && !g_stop &&
                      (rc == KB_VAULT_OP_OK || rc == KB_VAULT_OP_RETRY);
           s++)
      {
         rc = kb_vault_rotation_ops_step(owner, refs[i].team_id, refs[i].id, worker,
                                         ROTATION_LEASE_SECONDS);
         if (rotation_state(owner, refs[i].team_id, refs[i].id, after) != 0 ||
             !strcmp(before, after))
            break; /* no progress: try again next tick */
         memcpy(before, after, sizeof(before));
      }
      kb_vault_rotation_aws_set_caller(NULL, 0);
      if (rc != KB_VAULT_OP_OK && rc != KB_VAULT_OP_RETRY)
         aimee_log(rc == KB_VAULT_OP_COMPLETE ? LOG_INFO : LOG_WARN, "kb.vault.rotation",
                   "rotation %lld (team %lld): %s", (long long)refs[i].id,
                   (long long)refs[i].team_id, result_name(rc));
      else if (after[0])
         note_progress(refs[i].id, refs[i].team_id, after, time(NULL));
   }
}

/* File a rotate request for every schedule that is due. The request goes
 * through the same admission as an operator's, so the queue and the WORM log
 * show who asked (the owner, on the schedule's behalf). */
static time_t g_next_schedule;

static void file_scheduled(const kb_principal_t *owner, const char *actor)
{
   time_t now = time(NULL);
   if (now < g_next_schedule)
      return;
   g_next_schedule = now + ROTATION_SCHEDULE_SECONDS;
   kb_store_vault_rotation_schedule_t due[ROTATION_REQUESTS_PER_TICK];
   if (kb_store_tenant_scope_begin(owner, 0) != 0)
      return;
   int n = kb_store_vault_rotation_schedule_due(actor, due, ROTATION_REQUESTS_PER_TICK);
   if (n < 0 || kb_store_tenant_scope_commit() != 0)
   {
      if (n < 0)
         kb_store_tenant_scope_rollback();
      return;
   }
   for (int i = 0; i < n; i++)
   {
      int64_t id = 0;
      if (kb_store_tenant_scope_begin(owner, due[i].team_id) != 0)
         continue;
      if (kb_store_vault_rotation_request_add(actor, due[i].team_id, "rotate", due[i].model_id, 0,
                                              &id) != 0)
      {
         kb_store_tenant_scope_rollback();
         continue;
      }
      if (kb_store_tenant_scope_commit() == 0)
         aimee_log(LOG_INFO, "kb.vault.rotation", "schedule filed request %lld (team %lld, %s)",
                   (long long)id, (long long)due[i].team_id, due[i].model_id);
   }
}

void kb_vault_rotation_worker_schedule_now(void)
{
   g_next_schedule = 0;
}

void kb_vault_rotation_worker_tick(void)
{
   if (!kb_vault_live_keys_allowed())
      return;
   kb_principal_t owner;
   char actor[576];
   if (owner_principal(&owner, actor) != 0)
      return;
   char worker[64];
   snprintf(worker, sizeof(worker), "aimee-kb:%ld", (long)getpid());
   file_scheduled(&owner, actor);
   take_requests(&owner, actor, worker);
   drive(&owner, actor, worker);
   kb_store_lease_release_idle();
}

static void *worker_main(void *arg)
{
   (void)arg;
   while (!g_stop)
   {
      kb_vault_rotation_worker_tick();
      for (int i = 0; i < ROTATION_TICK_SECONDS * 5 && !g_stop; i++)
      {
         struct timespec ts = {.tv_sec = 0, .tv_nsec = 200L * 1000 * 1000};
         nanosleep(&ts, NULL);
      }
   }
   return NULL;
}

int kb_vault_rotation_worker_register_aws(void)
{
   kb_vault_rotation_aws_defaults(&g_aws);
   return kb_vault_rotation_ops_register(kb_vault_rotation_aws_provider(), &g_aws);
}

void kb_vault_rotation_worker_set_aws(const kb_vault_rotation_aws_t *aws)
{
   if (aws)
      g_aws = *aws;
}

int kb_vault_rotation_worker_start(void)
{
   if (g_running)
      return 0;
   if (kb_vault_rotation_worker_register_aws() != 0)
      return -1;
   g_stop = 0;
   if (pthread_create(&g_thread, NULL, worker_main, NULL) != 0)
      return -1;
   g_running = 1;
   return 0;
}

void kb_vault_rotation_worker_stop(void)
{
   if (!g_running)
      return;
   g_stop = 1;
   (void)pthread_join(g_thread, NULL);
   g_running = 0;
}
