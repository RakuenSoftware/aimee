/* P7-reseal-c standalone real-Postgres mock driver.
 *
 * This is deliberately not part of TEST_TARGETS: it requires an otherwise empty
 * scratch vault on real Postgres and drives the owner-only whole-vault staging API.
 * It models custody with two in-memory KEKs, but every DEK and verifier re-wrap is
 * the production RFC 3394 AES-KW implementation. */
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_internal.h"
#include "modules/db2/c/db_postgres.h"
#include "modules/db2/c/org_vault_rewrap.h"
#include "modules/vault/vault_crypto.h"

#include <assert.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This is a test executable, but calls with side effects must remain active in
 * NDEBUG builds.  Keep assert-style call sites while making them unconditional. */
static void check_failed(const char *expr, const char *file, int line)
{
   fprintf(stderr, "p7-rewrap-live: check failed at %s:%d: %s\n", file, line, expr);
   exit(EXIT_FAILURE);
}
#undef assert
#define assert(expr)                                                                               \
   do                                                                                              \
   {                                                                                               \
      if (!(expr))                                                                                 \
         check_failed(#expr, __FILE__, __LINE__);                                                  \
   } while (0)

#define SECRET_ROWS 521
#define PAGE_LIMIT  73
#define CHECK_ROWS  6
#define ERR_CAP     512

static const char *const OP = "7123456789abcdef0123456789abcdef";
static const char *const PRINCIPALS[CHECK_ROWS] = {"p7-rewrap-main",       "p7-rewrap-empty",
                                                   "p7-rewrap-check-only", "p7-rewrap-delim|:name",
                                                   "p7-rewrap-utf8-Î¼",    "p7-rewrap-empty-2"};

typedef struct
{
   int64_t source_id;
   int64_t version;
   char principal[601];
   char agent[256];
   char cred[256];
   uint8_t source_digest[32];
   uint8_t wrapped[VAULT_WRAPPED_DEK_LEN];
} secret_row_t;

typedef struct
{
   char principal[601];
   uint8_t source_digest[32];
   uint8_t check[VAULT_WRAPPED_DEK_LEN];
   size_t check_len;
} check_row_t;

static void die_pg(const char *what, const char *err)
{
   fprintf(stderr, "p7-rewrap-live: %s: %s\n", what, err ? err : "unknown postgres error");
   exit(EXIT_FAILURE);
}

static void exec_sql(const char *sql)
{
   char err[ERR_CAP] = "";
   if (aimee_pg_exec(db2_conn(), sql, err, sizeof(err)) != 0)
      die_pg(sql, err);
}

static int64_t scalar_i64(const char *sql)
{
   char err[ERR_CAP] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   if (!st)
      die_pg(sql, err);
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
      die_pg(sql, err);
   int64_t value = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return value;
}

static int all_zero(const void *p, size_t n)
{
   const uint8_t *b = p;
   uint8_t acc = 0;
   for (size_t i = 0; i < n; i++)
      acc |= b[i];
   return acc == 0;
}

static void make_key(uint8_t key[VAULT_KEK_LEN], uint8_t seed)
{
   for (size_t i = 0; i < VAULT_KEK_LEN; i++)
      key[i] = (uint8_t)(seed + i * 17u);
}

static void make_dek(int64_t version, uint8_t dek[VAULT_DEK_LEN])
{
   for (size_t i = 0; i < VAULT_DEK_LEN; i++)
      dek[i] = (uint8_t)((uint64_t)version * 29u + i * 11u + 3u);
}

static void make_check_plain(size_t principal_index, uint8_t plain[VAULT_DEK_LEN])
{
   for (size_t i = 0; i < VAULT_DEK_LEN; i++)
      plain[i] = (uint8_t)(0x80u + principal_index * 7u + i);
}

static void seed_salts(const uint8_t old_kek[VAULT_KEK_LEN])
{
   static const size_t empty[] = {1, 5};
   for (size_t i = 0; i < CHECK_ROWS; i++)
   {
      uint8_t salt[VAULT_SALT_LEN];
      uint8_t plain[VAULT_DEK_LEN] = {0};
      uint8_t wrapped[VAULT_WRAPPED_DEK_LEN] = {0};
      for (size_t j = 0; j < sizeof(salt); j++)
         salt[j] = (uint8_t)(i * 19u + j + 1u);
      int is_empty = i == empty[0] || i == empty[1];
      if (!is_empty)
      {
         make_check_plain(i, plain);
         assert(vault_dek_wrap(old_kek, plain, wrapped) == 0);
      }

      char err[ERR_CAP] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(
          db2_conn(), "INSERT INTO org_vault_salt(principal,salt,kek_check) VALUES(?1,?2,?3)", err,
          sizeof(err));
      if (!st)
         die_pg("prepare salt insert", err);
      assert(aimee_pg_bind_text(st, "?1", PRINCIPALS[i]) == 0);
      assert(aimee_pg_bind_blob(st, "?2", salt, sizeof(salt)) == 0);
      assert(aimee_pg_bind_blob(st, "?3", wrapped, is_empty ? 0 : VAULT_WRAPPED_DEK_LEN) == 0);
      if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ERR)
         die_pg("insert salt", err);
      aimee_pg_finalize(st);
      OPENSSL_cleanse(salt, sizeof(salt));
      OPENSSL_cleanse(plain, sizeof(plain));
      OPENSSL_cleanse(wrapped, sizeof(wrapped));
      assert(all_zero(plain, sizeof(plain)) && all_zero(wrapped, sizeof(wrapped)));
   }
}

