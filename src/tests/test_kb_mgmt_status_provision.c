#include "kb/kb_mgmt_status_provision.h"
#include "modules/vault/vault_server_key.h"

#include <assert.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

struct vault_maintenance_guard
{
   int unused;
};

static struct vault_maintenance_guard g_guard;
static uint8_t g_kek[VAULT_KEK_LEN];
static uint64_t g_hwm = 1;
static int g_hwm_reads, g_hwm_cas_calls, g_cas_mode;
static int g_guard_mode, g_guard_ended;
static atomic_int g_finalize_block, g_finalize_entered;

int vault_maintenance_guard_begin(vault_maintenance_guard_t **guard)
{
   if (g_guard_mode == 1)
      return VAULT_MAINTENANCE_ERROR;
   *guard = &g_guard;
   return VAULT_MAINTENANCE_OK;
}

int vault_maintenance_guard_sync_primary_epoch(vault_maintenance_guard_t *guard, uint64_t epoch)
{
   assert(guard == &g_guard && epoch == 9);
   return g_guard_mode == 2 ? VAULT_MAINTENANCE_EPOCH : VAULT_MAINTENANCE_OK;
}

int vault_maintenance_guard_unseal(vault_maintenance_guard_t *guard, const void *params, size_t len)
{
   assert(guard == &g_guard && !params && !len);
   return g_guard_mode == 3 ? VAULT_MAINTENANCE_ERROR : VAULT_MAINTENANCE_OK;
}

int vault_maintenance_guard_with_active_kek(vault_maintenance_guard_t *guard,
                                            vault_maintenance_kek_fn callback, void *ctx)
{
   assert(guard == &g_guard);
   if (g_guard_mode == 4)
      return VAULT_MAINTENANCE_SEALED;
   if (g_guard_mode == 5)
      return VAULT_MAINTENANCE_ERROR;
   return callback(g_kek, ctx);
}

int vault_maintenance_guard_end(vault_maintenance_guard_t **guard)
{
   assert(*guard == &g_guard);
   *guard = NULL;
   g_guard_ended++;
   return g_guard_mode == 6 ? VAULT_MAINTENANCE_ERROR : VAULT_MAINTENANCE_OK;
}

static void attest(const char *key, uint64_t version, uint8_t out[64])
{
   EVP_MD_CTX *ctx = EVP_MD_CTX_new();
   unsigned int n = 0;
   assert(ctx && EVP_DigestInit_ex(ctx, EVP_sha512(), NULL) == 1);
   assert(EVP_DigestUpdate(ctx, key, strlen(key)) == 1);
   assert(EVP_DigestUpdate(ctx, &version, sizeof(version)) == 1);
   assert(EVP_DigestFinal_ex(ctx, out, &n) == 1 && n == 64);
   EVP_MD_CTX_free(ctx);
}

int vault_hwm_verify(const char *key, uint64_t version, const uint8_t *att, size_t len)
{
   uint8_t expected[64];
   if (!key || !version || !att || len != sizeof(expected))
      return -1;
   attest(key, version, expected);
   int rc = CRYPTO_memcmp(expected, att, sizeof(expected)) ? -1 : 0;
   OPENSSL_cleanse(expected, sizeof(expected));
   return rc;
}

int vault_hwm_read(const char *key, uint64_t *version, uint8_t *att, size_t cap, size_t *len)
{
   g_hwm_reads++;
   if (g_cas_mode == 3 || cap < 64)
      return -1;
   *version = g_hwm;
   attest(key, g_hwm, att);
   *len = 64;
   return 0;
}

int vault_hwm_cas(const char *key, uint64_t expected, uint64_t next, uint8_t *att, size_t cap,
                  size_t *len)
{
   g_hwm_cas_calls++;
   if (cap < 64 || expected != 1 || next != 2)
      return -1;
   if (g_cas_mode == 1)
      return -1;
   if (g_cas_mode == 2)
   {
      g_hwm = 2;
      return -1;
   }
   g_hwm = 2;
   attest(key, 2, att);
   *len = 64;
   return 0;
}

typedef struct
{
   kb_mgmt_status_provision_record_t record;
   int have_record;
   int enabled;
   int inspect_calls;
   int stage_calls;
   int prepare_calls;
   int finalize_calls;
   kb_mgmt_status_provision_db_result_t inspect_rc;
   kb_mgmt_status_provision_db_result_t stage_rc;
   kb_mgmt_status_provision_db_result_t prepare_rc;
   kb_mgmt_status_provision_db_result_t finalize_rc;
} mock_db_t;

