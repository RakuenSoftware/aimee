#include "modules/db2/c/management_action_journal.h"
#include "modules/db2/c/db2_tenant.h"
#include "modules/db2/c/db_postgres.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum
{
   MOCK_INTENT,
   MOCK_OUTCOME
};

struct aimee_pg_stmt
{
   int mode, step;
};

static struct aimee_pg_stmt mock_stmt;
static int mock_bad_shape, mock_bad_field, mock_duplicate_row, mock_commit_failure,
    mock_rollback_count, mock_begin_result, mock_random_failure;
static const char *mock_sqlstate;
static char bound_text[11][700];
static int64_t bound_i64[11];
static int bound_null[11];

int platform_random_bytes(void *buf, size_t len)
{
   if (mock_random_failure)
      return -1;
   static unsigned char sequence = 1;
   unsigned char *out = buf;
   for (size_t i = 0; i < len; ++i)
      out[i] = sequence++;
   return 0;
}

static int parameter_index(const char *name)
{
   assert(name && name[0] == '?');
   int n = 0;
   for (const char *p = name + 1; *p; ++p)
   {
      assert(*p >= '0' && *p <= '9');
      n = n * 10 + (*p - '0');
   }
   assert(n >= 1 && n <= 10);
   return n;
}

int db2_tenant_scope_begin(const kb_principal_t *principal, int64_t team)
{
   assert(principal && team == 7);
   return mock_begin_result;
}

int db2_tenant_scope_commit(void)
{
   return mock_commit_failure ? -1 : 0;
}