static void seed_secrets(const uint8_t old_kek[VAULT_KEK_LEN])
{
   char err[ERR_CAP] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(),
       "INSERT INTO org_vault_secret(principal,team_id,agent,cred,version,wrapped_dek,nonce,"
       "ciphertext,tag) VALUES(?1,NULL,?2,?3,?4,?5,?6,?7,?8)",
       err, sizeof(err));
   if (!st)
      die_pg("prepare secret insert", err);
   for (int64_t version = 1; version <= SECRET_ROWS; version++)
   {
      uint8_t dek[VAULT_DEK_LEN];
      uint8_t wrapped[VAULT_WRAPPED_DEK_LEN];
      uint8_t nonce[VAULT_GCM_NONCE_LEN];
      uint8_t ciphertext[24];
      uint8_t tag[VAULT_GCM_TAG_LEN];
      make_dek(version, dek);
      assert(vault_dek_wrap(old_kek, dek, wrapped) == 0);
      for (size_t i = 0; i < sizeof(nonce); i++)
         nonce[i] = (uint8_t)(version + (int64_t)i);
      for (size_t i = 0; i < sizeof(ciphertext); i++)
         ciphertext[i] = (uint8_t)(version * 3 + (int64_t)i);
      for (size_t i = 0; i < sizeof(tag); i++)
         tag[i] = (uint8_t)(version * 5 + (int64_t)i);

      assert(aimee_pg_reset(st) == 0);
      assert(aimee_pg_bind_text(st, "?1", PRINCIPALS[0]) == 0);
      assert(aimee_pg_bind_text(st, "?2", version & 1 ? "bedrock" : "agent|delim") == 0);
      assert(aimee_pg_bind_text(st, "?3", version & 1 ? "primary" : "cred:name") == 0);
      assert(aimee_pg_bind_int64(st, "?4", version) == 0);
      assert(aimee_pg_bind_blob(st, "?5", wrapped, sizeof(wrapped)) == 0);
      assert(aimee_pg_bind_blob(st, "?6", nonce, sizeof(nonce)) == 0);
      assert(aimee_pg_bind_blob(st, "?7", ciphertext, sizeof(ciphertext)) == 0);
      assert(aimee_pg_bind_blob(st, "?8", tag, sizeof(tag)) == 0);
      if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ERR)
         die_pg("insert secret", err);
      OPENSSL_cleanse(dek, sizeof(dek));
      OPENSSL_cleanse(wrapped, sizeof(wrapped));
      assert(all_zero(dek, sizeof(dek)) && all_zero(wrapped, sizeof(wrapped)));
   }
   aimee_pg_finalize(st);
   exec_sql("INSERT INTO org_vault_current(principal,agent,cred,version) "
            "VALUES('p7-rewrap-main','bedrock','primary',521)");
   exec_sql("CREATE TEMP TABLE p7_rewrap_cipher_snapshot ON COMMIT PRESERVE ROWS AS "
            "SELECT id,nonce,ciphertext,tag FROM org_vault_secret");
}

static void verify_bad_old_material(const uint8_t old_kek[VAULT_KEK_LEN],
                                    const uint8_t wrong_kek[VAULT_KEK_LEN])
{
   char err[ERR_CAP] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(db2_conn(), "SELECT wrapped_dek FROM org_vault_secret ORDER BY id LIMIT 1",
                        err, sizeof(err));
   if (!st)
      die_pg("prepare bad-material probe", err);
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
      die_pg("read bad-material probe", err);
   const void *p = aimee_pg_column_blob(st, 0);
   int n = aimee_pg_column_bytes(st, 0);
   assert(p && n == VAULT_WRAPPED_DEK_LEN);
   uint8_t wrapped[VAULT_WRAPPED_DEK_LEN];
   uint8_t corrupt[VAULT_WRAPPED_DEK_LEN];
   uint8_t out[VAULT_DEK_LEN];
   memcpy(wrapped, p, sizeof(wrapped));
   memcpy(corrupt, wrapped, sizeof(corrupt));
   aimee_pg_finalize(st);

   memset(out, 0xa5, sizeof(out));
   assert(vault_dek_unwrap(wrong_kek, wrapped, out) != 0);
   assert(all_zero(out, sizeof(out)));
   corrupt[VAULT_WRAPPED_DEK_LEN / 2] ^= 0x80;
   memset(out, 0xa5, sizeof(out));
   assert(vault_dek_unwrap(old_kek, corrupt, out) != 0);
   assert(all_zero(out, sizeof(out)));
   OPENSSL_cleanse(wrapped, sizeof(wrapped));
   OPENSSL_cleanse(corrupt, sizeof(corrupt));
   OPENSSL_cleanse(out, sizeof(out));
}