static kb_mgmt_status_provision_db_result_t inspect_db(void *opaque, const char *key,
                                                       kb_mgmt_status_provision_record_t *record)
{
   mock_db_t *db = opaque;
   db->inspect_calls++;
   if (db->inspect_rc != KB_MGMT_STATUS_PROVISION_DB_OK)
      return db->inspect_rc;
   if (!db->have_record)
   {
      memset(record, 0, sizeof(*record));
      record->phase = KB_MGMT_STATUS_PROVISION_EMPTY;
      record->seal_epoch = 9;
      return KB_MGMT_STATUS_PROVISION_DB_OK;
   }
   *record = db->record;
   record->phase = db->enabled ? KB_MGMT_STATUS_PROVISION_ENABLED : KB_MGMT_STATUS_PROVISION_STAGED;
   assert(!strcmp(record->custody_key_id, key));
   return KB_MGMT_STATUS_PROVISION_DB_OK;
}

static kb_mgmt_status_provision_db_result_t
stage_db(void *opaque, const kb_mgmt_status_provision_record_t *record)
{
   mock_db_t *db = opaque;
   db->stage_calls++;
   if (db->stage_rc != KB_MGMT_STATUS_PROVISION_DB_OK)
      return db->stage_rc;
   assert(!db->have_record && record->phase == KB_MGMT_STATUS_PROVISION_STAGED);
   db->record = *record;
   db->have_record = 1;
   return KB_MGMT_STATUS_PROVISION_DB_OK;
}

static kb_mgmt_status_provision_db_result_t
finalize_db(void *opaque, const kb_mgmt_status_provision_record_t *record, const uint8_t *att,
            size_t len)
{
   mock_db_t *db = opaque;
   db->finalize_calls++;
   if (db->finalize_rc != KB_MGMT_STATUS_PROVISION_DB_OK)
      return db->finalize_rc;
   assert(db->have_record && !memcmp(record, &db->record, sizeof(*record)));
   assert(vault_hwm_verify(record->custody_key_id, 2, att, len) == 0);
   atomic_store(&g_finalize_entered, 1);
   while (atomic_load(&g_finalize_block))
      sched_yield();
   memcpy(db->record.hwm2_attestation, att, len);
   db->record.hwm2_attestation_len = len;
   db->enabled = 1;
   return KB_MGMT_STATUS_PROVISION_DB_OK;
}

static kb_mgmt_status_provision_db_result_t
prepare_db(void *opaque, const kb_mgmt_status_provision_record_t *record)
{
   mock_db_t *db = opaque;
   db->prepare_calls++;
   if (db->prepare_rc != KB_MGMT_STATUS_PROVISION_DB_OK)
      return db->prepare_rc;
   assert(db->have_record && !memcmp(record, &db->record, sizeof(*record)));
   return KB_MGMT_STATUS_PROVISION_DB_OK;
}

static kb_mgmt_status_provision_db_t seam(mock_db_t *db)
{
   kb_mgmt_status_provision_db_t value = {.inspect = inspect_db,
                                          .stage = stage_db,
                                          .prepare_activation = prepare_db,
                                          .finalize = finalize_db,
                                          .ctx = db};
   return value;
}

static void reset_globals(void)
{
   for (size_t i = 0; i < sizeof(g_kek); ++i)
      g_kek[i] = (uint8_t)(0x80 + i);
   g_hwm = 1;
   g_hwm_reads = 0;
   g_hwm_cas_calls = 0;
   g_cas_mode = 0;
   g_guard_mode = 0;
   g_guard_ended = 0;
   atomic_store(&g_finalize_block, 0);
   atomic_store(&g_finalize_entered, 0);
}

static int decrypt(const kb_mgmt_status_provision_envelope_t *envelope, uint8_t out[32])
{
   uint8_t dek[VAULT_DEK_LEN], aad[VAULT_ENVELOPE_AAD_MAX];
   size_t aad_len = 0;
   int rc = vault_aad_build_v2("org:p5-status", "management", "ed25519", envelope->version, aad,
                               sizeof(aad), &aad_len) ||
                    vault_dek_unwrap(g_kek, envelope->wrapped_dek, dek) ||
                    vault_secret_decrypt(dek, aad, aad_len, envelope->nonce, envelope->ciphertext,
                                         envelope->ciphertext_len, envelope->tag, out)
                ? -1
                : 0;
   OPENSSL_cleanse(dek, sizeof(dek));
   OPENSSL_cleanse(aad, sizeof(aad));
   return rc;
}

