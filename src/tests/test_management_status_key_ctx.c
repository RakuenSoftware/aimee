#include "modules/db2/c/management_status_key.h"
#include "modules/db2/c/db_postgres.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct aimee_pg_stmt
{
   int stepped;
   int mode;
};
static struct aimee_pg_stmt g_stmt;
static int g_in_tx, g_closed, g_rollbacks;
static unsigned g_bind_mask;
static int g_admit_replay, g_malformed_replay;

enum
{
   MODE_OTHER,
   MODE_ADMIT,
   MODE_STARTUP
};

static int bind_number(const char *key)
{
   assert(key && key[0] == '?');
   int n = 0;
   assert(sscanf(key + 1, "%d", &n) == 1 && n >= 1 && n <= 12);
   assert(!(g_bind_mask & (1u << (unsigned)(n - 1))));
   g_bind_mask |= 1u << (unsigned)(n - 1);
   return n;
}

void *aimee_pg_open(const char *dsn, char *e, size_t n)
{
   (void)e;
   (void)n;
   return dsn && *dsn ? &g_stmt : NULL;
}
void aimee_pg_close(void *c)
{
   assert(c == &g_stmt);
   g_closed++;
}
int aimee_pg_in_transaction(void *c)
{
   assert(c == &g_stmt);
   return g_in_tx;
}
int aimee_pg_exec(void *c, const char *sql, char *e, size_t n)
{
   (void)e;
   (void)n;
   assert(c == &g_stmt);
   if (strstr(sql, "kb_management_status_"))
      assert(strstr(sql, "public.kb_management_status_") != NULL);
   if (!strcmp(sql, "BEGIN"))
   {
      if (g_in_tx)
         return -1;
      g_in_tx = 1;
   }
   else if (!strcmp(sql, "COMMIT"))
      g_in_tx = 0;
   else if (!strcmp(sql, "ROLLBACK"))
   {
      g_in_tx = 0;
      g_rollbacks++;
   }
   return 0;
}
aimee_pg_stmt_t *aimee_pg_prepare(void *c, const char *sql, char *e, size_t n)
{
   (void)e;
   (void)n;
   assert(c == &g_stmt);
   g_stmt.stepped = 0;
   g_stmt.mode = strstr(sql, "kb_management_status_key_admit")
                     ? MODE_ADMIT
                     : (strstr(sql, "startup_status") ? MODE_STARTUP : MODE_OTHER);
   g_bind_mask = 0;
   return &g_stmt;
}
void aimee_pg_finalize(aimee_pg_stmt_t *s)
{
   assert(s == &g_stmt);
}
aimee_pg_step_t aimee_pg_step(aimee_pg_stmt_t *s, char *e, size_t n)
{
   (void)e;
   (void)n;
   return s->stepped++ ? AIMEE_PG_DONE : AIMEE_PG_ROW;
}
int aimee_pg_bind_int64(aimee_pg_stmt_t *s, const char *k, int64_t v)
{
   assert(s == &g_stmt);
   int n = bind_number(k);
   if (s->mode == MODE_ADMIT)
      assert((n == 4 && v == 3) || (n == 11 && v == 2));
   return 0;
}
int aimee_pg_bind_text(aimee_pg_stmt_t *s, const char *k, const char *v)
{
   assert(s == &g_stmt && v);
   int n = bind_number(k);
   if (s->mode == MODE_ADMIT)
   {
      if (n == 1 || n == 5 || n == 8 || n == 10)
         assert(strlen(v) == 64);
      else if (n == 2)
         assert(!strcmp(v, "platform:p5-status"));
      else if (n == 3)
         assert(!strcmp(v, "status-1"));
      else if (n == 6)
         assert(!strcmp(v, "issuer"));
      else if (n == 7)
         assert(!strcmp(v, "01"));
      else if (n == 9)
         assert(!strcmp(v, "server-1"));
      else
         assert(0);
   }
   return 0;
}
int aimee_pg_bind_blob(aimee_pg_stmt_t *s, const char *k, const void *v, int n)
{
   assert(s == &g_stmt && v);
   assert(bind_number(k) == 12 && n == 3 && !memcmp(v, "att", 3));
   return 0;
}
const void *aimee_pg_column_blob(aimee_pg_stmt_t *s, int c)
{
   assert(s == &g_stmt);
   static const unsigned char b[64] = {1};
   return b;
}
int aimee_pg_column_bytes(aimee_pg_stmt_t *s, int c)
{
   assert(s == &g_stmt);
   if (s->mode == MODE_ADMIT)
   {
      static const int sizes[] = {0, 0, 40, 12, 32, 16, 3};
      return c >= 2 && c <= 6 ? sizes[c] : 0;
   }
   return c == 0 ? 40 : (c == 1 ? 12 : (c == 2 ? 32 : (c == 3 ? 16 : 3)));
}
int aimee_pg_column_is_null(aimee_pg_stmt_t *s, int c)
{
   assert(s == &g_stmt);
   return s->mode == MODE_ADMIT && g_admit_replay && c >= 2 && c <= 6
              ? !(g_malformed_replay && c == 2)
              : 0;
}
int64_t aimee_pg_column_int64(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return 7;
}
const char *aimee_pg_column_text(aimee_pg_stmt_t *s, int c)
{
   assert(s == &g_stmt);
   if (s->mode == MODE_ADMIT && c == 0)
      return g_admit_replay ? "f" : "t";
   return c == 1 ? "f" : "t";
}