static int64_t begin_operation(void)
{
   char err[ERR_CAP] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "SELECT fencing_token FROM org_vault_rewrap_begin(?1,?2,?3,?4,?5)", err,
       sizeof(err));
   if (!st)
      die_pg("prepare begin", err);
   assert(aimee_pg_bind_text(st, "?1", "p7-rewrap-live-driver") == 0);
   assert(aimee_pg_bind_text(st, "?2", "p7-rewrap-live-request") == 0);
   assert(aimee_pg_bind_text(st, "?3", OP) == 0);
   assert(aimee_pg_bind_int64(st, "?4", 41) == 0);
   assert(aimee_pg_bind_int64(st, "?5", 42) == 0);
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
      die_pg("begin operation", err);
   int64_t fence = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   assert(fence > 0);
   return fence;
}

static void record_prepared(int64_t fence, uint8_t receipt_digest[32])
{
   vault_tpm2_reseal_receipt_t decoded = {0};
   uint8_t receipt[VAULT_RESEAL_RECEIPT_V1_LEN] = {0};
   assert(vault_reseal_operation_id_from_hex(OP, decoded.operation_id) == 0);
   decoded.old_generation = 41;
   decoded.new_generation = 42;
   for (size_t i = 0; i < 32; i++)
   {
      decoded.predecessor_digest[i] = (uint8_t)(0x10 + i);
      decoded.capsule_digest[i] = (uint8_t)(0x30 + i);
      decoded.future_digest[i] = (uint8_t)(0x50 + i);
      decoded.new_kek_digest[i] = (uint8_t)(0x70 + i);
      decoded.manifest_digest[i] = (uint8_t)(0x90 + i);
   }
   assert(vault_reseal_receipt_encode(&decoded, receipt) == 0);
   assert(vault_reseal_receipt_digest(receipt, receipt_digest) == 0);
   char err[ERR_CAP] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "SELECT org_vault_rewrap_record_prepared(?1,?2,?3,?4)", err, sizeof(err));
   if (!st)
      die_pg("prepare record_prepared", err);
   assert(aimee_pg_bind_text(st, "?1", OP) == 0);
   assert(aimee_pg_bind_int64(st, "?2", fence) == 0);
   assert(aimee_pg_bind_blob(st, "?3", receipt, sizeof(receipt)) == 0);
   assert(aimee_pg_bind_blob(st, "?4", receipt_digest, 32) == 0);
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
      die_pg("record prepared", err);
   const char *state = aimee_pg_column_text(st, 0);
   assert(state && strcmp(state, "custody_prepared") == 0);
   aimee_pg_finalize(st);
   OPENSSL_cleanse(&decoded, sizeof(decoded));
   OPENSSL_cleanse(receipt, sizeof(receipt));
}

static void stage_secret(const secret_row_t *row, int64_t fence,
                         const uint8_t old_kek[VAULT_KEK_LEN], const uint8_t new_kek[VAULT_KEK_LEN])
{
   uint8_t dek[VAULT_DEK_LEN] = {0};
   uint8_t new_wrapped[VAULT_WRAPPED_DEK_LEN] = {0};
   assert(vault_dek_unwrap(old_kek, row->wrapped, dek) == 0);
   assert(vault_dek_wrap(new_kek, dek, new_wrapped) == 0);
   char err[ERR_CAP] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(db2_conn(), "SELECT org_vault_rewrap_stage_dek(?1,?2,?3,?4,?5,?6,?7,?8,?9)",
                        err, sizeof(err));
   if (!st)
      die_pg("prepare stage_dek", err);
   assert(aimee_pg_bind_text(st, "?1", OP) == 0);
   assert(aimee_pg_bind_int64(st, "?2", fence) == 0);
   assert(aimee_pg_bind_int64(st, "?3", row->source_id) == 0);
   assert(aimee_pg_bind_text(st, "?4", row->principal) == 0);
   assert(aimee_pg_bind_text(st, "?5", row->agent) == 0);
   assert(aimee_pg_bind_text(st, "?6", row->cred) == 0);
   assert(aimee_pg_bind_int64(st, "?7", row->version) == 0);
   assert(aimee_pg_bind_blob(st, "?8", row->source_digest, sizeof(row->source_digest)) == 0);
   assert(aimee_pg_bind_blob(st, "?9", new_wrapped, sizeof(new_wrapped)) == 0);
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ERR)
      die_pg("stage DEK", err);
   aimee_pg_finalize(st);
   OPENSSL_cleanse(dek, sizeof(dek));
   OPENSSL_cleanse(new_wrapped, sizeof(new_wrapped));
   assert(all_zero(dek, sizeof(dek)) && all_zero(new_wrapped, sizeof(new_wrapped)));
}