static int zero_output(const kb_mgmt_status_provision_output_t *out)
{
   kb_mgmt_status_provision_output_t zero = {0};
   return !memcmp(out, &zero, sizeof(zero));
}

static void test_fresh_and_crypto(mock_db_t *saved)
{
   reset_globals();
   mock_db_t db = {0};
   kb_mgmt_status_provision_db_t callbacks = seam(&db);
   kb_mgmt_status_provision_output_t out;
   memset(&out, 0xa5, sizeof(out));
   assert(kb_mgmt_status_provision("kms:p5-status", &callbacks, &out) ==
          KB_MGMT_STATUS_PROVISION_FRESH);
   assert(db.have_record && db.enabled && db.stage_calls == 1 && db.prepare_calls == 1 &&
          db.finalize_calls == 1);
   assert(!strcmp(out.custody_key_id, "kms:p5-status"));
   assert(!strcmp(out.wire_key_id, db.record.wire_key_id));
   char bootstrap_id[65];
   assert(!kb_mgmt_status_provision_bootstrap_id("kms:p5-status", bootstrap_id));
   assert(strlen(bootstrap_id) == 64 && !strcmp(bootstrap_id, db.record.bootstrap_id));
   assert(!memcmp(out.public_key, db.record.public_key, sizeof(out.public_key)));
   assert(!strncmp(out.wire_key_id, "p5-status-v1-", 13) && strlen(out.wire_key_id) == 45);
   assert(g_hwm == 2 && g_hwm_cas_calls == 1 && g_guard_ended == 1);

   uint8_t s1[32], s2[32], digest[32];
   assert(!decrypt(&db.record.v1, s1) && !decrypt(&db.record.v2, s2));
   assert(!CRYPTO_memcmp(s1, s2, sizeof(s1)));
   assert(CRYPTO_memcmp(db.record.v1.wrapped_dek, db.record.v2.wrapped_dek,
                        sizeof(db.record.v1.wrapped_dek)));
   assert(CRYPTO_memcmp(db.record.v1.nonce, db.record.v2.nonce, sizeof(db.record.v1.nonce)));
   assert(!kb_mgmt_status_provision_envelope_digest(&db.record.v1, digest));
   assert(!CRYPTO_memcmp(digest, db.record.v1_digest, sizeof(digest)));
   assert(!kb_mgmt_status_provision_envelope_digest(&db.record.v2, digest));
   assert(!CRYPTO_memcmp(digest, db.record.v2_digest, sizeof(digest)));
   OPENSSL_cleanse(s1, sizeof(s1));
   OPENSSL_cleanse(s2, sizeof(s2));
   OPENSSL_cleanse(digest, sizeof(digest));
   *saved = db;
}

static void test_recovery(const mock_db_t *saved)
{
   reset_globals();
   mock_db_t db = *saved;
   db.enabled = 0;
   db.stage_calls = db.prepare_calls = db.finalize_calls = 0;
   kb_mgmt_status_provision_db_t callbacks = seam(&db);
   kb_mgmt_status_provision_output_t out;
   memset(&out, 0xa5, sizeof(out));
   assert(kb_mgmt_status_provision("kms:p5-status", &callbacks, &out) ==
          KB_MGMT_STATUS_PROVISION_RECOVERED);
   assert(zero_output(&out) && db.enabled && !db.stage_calls && db.prepare_calls == 1 &&
          db.finalize_calls == 1);
   assert(g_hwm == 2 && g_hwm_cas_calls == 1 && g_guard_ended == 1);

   reset_globals();
   db = *saved;
   db.enabled = 0;
   db.stage_calls = db.prepare_calls = db.finalize_calls = 0;
   callbacks = seam(&db);
   g_hwm = 2;
   memset(&out, 0xa5, sizeof(out));
   assert(kb_mgmt_status_provision("kms:p5-status", &callbacks, &out) ==
          KB_MGMT_STATUS_PROVISION_RECOVERED);
   assert(zero_output(&out) && db.finalize_calls == 1 && !g_hwm_cas_calls);
}