void db2_tenant_scope_rollback(void)
{
   mock_rollback_count++;
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

aimee_pg_stmt_t *aimee_pg_prepare_ex(void *conn, const char *sql, aimee_pg_prepare_error_t *kind,
                                     char *err, size_t errlen)
{
   (void)err;
   (void)errlen;
   assert(conn == &mock_stmt);
   mock_stmt.mode = strstr(sql, "outcome_append") ? MOCK_OUTCOME : MOCK_INTENT;
   mock_stmt.step = 0;
   memset(bound_text, 0, sizeof(bound_text));
   memset(bound_i64, 0, sizeof(bound_i64));
   memset(bound_null, 0, sizeof(bound_null));
   if (kind)
      *kind = AIMEE_PG_PREPARE_OK;
   return &mock_stmt;
}

void aimee_pg_finalize(aimee_pg_stmt_t *st)
{
   assert(st == &mock_stmt);
}

aimee_pg_step_t aimee_pg_step(aimee_pg_stmt_t *st, char *err, size_t errlen)
{
   (void)err;
   (void)errlen;
   if (mock_sqlstate && st->step++ == 0)
      return AIMEE_PG_ERR;
   if (st->step++ == 0)
      return AIMEE_PG_ROW;
   return mock_duplicate_row ? AIMEE_PG_ROW : AIMEE_PG_DONE;
}

const char *aimee_pg_sqlstate(const aimee_pg_stmt_t *st)
{
   (void)st;
   return mock_sqlstate;
}

int aimee_pg_bind_text(aimee_pg_stmt_t *st, const char *name, const char *value)
{
   assert(st == &mock_stmt && value);
   int n = parameter_index(name);
   assert(strlen(value) < sizeof(bound_text[n]));
   strcpy(bound_text[n], value);
   return 0;
}

int aimee_pg_bind_int(aimee_pg_stmt_t *st, const char *name, int value)
{
   assert(st == &mock_stmt);
   bound_i64[parameter_index(name)] = value;
   return 0;
}

int aimee_pg_bind_int64(aimee_pg_stmt_t *st, const char *name, int64_t value)
{
   assert(st == &mock_stmt);
   bound_i64[parameter_index(name)] = value;
   return 0;
}

int aimee_pg_bind_null(aimee_pg_stmt_t *st, const char *name)
{
   assert(st == &mock_stmt);
   bound_null[parameter_index(name)] = 1;
   return 0;
}

int aimee_pg_column_count(aimee_pg_stmt_t *st)
{
   return mock_bad_shape ? 1 : (st->mode == MOCK_INTENT ? 25 : 8);
}

int aimee_pg_column_is_null(aimee_pg_stmt_t *st, int col)
{
   return st->mode == MOCK_OUTCOME && ((col == 5 && bound_null[4]) || (col == 6 && bound_null[5]));
}

const char *aimee_pg_column_text(aimee_pg_stmt_t *st, int col)
{
   static const char hex64[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
   if (st->mode == MOCK_OUTCOME)
   {
      switch (col)
      {
      case 0:
         return "f";
      case 1:
         return bound_text[1];
      case 2:
         return "7";
      case 3:
         return bound_text[2];
      case 4:
         return bound_text[3];
      case 5:
         return bound_null[4] ? NULL : "204";
      case 6:
         return bound_null[5] ? NULL : bound_text[5];
      case 7:
         return "1100";
      }
   }
   switch (col)
   {
   case 0:
      return "f";
   case 1:
      return bound_text[1];
   case 2:
      return bound_text[2];
   case 3:
      return "7";
   case 4:
      return mock_bad_field == 2 ? "oidc:bad%2Fissuer:operator" : "oidc:https%3A//issuer:operator";
   case 5:
      return "remote_writes";
   case 6:
      return bound_text[4];
   case 7:
      return bound_text[6];
   case 8:
      return bound_text[7];
   case 9:
      return bound_text[4];
   case 10:
      return mock_bad_field == 1 ? "bad/kid" : bound_text[8];
   case 11:
      return "1000";
   case 12:
      return "1060";
   case 13:
      return bound_text[10];
   case 14:
      return "1";
   case 15:
      return "9";
   case 16:
      return "local-issuer";
   case 17:
      return "01";
   case 18:
      return hex64;
   case 19:
      return "10";
   case 20:
      return "target-issuer";
   case 21:
      return "02";
   case 22:
      return hex64;
   case 23:
      return "4";
   case 24:
      return "999";
   }
   return NULL;
}

static db2_management_action_operation_t make_operation(void)
{
   uint8_t digest[32];
   memset(digest, 0xa5, sizeof(digest));
   db2_management_action_operation_t op;
   assert(db2_management_action_operation_init(
              7, "server-a", DB2_MANAGEMENT_ACTION_CAP_REMOTE_WRITES, digest,
              "https://kb.example.test", "management-key-1", 60, "0123456789abcdef0123456789abcdef",
              &op) == DB2_MANAGEMENT_ACTION_OK);
   assert(strlen(op.correlation_id) == 64 && strlen(op.jti) == 64);
   assert(strcmp(op.correlation_id, op.jti) != 0);
   for (size_t i = 0; i < 64; ++i)
      assert(op.request_sha256[i] == (i & 1 ? '5' : 'a'));
   return op;
}

static void test_init_and_validation(void)
{
   db2_management_action_operation_t op = make_operation(), clear;
   memset(&clear, 0x5a, sizeof(clear));
   assert(db2_management_action_operation_init_hex(
              7, "server-a", DB2_MANAGEMENT_ACTION_CAP_REMOTE_WRITES, "BAD", "issuer", "kid", 60,
              "0123456789abcdef0123456789abcdef", &clear) == DB2_MANAGEMENT_ACTION_INVALID);
   assert(!memcmp(&clear, &(db2_management_action_operation_t){0}, sizeof(clear)));

#define INVALID_INIT(team, target, cap, digest, issuer, kid, ttl, installation)                    \
   do                                                                                              \
   {                                                                                               \
      memset(&clear, 0x5a, sizeof(clear));                                                         \
      assert(db2_management_action_operation_init_hex(team, target, cap, digest, issuer, kid, ttl, \
                                                      installation,                                \
                                                      &clear) == DB2_MANAGEMENT_ACTION_INVALID);   \
      assert(!memcmp(&clear, &(db2_management_action_operation_t){0}, sizeof(clear)));             \
   } while (0)
   static const char hex64[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
   INVALID_INIT(0, "server-a", DB2_MANAGEMENT_ACTION_CAP_REMOTE_WRITES, hex64, "issuer", "kid", 60,
                "0123456789abcdef0123456789abcdef");
   INVALID_INIT(7, "bad/server", DB2_MANAGEMENT_ACTION_CAP_REMOTE_WRITES, hex64, "issuer", "kid",
                60, "0123456789abcdef0123456789abcdef");
   INVALID_INIT(7, "server-a", 0, hex64, "issuer", "kid", 60, "0123456789abcdef0123456789abcdef");
   INVALID_INIT(7, "server-a", DB2_MANAGEMENT_ACTION_CAP_REMOTE_WRITES, hex64, "bad\nissuer", "kid",
                60, "0123456789abcdef0123456789abcdef");
   INVALID_INIT(7, "server-a", DB2_MANAGEMENT_ACTION_CAP_REMOTE_WRITES, hex64, "issuer", "bad/kid",
                60, "0123456789abcdef0123456789abcdef");
   INVALID_INIT(7, "server-a", DB2_MANAGEMENT_ACTION_CAP_REMOTE_WRITES, hex64, "issuer", "kid", 91,
                "0123456789abcdef0123456789abcdef");
   INVALID_INIT(7, "server-a", DB2_MANAGEMENT_ACTION_CAP_REMOTE_WRITES, hex64, "issuer", "kid", 60,
                "0123456789abcdef0123456789abcdeF");
#undef INVALID_INIT

   mock_random_failure = 1;
   memset(&clear, 0x5a, sizeof(clear));
   assert(db2_management_action_operation_init_hex(
              7, "server-a", DB2_MANAGEMENT_ACTION_CAP_REMOTE_WRITES, hex64, "issuer", "kid", 60,
              "0123456789abcdef0123456789abcdef", &clear) == DB2_MANAGEMENT_ACTION_UNAVAILABLE);
   assert(!memcmp(&clear, &(db2_management_action_operation_t){0}, sizeof(clear)));
   mock_random_failure = 0;

   /* A visible string prefix is insufficient: unused bytes are part of the
    * fixed record and must stay zero to reject embedded-NUL suffixes. */
   op.target_server_id[strlen(op.target_server_id) + 1] = 'x';
   db2_management_action_intent_t out;
   memset(&out, 0x5a, sizeof(out));
   kb_principal_t principal = {0};
   assert(db2_management_action_intent_start(&principal, &op, &out) ==
          DB2_MANAGEMENT_ACTION_INVALID);
   assert(!memcmp(&out, &(db2_management_action_intent_t){0}, sizeof(out)));
}

static void test_intent_and_ambiguity(void)
{
   db2_management_action_operation_t op = make_operation();
   db2_management_action_operation_t retained = op;
   db2_management_action_intent_t out;
   kb_principal_t principal = {0};
   mock_commit_failure = 1;
   memset(&out, 0x5a, sizeof(out));
   assert(db2_management_action_intent_start(&principal, &op, &out) ==
          DB2_MANAGEMENT_ACTION_COMMIT_AMBIGUOUS);
   assert(!memcmp(&out, &(db2_management_action_intent_t){0}, sizeof(out)));
   assert(!memcmp(&op, &retained, sizeof(op)));

   mock_commit_failure = 0;
   assert(db2_management_action_intent_start(&principal, &op, &out) == DB2_MANAGEMENT_ACTION_OK);
   assert(out.dispatch_eligibility == DB2_MANAGEMENT_ACTION_JOURNALED_ONLY);
   assert(!strcmp(out.correlation_id, op.correlation_id));
   assert(!strcmp(out.jti, op.jti));
   assert(!strcmp(out.audience, op.target_server_id));

   mock_bad_shape = 1;
   memset(&out, 0x5a, sizeof(out));
   assert(db2_management_action_intent_start(&principal, &op, &out) ==
          DB2_MANAGEMENT_ACTION_INTEGRITY);
   assert(!memcmp(&out, &(db2_management_action_intent_t){0}, sizeof(out)));
   mock_bad_shape = 0;

   mock_bad_field = 1;
   assert(db2_management_action_intent_start(&principal, &op, &out) ==
          DB2_MANAGEMENT_ACTION_INTEGRITY);
   assert(!memcmp(&out, &(db2_management_action_intent_t){0}, sizeof(out)));
   mock_bad_field = 0;

   mock_bad_field = 2;
   assert(db2_management_action_intent_start(&principal, &op, &out) ==
          DB2_MANAGEMENT_ACTION_INTEGRITY);
   assert(!memcmp(&out, &(db2_management_action_intent_t){0}, sizeof(out)));
   mock_bad_field = 0;

   mock_duplicate_row = 1;
   assert(db2_management_action_intent_start(&principal, &op, &out) ==
          DB2_MANAGEMENT_ACTION_INTEGRITY);
   assert(!memcmp(&out, &(db2_management_action_intent_t){0}, sizeof(out)));
   mock_duplicate_row = 0;

   mock_begin_result = DB2_ERR_TENANT_UNAUTHENTICATED;
   assert(db2_management_action_intent_start(&principal, &op, &out) ==
          DB2_MANAGEMENT_ACTION_DENIED);
   assert(!memcmp(&out, &(db2_management_action_intent_t){0}, sizeof(out)));
   mock_begin_result = 0;
}

static void test_outcome(void)
{
   db2_management_action_operation_t start = make_operation();
   db2_management_action_outcome_operation_t op;
   memset(&op, 0, sizeof(op));
   memcpy(op.correlation_id, start.correlation_id, sizeof(op.correlation_id));
   op.team_id = 7;
   op.result = DB2_MANAGEMENT_ACTION_SUCCEEDED;
   op.result_class = DB2_MANAGEMENT_ACTION_CLASS_REMOTE_SUCCESS;
   op.has_status_code = 1;
   op.status_code = 204;
   op.has_response_sha256 = 1;
   memcpy(op.response_sha256, start.request_sha256, sizeof(op.response_sha256));
   db2_management_action_outcome_operation_t retained = op;
   db2_management_action_outcome_t out;
   kb_principal_t principal = {0};

   mock_commit_failure = 1;
   memset(&out, 0x5a, sizeof(out));
   assert(db2_management_action_outcome_append(&principal, &op, &out) ==
          DB2_MANAGEMENT_ACTION_COMMIT_AMBIGUOUS);
   assert(!memcmp(&out, &(db2_management_action_outcome_t){0}, sizeof(out)));
   assert(!memcmp(&op, &retained, sizeof(op)));
   mock_commit_failure = 0;

   assert(db2_management_action_outcome_append(&principal, &op, &out) == DB2_MANAGEMENT_ACTION_OK);
   assert(out.result == op.result && out.result_class == op.result_class);
   assert(out.status_code == 204 && out.has_response_sha256);

   op.result_class = DB2_MANAGEMENT_ACTION_CLASS_TRANSPORT_AMBIGUOUS;
   memset(&out, 0x5a, sizeof(out));
   assert(db2_management_action_outcome_append(&principal, &op, &out) ==
          DB2_MANAGEMENT_ACTION_INVALID);
   assert(!memcmp(&out, &(db2_management_action_outcome_t){0}, sizeof(out)));

   op.result = DB2_MANAGEMENT_ACTION_INDETERMINATE;
   op.has_status_code = 0;
   op.status_code = 0;
   op.has_response_sha256 = 0;
   memset(op.response_sha256, 0, sizeof(op.response_sha256));
   assert(db2_management_action_outcome_append(&principal, &op, &out) == DB2_MANAGEMENT_ACTION_OK);
   assert(!out.has_status_code && !out.has_response_sha256);

   op.result_class = DB2_MANAGEMENT_ACTION_CLASS_PROTOCOL_FAILURE;
   assert(db2_management_action_outcome_append(&principal, &op, &out) == DB2_MANAGEMENT_ACTION_OK);
   op.result = DB2_MANAGEMENT_ACTION_FAILED;
   assert(db2_management_action_outcome_append(&principal, &op, &out) ==
          DB2_MANAGEMENT_ACTION_INVALID);
}

static void test_sqlstate(void)
{
   assert(db2_management_action_classify_sqlstate("22023") == DB2_MANAGEMENT_ACTION_INVALID);
   assert(db2_management_action_classify_sqlstate("42501") == DB2_MANAGEMENT_ACTION_DENIED);
   assert(db2_management_action_classify_sqlstate("23505") == DB2_MANAGEMENT_ACTION_CONFLICT);
   assert(db2_management_action_classify_sqlstate("40001") == DB2_MANAGEMENT_ACTION_RETRY);
   assert(db2_management_action_classify_sqlstate("25006") == DB2_MANAGEMENT_ACTION_RETRY);
   assert(db2_management_action_classify_sqlstate("55000") == DB2_MANAGEMENT_ACTION_INTEGRITY);
   assert(db2_management_action_classify_sqlstate("XX000") == DB2_MANAGEMENT_ACTION_UNAVAILABLE);

   db2_management_action_operation_t op = make_operation();
   db2_management_action_intent_t out;
   kb_principal_t principal = {0};
   mock_sqlstate = "23505";
   int before = mock_rollback_count;
   assert(db2_management_action_intent_start(&principal, &op, &out) ==
          DB2_MANAGEMENT_ACTION_CONFLICT);
   assert(mock_rollback_count == before + 1);
   assert(!memcmp(&out, &(db2_management_action_intent_t){0}, sizeof(out)));
   mock_sqlstate = NULL;
}

int main(void)
{
   test_init_and_validation();
   test_intent_and_ambiguity();
   test_outcome();
   test_sqlstate();
   puts("test_management_action_journal: ok");
   return 0;
}