static int load_secret_page(int64_t after, int64_t fence, secret_row_t page[PAGE_LIMIT])
{
   char err[ERR_CAP] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "SELECT * FROM org_vault_rewrap_secret_page(?1,?2,?3,?4)", err, sizeof(err));
   if (!st)
      die_pg("prepare secret page", err);
   assert(aimee_pg_bind_text(st, "?1", OP) == 0);
   assert(aimee_pg_bind_int64(st, "?2", fence) == 0);
   assert(aimee_pg_bind_int64(st, "?3", after) == 0);
   assert(aimee_pg_bind_int(st, "?4", PAGE_LIMIT) == 0);
   int n = 0;
   int rc;
   while ((rc = aimee_pg_step(st, err, sizeof(err))) == AIMEE_PG_ROW)
   {
      assert(n < PAGE_LIMIT);
      page[n].source_id = aimee_pg_column_int64(st, 0);
      const char *p = aimee_pg_column_text(st, 1);
      const char *a = aimee_pg_column_text(st, 2);
      const char *c = aimee_pg_column_text(st, 3);
      assert(p && a && c);
      snprintf(page[n].principal, sizeof(page[n].principal), "%s", p);
      snprintf(page[n].agent, sizeof(page[n].agent), "%s", a);
      snprintf(page[n].cred, sizeof(page[n].cred), "%s", c);
      page[n].version = aimee_pg_column_int64(st, 4);
      const void *digest = aimee_pg_column_blob(st, 5);
      int digest_len = aimee_pg_column_bytes(st, 5);
      assert(digest && digest_len == 32);
      memcpy(page[n].source_digest, digest, 32);
      const void *wrapped = aimee_pg_column_blob(st, 6);
      int wrapped_len = aimee_pg_column_bytes(st, 6);
      assert(wrapped && wrapped_len == VAULT_WRAPPED_DEK_LEN);
      memcpy(page[n].wrapped, wrapped, VAULT_WRAPPED_DEK_LEN);
      n++;
   }
   if (rc != AIMEE_PG_DONE)
      die_pg("read secret page", err);
   aimee_pg_finalize(st);
   return n;
}

static void stage_check(const check_row_t *row, size_t principal_index, int64_t fence,
                        const uint8_t old_kek[VAULT_KEK_LEN], const uint8_t new_kek[VAULT_KEK_LEN])
{
   uint8_t plain[VAULT_DEK_LEN] = {0};
   uint8_t new_check[VAULT_WRAPPED_DEK_LEN] = {0};
   size_t new_len = 0;
   if (row->check_len)
   {
      uint8_t expected[VAULT_DEK_LEN];
      make_check_plain(principal_index, expected);
      assert(vault_dek_unwrap(old_kek, row->check, plain) == 0);
      assert(CRYPTO_memcmp(plain, expected, sizeof(plain)) == 0);
      assert(vault_dek_wrap(new_kek, plain, new_check) == 0);
      new_len = sizeof(new_check);
      OPENSSL_cleanse(expected, sizeof(expected));
   }
   char err[ERR_CAP] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "SELECT org_vault_rewrap_stage_check(?1,?2,?3,?4,?5)", err, sizeof(err));
   if (!st)
      die_pg("prepare stage_check", err);
   assert(aimee_pg_bind_text(st, "?1", OP) == 0);
   assert(aimee_pg_bind_int64(st, "?2", fence) == 0);
   assert(aimee_pg_bind_text(st, "?3", row->principal) == 0);
   assert(aimee_pg_bind_blob(st, "?4", row->source_digest, 32) == 0);
   assert(aimee_pg_bind_blob(st, "?5", new_check, (int)new_len) == 0);
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ERR)
      die_pg("stage check", err);
   aimee_pg_finalize(st);
   OPENSSL_cleanse(plain, sizeof(plain));
   OPENSSL_cleanse(new_check, sizeof(new_check));
   assert(all_zero(plain, sizeof(plain)) && all_zero(new_check, sizeof(new_check)));
}