static void test_cas_ambiguity(void)
{
   reset_globals();
   mock_db_t db = {0};
   kb_mgmt_status_provision_db_t callbacks = seam(&db);
   kb_mgmt_status_provision_output_t out;
   g_cas_mode = 2;
   assert(kb_mgmt_status_provision("kms:p5-status", &callbacks, &out) ==
          KB_MGMT_STATUS_PROVISION_FRESH);
   assert(g_hwm_reads == 2 && db.enabled);

   reset_globals();
   memset(&db, 0, sizeof(db));
   callbacks = seam(&db);
   g_cas_mode = 1;
   memset(&out, 0xa5, sizeof(out));
   assert(kb_mgmt_status_provision("kms:p5-status", &callbacks, &out) ==
          KB_MGMT_STATUS_PROVISION_RETRY);
   assert(zero_output(&out) && db.have_record && !db.enabled && !db.finalize_calls);
}

static void test_refusals(const mock_db_t *saved)
{
   reset_globals();
   mock_db_t db = *saved;
   kb_mgmt_status_provision_db_t callbacks = seam(&db);
   kb_mgmt_status_provision_output_t out;
   g_hwm = 2;
   memset(&out, 0xa5, sizeof(out));
   assert(kb_mgmt_status_provision("kms:p5-status", &callbacks, &out) ==
          KB_MGMT_STATUS_PROVISION_CONFLICT);
   assert(zero_output(&out) && g_hwm_reads == 1 && g_guard_ended == 1);

   reset_globals();
   db = *saved;
   db.record.hwm2_attestation[0] ^= 1;
   callbacks = seam(&db);
   assert(kb_mgmt_status_provision("kms:p5-status", &callbacks, &out) ==
          KB_MGMT_STATUS_PROVISION_INTEGRITY);
   assert(zero_output(&out) && !g_hwm_reads && !g_guard_ended);

   reset_globals();
   db = *saved;
   g_hwm = 1;
   callbacks = seam(&db);
   assert(kb_mgmt_status_provision("kms:p5-status", &callbacks, &out) ==
          KB_MGMT_STATUS_PROVISION_INTEGRITY);
   assert(zero_output(&out) && g_hwm_reads == 1 && !g_guard_ended);

   reset_globals();
   db = *saved;
   db.record.v2.ciphertext[0] ^= 1;
   assert(!kb_mgmt_status_provision_envelope_digest(&db.record.v2, db.record.v2_digest));
   g_hwm = 2;
   callbacks = seam(&db);
   assert(kb_mgmt_status_provision("kms:p5-status", &callbacks, &out) ==
          KB_MGMT_STATUS_PROVISION_INTEGRITY);
   assert(zero_output(&out) && g_hwm_reads == 1 && g_guard_ended == 1);

   reset_globals();
   db = *saved;
   db.enabled = 0;
   db.prepare_calls = db.finalize_calls = 0;
   db.record.bootstrap_id[0] ^= 1;
   callbacks = seam(&db);
   assert(kb_mgmt_status_provision("kms:p5-status", &callbacks, &out) ==
          KB_MGMT_STATUS_PROVISION_INTEGRITY);
   assert(zero_output(&out) && !g_hwm_reads && !g_guard_ended);

   reset_globals();
   db = *saved;
   db.enabled = 0;
   db.prepare_calls = db.finalize_calls = 0;
   db.record.hwm1_attestation[0] ^= 1;
   callbacks = seam(&db);
   assert(kb_mgmt_status_provision("kms:p5-status", &callbacks, &out) ==
          KB_MGMT_STATUS_PROVISION_INTEGRITY);
   assert(zero_output(&out) && !g_hwm_reads && !g_guard_ended);

   reset_globals();
   db = *saved;
   db.enabled = 0;
   db.prepare_calls = db.finalize_calls = 0;
   db.record.v2.ciphertext[0] ^= 1;
   callbacks = seam(&db);
   assert(kb_mgmt_status_provision("kms:p5-status", &callbacks, &out) ==
          KB_MGMT_STATUS_PROVISION_INTEGRITY);
   assert(zero_output(&out) && !g_hwm_reads);

   reset_globals();
   db = *saved;
   db.enabled = 0;
   db.prepare_calls = db.finalize_calls = 0;
   db.record.v2.ciphertext[0] ^= 1;
   assert(!kb_mgmt_status_provision_envelope_digest(&db.record.v2, db.record.v2_digest));
   callbacks = seam(&db);
   assert(kb_mgmt_status_provision("kms:p5-status", &callbacks, &out) ==
          KB_MGMT_STATUS_PROVISION_INTEGRITY);
   assert(zero_output(&out) && g_guard_ended == 1 && !db.finalize_calls);

   reset_globals();
   db = *saved;
   db.enabled = 0;
   db.prepare_calls = db.finalize_calls = 0;
   callbacks = seam(&db);
   g_hwm = 3;
   assert(kb_mgmt_status_provision("kms:p5-status", &callbacks, &out) ==
          KB_MGMT_STATUS_PROVISION_INTEGRITY);
   assert(zero_output(&out) && !db.finalize_calls);

   reset_globals();
   memset(&db, 0, sizeof(db));
   db.stage_rc = KB_MGMT_STATUS_PROVISION_DB_SEALED;
   callbacks = seam(&db);
   assert(kb_mgmt_status_provision("kms:p5-status", &callbacks, &out) ==
          KB_MGMT_STATUS_PROVISION_SEALED);
   assert(zero_output(&out) && !db.have_record && g_guard_ended == 1);

   reset_globals();
   memset(&db, 0, sizeof(db));
   db.prepare_rc = KB_MGMT_STATUS_PROVISION_DB_CONFLICT;
   callbacks = seam(&db);
   assert(kb_mgmt_status_provision("kms:p5-status", &callbacks, &out) ==
          KB_MGMT_STATUS_PROVISION_CONFLICT);
   assert(zero_output(&out) && db.have_record && db.prepare_calls == 1 && !db.finalize_calls &&
          !g_hwm_cas_calls);

   reset_globals();
   memset(&db, 0, sizeof(db));
   db.finalize_rc = KB_MGMT_STATUS_PROVISION_DB_RETRY;
   callbacks = seam(&db);
   assert(kb_mgmt_status_provision("kms:p5-status", &callbacks, &out) ==
          KB_MGMT_STATUS_PROVISION_RETRY);
   assert(zero_output(&out) && db.have_record && db.prepare_calls == 1 && db.finalize_calls == 1 &&
          g_hwm == 2);

   reset_globals();
   memset(&db, 0, sizeof(db));
   callbacks = seam(&db);
   g_guard_mode = 6;
   assert(kb_mgmt_status_provision("kms:p5-status", &callbacks, &out) ==
          KB_MGMT_STATUS_PROVISION_RETRY);
   assert(zero_output(&out) && !db.stage_calls && g_guard_ended == 1);
}

