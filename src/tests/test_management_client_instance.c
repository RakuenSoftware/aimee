#include "modules/db2/c/management_client_instance.h"
#include "modules/db2/c/db_postgres.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum
{
   MOCK_PREFLIGHT,
   MOCK_PENDING_INITIAL,
   MOCK_PENDING_RENEW,
   MOCK_ACTIVE,
   MOCK_SNAPSHOT,
   MOCK_MAINTENANCE
};
struct aimee_pg_stmt
{
   int mode, step;
};
static struct aimee_pg_stmt mock_stmt;
static int mock_guard, mock_bind_count, mock_bad_shape;
static const char *mock_sqlstate;

int db2_tenant_require_pg(void)
{
   return mock_guard;
}
void *(db2_conn)(void)
{
   return &mock_stmt;
}

/* Real code reaches the pool through the db2_conn() macro, which expands to
 * db2_conn_at(site) so a lazy acquire can be attributed. Route the stub. */
void *db2_conn_at(const char *site)
{
   (void)site;
   return (db2_conn)();
}
aimee_pg_stmt_t *aimee_pg_prepare_ex(void *c, const char *sql, aimee_pg_prepare_error_t *kind,
                                     char *err, size_t len)
{
   (void)err;
   (void)len;
   assert(c == &mock_stmt);
   mock_stmt.mode = strstr(sql, "grant_preflight") ? MOCK_PREFLIGHT
                    : strstr(sql, "begin_initial") ? MOCK_PENDING_INITIAL
                    : strstr(sql, "begin_renewal") ? MOCK_PENDING_RENEW
                    : strstr(sql, "activate")      ? MOCK_ACTIVE
                    : strstr(sql, "snapshot")      ? MOCK_SNAPSHOT
                                                   : MOCK_MAINTENANCE;
   mock_stmt.step = 0;
   mock_bind_count = 0;
   if (kind)
      *kind = AIMEE_PG_PREPARE_OK;
   return &mock_stmt;
}
void aimee_pg_finalize(aimee_pg_stmt_t *st)
{
   assert(st == &mock_stmt);
}
aimee_pg_step_t aimee_pg_step(aimee_pg_stmt_t *st, char *err, size_t len)
{
   (void)err;
   (void)len;
   if (mock_sqlstate && st->step++ == 0)
      return AIMEE_PG_ERR;
   return st->step++ == 0 ? AIMEE_PG_ROW : AIMEE_PG_DONE;
}
const char *aimee_pg_sqlstate(const aimee_pg_stmt_t *st)
{
   (void)st;
   return mock_sqlstate;
}
int aimee_pg_bind_text(aimee_pg_stmt_t *st, const char *name, const char *value)
{
   assert(st == &mock_stmt && name && value);
   mock_bind_count++;
   return 0;
}
int aimee_pg_bind_int(aimee_pg_stmt_t *st, const char *name, int value)
{
   assert(st == &mock_stmt && name && value > 0);
   mock_bind_count++;
   return 0;
}
int aimee_pg_bind_int64(aimee_pg_stmt_t *st, const char *name, int64_t value)
{
   assert(st == &mock_stmt && name && value > 0);
   mock_bind_count++;
   return 0;
}
int aimee_pg_bind_null(aimee_pg_stmt_t *st, const char *name)
{
   assert(st == &mock_stmt && name);
   mock_bind_count++;
   return 0;
}
int aimee_pg_column_count(aimee_pg_stmt_t *st)
{
   if (mock_bad_shape)
      return 1;
   return st->mode == MOCK_PREFLIGHT ? 3
          : st->mode == MOCK_MAINTENANCE
              ? 3
              : (st->mode == MOCK_SNAPSHOT ? 22 : (st->mode == MOCK_ACTIVE ? 24 : 18));
}
int aimee_pg_column_is_null(aimee_pg_stmt_t *st, int col)
{
   return st->mode == MOCK_PENDING_INITIAL && col >= 11 && col <= 14;
}
const char *aimee_pg_column_text(aimee_pg_stmt_t *st, int col)
{
   static const char hex32[] = "0123456789abcdef0123456789abcdef";
   static const char hex64[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
   if (st->mode == MOCK_PREFLIGHT)
      return col < 2 ? hex32 : "2000000000";
   if (st->mode == MOCK_MAINTENANCE)
      return col == 0 ? "1" : (col == 1 ? "2" : "0");
   if (st->mode == MOCK_PENDING_INITIAL || st->mode == MOCK_PENDING_RENEW)
   {
      switch (col)
      {
      case 0:
         return "f";
      case 1:
      case 2:
      case 3:
         return hex32;
      case 4:
         return "7";
      case 5:
      case 8:
      case 14:
      case 15:
      case 16:
         return hex64;
      case 6:
         return "active";
      case 7:
         return st->mode == MOCK_PENDING_INITIAL ? "1" : "2";
      case 9:
         return st->mode == MOCK_PENDING_INITIAL ? "initial" : "renew";
      case 10:
         return "pending";
      case 11:
         return "9";
      case 12:
         return "issuer";
      case 13:
         return "01";
      case 17:
         return "2000000000";
      }
   }
   int snapshot = st->mode == MOCK_SNAPSHOT;
   switch (col)
   {
   case 0:
      return "f";
   case 1:
   case 2:
   case 3:
      return hex32;
   case 4:
      return "7";
   case 5:
      return hex64;
   case 6:
      return "active";
   case 7:
   case 8:
      return "1";
   case 9:
      return hex64;
   case 10:
      return "initial";
   case 11:
      return "active";
   default:
      break;
   }
   int base = snapshot ? 12 : 14;
   if (!snapshot && (col == 12 || col == 13))
      return hex64;
   if (col == base || col == base + 4 || col == base + 5)
      return hex64;
   if (col == base + 1)
      return "cert:issuer:01";
   if (col == base + 2)
      return "issuer";
   if (col == base + 3)
      return "01";
   if (col == base + 6)
      return "1000";
   if (col == base + 7)
      return "4600";
   if (col == base + 8)
      return "1";
   if (col == base + 9)
      return "1100";
   return "";
}

static int all_zero(const void *value, size_t length)
{
   const unsigned char *p = value;
   unsigned char any = 0;
   for (size_t i = 0; i < length; ++i)
      any |= p[i];
   return any == 0;
}

static void test_sqlstate(void)
{
   assert(db2_management_client_instance_classify_sqlstate("22023") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID);
   assert(db2_management_client_instance_classify_sqlstate("28000") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_DENIED);
   assert(db2_management_client_instance_classify_sqlstate("42501") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_DENIED);
   assert(db2_management_client_instance_classify_sqlstate("23505") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_CONFLICT);
   assert(db2_management_client_instance_classify_sqlstate("40001") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_RETRY);
   assert(db2_management_client_instance_classify_sqlstate("40P01") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_RETRY);
   assert(db2_management_client_instance_classify_sqlstate("55000") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_INTEGRITY);
   assert(db2_management_client_instance_classify_sqlstate("08006") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE);
   assert(db2_management_client_instance_classify_sqlstate("25006") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE);
   assert(db2_management_client_instance_classify_sqlstate("XX000") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE);
   assert(db2_management_client_instance_classify_sqlstate("") ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE);
   assert(db2_management_client_instance_classify_sqlstate(NULL) ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE);
}

static void test_digest_vector(void)
{
   static const unsigned char expected[32] = {0x95, 0x63, 0x7a, 0xcc, 0x7f, 0x1b, 0x99, 0xb7,
                                              0xa5, 0x93, 0x27, 0x43, 0xdc, 0x7d, 0x56, 0x45,
                                              0x52, 0x35, 0x48, 0x62, 0x2a, 0xfb, 0x1e, 0x67,
                                              0x9a, 0x62, 0x13, 0x78, 0xc1, 0xe1, 0x6e, 0xe4};
   unsigned char proof[32], custody[32], digest[32];
   for (size_t i = 0; i < sizeof(proof); ++i)
   {
      proof[i] = (unsigned char)i;
      custody[i] = (unsigned char)(i + 32);
   }
   assert(db2_management_client_instance_binding_digest(
              "spiffe://example.test", "spiffe://example.test/kb/node-1", proof, custody, digest) ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_OK);
   assert(memcmp(digest, expected, sizeof(expected)) == 0);

   proof[0] ^= 1;
   assert(db2_management_client_instance_binding_digest(
              "spiffe://example.test", "spiffe://example.test/kb/node-1", proof, custody, digest) ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_OK);
   assert(memcmp(digest, expected, sizeof(expected)) != 0);
}

static void test_bounds_and_clearing(void)
{
   unsigned char anchor[32] = {1}, digest[32];
   memset(digest, 0xa5, sizeof(digest));
   assert(db2_management_client_instance_binding_digest("", "subject", anchor, anchor, digest) ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID);
   assert(all_zero(digest, sizeof(digest)));

   char too_long[DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 2];
   memset(too_long, 'a', sizeof(too_long));
   too_long[sizeof(too_long) - 1] = 0;
   memset(digest, 0xa5, sizeof(digest));
   assert(
       db2_management_client_instance_binding_digest(too_long, "subject", anchor, anchor, digest) ==
       DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID);
   assert(all_zero(digest, sizeof(digest)));

   memset(digest, 0xa5, sizeof(digest));
   assert(db2_management_client_instance_binding_digest("issuer", "bad subject", anchor, anchor,
                                                        digest) ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID);
   assert(all_zero(digest, sizeof(digest)));
   assert(db2_management_client_instance_binding_digest(
              "issuer", "subject", anchor, anchor, NULL) == DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID);

   db2_management_client_instance_binding_t binding;
   memset(&binding, 0xa5, sizeof(binding));
   assert(
       db2_management_client_instance_binding_init("issuer", too_long, anchor, anchor, &binding) ==
       DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID);
   assert(all_zero(&binding, sizeof(binding)));
   assert(db2_management_client_instance_binding_init(
              "issuer", "subject", anchor, anchor, &binding) == DB2_MANAGEMENT_CLIENT_INSTANCE_OK);
   assert(strcmp(binding.issuer, "issuer") == 0);
   assert(strcmp(binding.subject, "subject") == 0);
   assert(memcmp(binding.proof_anchor, anchor, sizeof(anchor)) == 0);
   assert(!all_zero(binding.binding_digest, sizeof(binding.binding_digest)));
}

static void fill_id(char *out, size_t n)
{
   static const char hex[] = "0123456789abcdef";
   for (size_t i = 0; i < n; ++i)
      out[i] = hex[i & 15];
   out[n] = 0;
}

static void test_runtime_facades(void)
{
   unsigned char anchor[32] = {1};
   db2_management_client_instance_binding_t binding;
   assert(db2_management_client_instance_binding_init(
              "issuer", "subject", anchor, anchor, &binding) == DB2_MANAGEMENT_CLIENT_INSTANCE_OK);
   db2_management_client_initial_request_t initial = {0};
   fill_id(initial.operation_id, 64);
   fill_id(initial.authority_id, 32);
   fill_id(initial.installation_id, 32);
   fill_id(initial.expected_lineage_id, 32);
   initial.binding = binding;
   db2_management_client_grant_preflight_request_t preflight = {0};
   memcpy(preflight.installation_id, initial.installation_id, sizeof(preflight.installation_id));
   preflight.binding = binding;
   db2_management_client_grant_preflight_t grant;
   assert(db2_management_client_instance_grant_preflight(&preflight, &grant) ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_OK);
   assert(!strcmp(grant.installation_id, preflight.installation_id) &&
          !strcmp(grant.replacement_lineage_id, preflight.installation_id) &&
          grant.expires_at_epoch == 2000000000 && mock_bind_count == 6);
   db2_management_client_pending_t pending;
   assert(db2_management_client_instance_begin_initial(&initial, &pending) ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_OK);
   assert(pending.generation == 1 && pending.issue_kind == DB2_MANAGEMENT_CLIENT_ISSUE_INITIAL &&
          !pending.has_previous && mock_bind_count == 11);

   db2_management_client_renewal_request_t renewal = {0};
   fill_id(renewal.operation_id, 64);
   fill_id(renewal.installation_id, 32);
   renewal.binding = binding;
   renewal.generation = 2;
   renewal.previous_enrollment_id = 9;
   strcpy(renewal.previous_cert_issuer, "issuer");
   strcpy(renewal.previous_cert_serial_norm, "01");
   assert(db2_management_client_instance_begin_renewal(&renewal, &pending) ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_OK);
   assert(pending.has_previous && pending.generation == 2 && mock_bind_count == 14);

   db2_management_client_activation_request_t activation = {0};
   fill_id(activation.operation_id, 64);
   fill_id(activation.installation_id, 32);
   activation.binding = binding;
   activation.issue_kind = DB2_MANAGEMENT_CLIENT_ISSUE_INITIAL;
   activation.generation = 1;
   strcpy(activation.verified_ca_issuer, "issuer");
   strcpy(activation.leaf_issuer, "issuer");
   strcpy(activation.leaf_serial_norm, "01");
   activation.leaf_not_before_epoch = 1000;
   activation.leaf_not_after_epoch = 4600;
   db2_management_client_active_t active;
   assert(db2_management_client_instance_activate(&activation, &active) ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_OK);
   assert(active.enrollment_id == 1 && active.issue_state == DB2_MANAGEMENT_CLIENT_ISSUE_ACTIVE &&
          mock_bind_count == 24);

   assert(db2_management_client_instance_snapshot(initial.installation_id, &binding, &active) ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_OK);
   assert(active.generation == 1 && mock_bind_count == 6);

   db2_management_client_maintenance_t maintenance;
   assert(db2_management_client_instance_expire_quarantine(10, &maintenance) ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_OK);
   assert(maintenance.expired_grants == 1 && maintenance.expired_issues == 2 &&
          maintenance.quarantined_issues == 0 && mock_bind_count == 1);

   mock_sqlstate = "40001";
   memset(&grant, 0xa5, sizeof(grant));
   assert(db2_management_client_instance_grant_preflight(&preflight, &grant) ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_RETRY);
   assert(all_zero(&grant, sizeof(grant)));
   memset(&pending, 0xa5, sizeof(pending));
   assert(db2_management_client_instance_begin_initial(&initial, &pending) ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_RETRY);
   assert(all_zero(&pending, sizeof(pending)));
   mock_sqlstate = NULL;
   mock_bad_shape = 1;
   memset(&grant, 0xa5, sizeof(grant));
   assert(db2_management_client_instance_grant_preflight(&preflight, &grant) ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_INTEGRITY);
   assert(all_zero(&grant, sizeof(grant)));
   memset(&active, 0xa5, sizeof(active));
   assert(db2_management_client_instance_snapshot(initial.installation_id, &binding, &active) ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_INTEGRITY);
   assert(all_zero(&active, sizeof(active)));
   mock_bad_shape = 0;
   mock_guard = -1;
   memset(&active, 0xa5, sizeof(active));
   assert(db2_management_client_instance_snapshot(initial.installation_id, &binding, &active) ==
          DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE);
   assert(all_zero(&active, sizeof(active)));
   mock_guard = 0;
}

int main(void)
{
   test_sqlstate();
   test_digest_vector();
   test_bounds_and_clearing();
   test_runtime_facades();
   puts("management_client_instance: all tests passed");
   return 0;
}