static int load_check_page(const uint8_t *after, size_t after_len, int64_t fence,
                           check_row_t page[PAGE_LIMIT], uint8_t next[640], size_t *next_len)
{
   char err[ERR_CAP] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "SELECT * FROM org_vault_rewrap_check_page(?1,?2,?3,?4)", err, sizeof(err));
   if (!st)
      die_pg("prepare check page", err);
   assert(aimee_pg_bind_text(st, "?1", OP) == 0);
   assert(aimee_pg_bind_int64(st, "?2", fence) == 0);
   assert(aimee_pg_bind_blob(st, "?3", after, (int)after_len) == 0);
   assert(aimee_pg_bind_int(st, "?4", PAGE_LIMIT) == 0);
   int n = 0;
   int rc;
   while ((rc = aimee_pg_step(st, err, sizeof(err))) == AIMEE_PG_ROW)
   {
      assert(n < PAGE_LIMIT);
      const char *principal = aimee_pg_column_text(st, 0);
      assert(principal);
      snprintf(page[n].principal, sizeof(page[n].principal), "%s", principal);
      const void *digest = aimee_pg_column_blob(st, 1);
      int digest_len = aimee_pg_column_bytes(st, 1);
      assert(digest && digest_len == 32);
      memcpy(page[n].source_digest, digest, 32);
      int check_len = aimee_pg_column_bytes(st, 2);
      const void *check = aimee_pg_column_blob(st, 2);
      assert(check_len == 0 || (check && check_len == VAULT_WRAPPED_DEK_LEN));
      if (check_len)
         memcpy(page[n].check, check, (size_t)check_len);
      page[n].check_len = (size_t)check_len;
      const void *cursor = aimee_pg_column_blob(st, 3);
      int cursor_len = aimee_pg_column_bytes(st, 3);
      assert(cursor && cursor_len > 0 && cursor_len <= 640);
      memcpy(next, cursor, (size_t)cursor_len);
      *next_len = (size_t)cursor_len;
      n++;
   }
   if (rc != AIMEE_PG_DONE)
      die_pg("read check page", err);
   aimee_pg_finalize(st);
   return n;
}

static size_t principal_index(const char *principal)
{
   for (size_t i = 0; i < CHECK_ROWS; i++)
      if (strcmp(principal, PRINCIPALS[i]) == 0)
         return i;
   assert(!"unexpected principal");
   return 0;
}

static void stage_all(int64_t fence, const uint8_t old_kek[VAULT_KEK_LEN],
                      const uint8_t new_kek[VAULT_KEK_LEN])
{
   exec_sql("BEGIN ISOLATION LEVEL SERIALIZABLE");
   secret_row_t *secrets = calloc(PAGE_LIMIT, sizeof(*secrets));
   check_row_t *checks = calloc(PAGE_LIMIT, sizeof(*checks));
   assert(secrets && checks);

   int64_t after_id = 0;
   int secret_total = 0;
   int secret_pages = 0;
   for (;;)
   {
      int n = load_secret_page(after_id, fence, secrets);
      assert(n >= 0 && n <= PAGE_LIMIT);
      if (!n)
         break;
      secret_pages++;
      for (int i = 0; i < n; i++)
      {
         stage_secret(&secrets[i], fence, old_kek, new_kek);
         after_id = secrets[i].source_id;
         OPENSSL_cleanse(&secrets[i], sizeof(secrets[i]));
      }
      secret_total += n;
   }
   assert(secret_total == SECRET_ROWS && secret_pages > 7);

   uint8_t after_principal[640] = {0}, next_principal[640] = {0};
   size_t after_principal_len = 0, next_principal_len = 0;
   int check_total = 0;
   for (;;)
   {
      int n = load_check_page(after_principal, after_principal_len, fence, checks, next_principal,
                              &next_principal_len);
      assert(n >= 0 && n <= PAGE_LIMIT);
      if (!n)
         break;
      for (int i = 0; i < n; i++)
      {
         stage_check(&checks[i], principal_index(checks[i].principal), fence, old_kek, new_kek);
         OPENSSL_cleanse(&checks[i], sizeof(checks[i]));
      }
      memcpy(after_principal, next_principal, next_principal_len);
      after_principal_len = next_principal_len;
      check_total += n;
   }
   assert(check_total == CHECK_ROWS);

   char err[ERR_CAP] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(db2_conn(),
                        "SELECT org_vault_rewrap_stage_finish(?1,?2,x.secret_count,x.check_count,"
                        "x.inventory_digest) FROM org_vault_rewrap_inventory_summary(?1,?2) AS x",
                        err, sizeof(err));
   if (!st)
      die_pg("prepare stage_finish", err);
   assert(aimee_pg_bind_text(st, "?1", OP) == 0);
   assert(aimee_pg_bind_int64(st, "?2", fence) == 0);
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
      die_pg("stage finish", err);
   const char *state = aimee_pg_column_text(st, 0);
   assert(state && strcmp(state, "wraps_staged") == 0);
   aimee_pg_finalize(st);
   exec_sql("COMMIT");

   OPENSSL_cleanse(secrets, PAGE_LIMIT * sizeof(*secrets));
   OPENSSL_cleanse(checks, PAGE_LIMIT * sizeof(*checks));
   free(secrets);
   free(checks);
   OPENSSL_cleanse(after_principal, sizeof(after_principal));
}

static void transition(const char *sql, int64_t fence, const uint8_t *digest)
{
   char err[ERR_CAP] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   if (!st)
      die_pg("prepare transition", err);
   assert(aimee_pg_bind_text(st, "?1", OP) == 0);
   assert(aimee_pg_bind_int64(st, "?2", fence) == 0);
   if (digest)
      assert(aimee_pg_bind_blob(st, "?3", digest, 32) == 0);
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
      die_pg("transition", err);
   aimee_pg_finalize(st);
}