typedef struct
{
   kb_mgmt_status_provision_db_t callbacks;
   kb_mgmt_status_provision_output_t output;
   kb_mgmt_status_provision_result_t result;
} cancel_call_t;

static void *provision_thread(void *opaque)
{
   cancel_call_t *call = opaque;
   call->result = kb_mgmt_status_provision("kms:p5-status", &call->callbacks, &call->output);
   return NULL;
}

static void test_cancellation_cleanup(const mock_db_t *saved)
{
   reset_globals();
   mock_db_t db = *saved;
   db.enabled = 0;
   db.prepare_calls = db.finalize_calls = 0;
   g_hwm = 2;
   atomic_store(&g_finalize_block, 1);
   cancel_call_t call = {.callbacks = seam(&db), .result = KB_MGMT_STATUS_PROVISION_INTEGRITY};
   memset(&call.output, 0xa5, sizeof(call.output));
   pthread_t thread;
   assert(!pthread_create(&thread, NULL, provision_thread, &call));
   while (!atomic_load(&g_finalize_entered))
      sched_yield();
   assert(!pthread_cancel(thread));
   atomic_store(&g_finalize_block, 0);
   void *result = NULL;
   assert(!pthread_join(thread, &result));
   assert(result == PTHREAD_CANCELED && db.enabled && db.finalize_calls == 1 &&
          g_guard_ended == 1 && zero_output(&call.output));
}

int main(void)
{
   mock_db_t saved;
   test_fresh_and_crypto(&saved);
   test_recovery(&saved);
   test_cas_ambiguity();
   test_refusals(&saved);
   test_cancellation_cleanup(&saved);
   puts("kb_mgmt_status_provision: all tests passed");
   return 0;
}