int main(void)
{
   db2_management_status_key_ctx_t c;
   char e[64] = "";
   assert(db2_management_status_key_ctx_open(&c, "postgres://status", e, sizeof(e)) == 0);
   assert(db2_management_status_key_guard_begin(&c, 7) == 0 && c.transaction_active);
   assert(db2_management_status_key_guard_begin(&c, 7) < 0);
   db2_vault_key_use_envelope_t out;
   assert(db2_management_status_key_candidate(&c, "key", "wire", 1, &out) < 0);
   assert(db2_management_status_key_guard_end(&c, 1) == 0 && !c.transaction_active);
   assert(db2_management_status_key_guard_end(&c, 1) < 0);

   unsigned char attestation[] = "att";
   char use_id[65], digest[65], caller_fp[65], target_fp[65];
   memset(use_id, '1', 64);
   memset(digest, '2', 64);
   memset(caller_fp, '3', 64);
   memset(target_fp, '4', 64);
   use_id[64] = digest[64] = caller_fp[64] = target_fp[64] = 0;
   db2_management_status_admission_t admission = {
       .use_id = use_id,
       .custody_key_id = "platform:p5-status",
       .wire_key_id = "status-1",
       .version = 3,
       .request_digest = digest,
       .caller_issuer = "issuer",
       .caller_serial_norm = "01",
       .caller_fingerprint = caller_fp,
       .target_server_id = "server-1",
       .target_mgmt_fingerprint = target_fp,
       .revocation_generation = 2,
       .hwm_attestation = attestation,
       .hwm_attestation_len = 3,
   };
   memset(&out, 0, sizeof(out));
   assert(db2_management_status_key_admit(&c, &admission, &out) == 1);
   assert(g_bind_mask == 0xfffu && out.seal_epoch == 7 && out.version == 3 &&
          out.ciphertext_len == 32 && out.hwm_attestation_len == 3);
   g_admit_replay = 1;
   memset(&out, 0, sizeof(out));
   assert(db2_management_status_key_admit(&c, &admission, &out) == 0);
   assert(g_bind_mask == 0xfffu && out.seal_epoch == 7 && !out.ciphertext_len &&
          !out.hwm_attestation_len);
   g_malformed_replay = 1;
   assert(db2_management_status_key_admit(&c, &admission, &out) == DB2_VAULT_KEY_USE_INTEGRITY);
   g_admit_replay = g_malformed_replay = 0;

   int64_t epoch = 0;
   int sealed = -1;
   assert(db2_management_status_key_startup_begin(&c, &epoch, &sealed) == 0);
   assert(epoch == 7 && sealed == 0);
   db2_management_status_key_ctx_close(&c);
   assert(g_closed == 1 && g_rollbacks == 1 && !g_in_tx && !c.connection);
   puts("management_status_key_ctx: all tests passed");
   return 0;
}