static void verify_promoted(const uint8_t old_kek[VAULT_KEK_LEN],
                            const uint8_t new_kek[VAULT_KEK_LEN])
{
   assert(scalar_i64("SELECT count(*) FROM org_vault_secret") == SECRET_ROWS);
   assert(scalar_i64("SELECT count(*) FROM org_vault_salt") == CHECK_ROWS);
   assert(scalar_i64("SELECT count(*) FROM org_vault_secret s JOIN p7_rewrap_cipher_snapshot x "
                     "USING(id) WHERE s.nonce<>x.nonce OR s.ciphertext<>x.ciphertext OR "
                     "s.tag<>x.tag") == 0);
   assert(scalar_i64("SELECT count(*) FROM kb_vault_rewrap_operation WHERE operation_id='"
                     "7123456789abcdef0123456789abcdef' AND state='promoted'") == 1);
   assert(scalar_i64("SELECT count(*) FROM kb_vault_control WHERE singleton=1 AND sealed AND "
                     "maintenance_id='7123456789abcdef0123456789abcdef'") == 1);

   char err[ERR_CAP] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(db2_conn(), "SELECT version,wrapped_dek FROM org_vault_secret ORDER BY id",
                        err, sizeof(err));
   if (!st)
      die_pg("prepare promoted wraps", err);
   int seen = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      int64_t version = aimee_pg_column_int64(st, 0);
      const void *p = aimee_pg_column_blob(st, 1);
      int n = aimee_pg_column_bytes(st, 1);
      assert(p && n == VAULT_WRAPPED_DEK_LEN);
      uint8_t wrapped[VAULT_WRAPPED_DEK_LEN];
      uint8_t expected[VAULT_DEK_LEN];
      uint8_t actual[VAULT_DEK_LEN];
      uint8_t refused[VAULT_DEK_LEN];
      memcpy(wrapped, p, sizeof(wrapped));
      make_dek(version, expected);
      assert(vault_dek_unwrap(new_kek, wrapped, actual) == 0);
      assert(CRYPTO_memcmp(actual, expected, sizeof(actual)) == 0);
      memset(refused, 0xa5, sizeof(refused));
      assert(vault_dek_unwrap(old_kek, wrapped, refused) != 0);
      assert(all_zero(refused, sizeof(refused)));
      OPENSSL_cleanse(wrapped, sizeof(wrapped));
      OPENSSL_cleanse(expected, sizeof(expected));
      OPENSSL_cleanse(actual, sizeof(actual));
      OPENSSL_cleanse(refused, sizeof(refused));
      assert(all_zero(expected, sizeof(expected)) && all_zero(actual, sizeof(actual)));
      seen++;
   }
   aimee_pg_finalize(st);
   assert(seen == SECRET_ROWS);

   st = aimee_pg_prepare(db2_conn(),
                         "SELECT principal,kek_check FROM org_vault_salt "
                         "ORDER BY principal COLLATE \"C\"",
                         err, sizeof(err));
   if (!st)
      die_pg("prepare promoted checks", err);
   seen = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *principal = aimee_pg_column_text(st, 0);
      int n = aimee_pg_column_bytes(st, 1);
      const void *p = aimee_pg_column_blob(st, 1);
      size_t idx = principal_index(principal);
      if (idx == 1 || idx == 5)
         assert(n == 0);
      else
      {
         assert(p && n == VAULT_WRAPPED_DEK_LEN);
         uint8_t wrapped[VAULT_WRAPPED_DEK_LEN];
         uint8_t expected[VAULT_DEK_LEN];
         uint8_t actual[VAULT_DEK_LEN];
         uint8_t refused[VAULT_DEK_LEN];
         memcpy(wrapped, p, sizeof(wrapped));
         make_check_plain(idx, expected);
         assert(vault_dek_unwrap(new_kek, wrapped, actual) == 0);
         assert(CRYPTO_memcmp(actual, expected, sizeof(actual)) == 0);
         memset(refused, 0xa5, sizeof(refused));
         assert(vault_dek_unwrap(old_kek, wrapped, refused) != 0);
         assert(all_zero(refused, sizeof(refused)));
         OPENSSL_cleanse(wrapped, sizeof(wrapped));
         OPENSSL_cleanse(expected, sizeof(expected));
         OPENSSL_cleanse(actual, sizeof(actual));
         OPENSSL_cleanse(refused, sizeof(refused));
      }
      seen++;
   }
   aimee_pg_finalize(st);
   assert(seen == CHECK_ROWS);
}

static int byte_cursor_gt(const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
   size_t common = an < bn ? an : bn;
   int cmp = common ? memcmp(a, b, common) : 0;
   return cmp > 0 || (cmp == 0 && an > bn);
}

static void verify_promoted_bounded(int64_t fence)
{
   char err[ERR_CAP] = "";
   exec_sql("BEGIN ISOLATION LEVEL SERIALIZABLE");
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "SELECT * FROM org_vault_rewrap_verify_summary(?1,?2)", err, sizeof(err));
   assert(st);
   assert(aimee_pg_bind_text(st, "?1", OP) == 0);
   assert(aimee_pg_bind_int64(st, "?2", fence) == 0);
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
      die_pg("org_vault_rewrap_verify_summary", err);
   assert(aimee_pg_column_int64(st, 0) == SECRET_ROWS);
   assert(aimee_pg_column_int64(st, 1) == CHECK_ROWS);
   assert(aimee_pg_column_bytes(st, 2) == 32);
   assert(aimee_pg_column_bytes(st, 3) == 32);
   assert(aimee_pg_column_bytes(st, 4) == 32);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE);
   aimee_pg_finalize(st);

   int64_t after_id = 0;
   int secret_seen = 0;
   do
   {
      st = aimee_pg_prepare(db2_conn(),
                            "SELECT * FROM org_vault_rewrap_verify_secret_page(?1,?2,?3,?4)", err,
                            sizeof(err));
      assert(st);
      assert(aimee_pg_bind_text(st, "?1", OP) == 0);
      assert(aimee_pg_bind_int64(st, "?2", fence) == 0);
      assert(aimee_pg_bind_int64(st, "?3", after_id) == 0);
      assert(aimee_pg_bind_int(st, "?4", 128) == 0);
      int page = 0;
      while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         int64_t id = aimee_pg_column_int64(st, 0);
         assert(id > after_id && aimee_pg_column_bytes(st, 5) == VAULT_WRAPPED_DEK_LEN);
         after_id = id;
         page++;
      }
      aimee_pg_finalize(st);
      secret_seen += page;
      if (page < 128)
         break;
   } while (1);
   assert(secret_seen == SECRET_ROWS);

   uint8_t after_principal[640];
   size_t after_len = 0;
   int check_seen = 0;
   do
   {
      st = aimee_pg_prepare(db2_conn(),
                            "SELECT * FROM org_vault_rewrap_verify_check_page(?1,?2,?3,?4)", err,
                            sizeof(err));
      assert(st);
      assert(aimee_pg_bind_text(st, "?1", OP) == 0);
      assert(aimee_pg_bind_int64(st, "?2", fence) == 0);
      assert(aimee_pg_bind_blob(st, "?3", after_principal, (int)after_len) == 0);
      assert(aimee_pg_bind_int(st, "?4", 128) == 0);
      int page = 0;
      while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         const char *principal = aimee_pg_column_text(st, 0);
         const void *cursor = aimee_pg_column_blob(st, 2);
         int cursor_n = aimee_pg_column_bytes(st, 2);
         size_t principal_n = strlen(principal);
         assert(cursor && cursor_n > 0 && (size_t)cursor_n <= sizeof(after_principal));
         assert(principal_n == (size_t)cursor_n &&
                memcmp(principal, cursor, (size_t)cursor_n) == 0);
         assert(byte_cursor_gt(cursor, (size_t)cursor_n, after_principal, after_len));
         memcpy(after_principal, cursor, (size_t)cursor_n);
         after_len = (size_t)cursor_n;
         assert(aimee_pg_column_bytes(st, 1) == 0 ||
                aimee_pg_column_bytes(st, 1) == VAULT_WRAPPED_DEK_LEN);
         page++;
      }
      aimee_pg_finalize(st);
      check_seen += page;
      if (page < 128)
         break;
   } while (1);
   assert(check_seen == CHECK_ROWS);
   exec_sql("COMMIT");
}

static void verify_typed_wrapper(int64_t fence, const uint8_t new_kek[VAULT_KEK_LEN])
{
   uint8_t opid[16];
   assert(vault_reseal_operation_id_from_hex(OP, opid) == 0);
   db2_vault_rewrap_tx_t *tx = NULL;
   db2_vault_rewrap_verify_summary_t summary;
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_verify_summary(tx, opid, fence, &summary) == DB2_VAULT_REWRAP_OK);
   assert(summary.secret_count == SECRET_ROWS && summary.check_count == CHECK_ROWS);

   int64_t after = 0;
   int64_t secret_seen = 0;
   for (;;)
   {
      db2_vault_rewrap_secret_t rows[DB2_VAULT_REWRAP_PAGE_MAX];
      size_t count = 0;
      assert(db2_vault_rewrap_verify_secret_page(tx, opid, fence, after, DB2_VAULT_REWRAP_PAGE_MAX,
                                                 rows, DB2_VAULT_REWRAP_PAGE_MAX,
                                                 &count) == DB2_VAULT_REWRAP_OK);
      if (count == 0)
         break;
      for (size_t i = 0; i < count; i++)
      {
         uint8_t expected[VAULT_DEK_LEN], actual[VAULT_DEK_LEN];
         make_dek(rows[i].version, expected);
         assert(vault_dek_unwrap(new_kek, rows[i].wrapped_dek, actual) == 0);
         assert(CRYPTO_memcmp(expected, actual, sizeof(actual)) == 0);
         OPENSSL_cleanse(expected, sizeof(expected));
         OPENSSL_cleanse(actual, sizeof(actual));
         after = rows[i].source_id;
      }
      OPENSSL_cleanse(rows, sizeof(rows));
      secret_seen += (int64_t)count;
   }

   db2_vault_rewrap_cursor_t cursor = {0}, next = {0};
   int64_t check_seen = 0;
   for (;;)
   {
      db2_vault_rewrap_check_t rows[DB2_VAULT_REWRAP_PAGE_MAX];
      size_t count = 0;
      assert(db2_vault_rewrap_verify_check_page(tx, opid, fence, &cursor, DB2_VAULT_REWRAP_PAGE_MAX,
                                                rows, DB2_VAULT_REWRAP_PAGE_MAX, &count,
                                                &next) == DB2_VAULT_REWRAP_OK);
      if (count == 0)
      {
         assert(next.len == cursor.len && CRYPTO_memcmp(next.bytes, cursor.bytes, cursor.len) == 0);
         break;
      }
      for (size_t i = 0; i < count; i++)
      {
         size_t idx = principal_index(rows[i].principal);
         if (idx == 1 || idx == 5)
            assert(rows[i].kek_check_len == 0);
         else
         {
            uint8_t expected[VAULT_DEK_LEN], actual[VAULT_DEK_LEN];
            make_check_plain(idx, expected);
            assert(rows[i].kek_check_len == VAULT_WRAPPED_DEK_LEN);
            assert(vault_dek_unwrap(new_kek, rows[i].kek_check, actual) == 0);
            assert(CRYPTO_memcmp(expected, actual, sizeof(actual)) == 0);
            OPENSSL_cleanse(expected, sizeof(expected));
            OPENSSL_cleanse(actual, sizeof(actual));
         }
      }
      cursor = next;
      OPENSSL_cleanse(rows, sizeof(rows));
      check_seen += (int64_t)count;
   }
   assert(db2_vault_rewrap_verify_crypto_ack(tx, opid, fence) == DB2_VAULT_REWRAP_OK);
   db2_vault_rewrap_tx_rollback(&tx);
   assert(tx == NULL);

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_verify_summary(tx, opid, fence, &summary) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_verify_crypto_ack(tx, opid, fence) == DB2_VAULT_REWRAP_INVALID);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_INVALID && tx != NULL);
   db2_vault_rewrap_tx_rollback(&tx);
   db2_vault_rewrap_verify_summary_clear(&summary);
   OPENSSL_cleanse(opid, sizeof(opid));
}

int main(void)
{
   const char *url = getenv("AIMEE_TEST_PG_URL");
   if (!url || !url[0])
   {
      puts("p7-rewrap-live: SKIP (AIMEE_TEST_PG_URL unset; empty real-PG scratch DB required)");
      return 0;
   }
   if (db2_init(url) != 0)
   {
      fputs("p7-rewrap-live: db2_init failed\n", stderr);
      return 1;
   }
   if (scalar_i64("SELECT count(*) FROM org_vault_secret") != 0 ||
       scalar_i64("SELECT count(*) FROM org_vault_salt") != 0 ||
       scalar_i64("SELECT count(*) FROM kb_vault_rewrap_operation") != 0)
   {
      fputs("p7-rewrap-live: requires an empty scratch vault database\n", stderr);
      db2_shutdown();
      return 1;
   }

   uint8_t old_kek[VAULT_KEK_LEN];
   uint8_t new_kek[VAULT_KEK_LEN];
   uint8_t receipt_digest[32] = {0};
   make_key(old_kek, 0x19);
   make_key(new_kek, 0xa7);
   seed_salts(old_kek);
   seed_secrets(old_kek);
   verify_bad_old_material(old_kek, new_kek);

   int64_t fence = begin_operation();
   record_prepared(fence, receipt_digest);
   stage_all(fence, old_kek, new_kek);
   transition("SELECT org_vault_rewrap_mark_committing(?1,?2)", fence, NULL);
   transition("SELECT org_vault_rewrap_mark_resealed(?1,?2,?3)", fence, receipt_digest);
   exec_sql("BEGIN ISOLATION LEVEL SERIALIZABLE");
   transition("SELECT org_vault_rewrap_promote(?1,?2)", fence, NULL);
   exec_sql("COMMIT");
   verify_promoted(old_kek, new_kek);
   verify_promoted_bounded(fence);
   verify_typed_wrapper(fence, new_kek);

   OPENSSL_cleanse(old_kek, sizeof(old_kek));
   OPENSSL_cleanse(new_kek, sizeof(new_kek));
   OPENSSL_cleanse(receipt_digest, sizeof(receipt_digest));
   assert(all_zero(old_kek, sizeof(old_kek)) && all_zero(new_kek, sizeof(new_kek)) &&
          all_zero(receipt_digest, sizeof(receipt_digest)));
   db2_shutdown();
   puts("PASS: P7 rewrap mock staged/promoted 521 AES-KW DEKs with bounded pages");
   return 0;
}
