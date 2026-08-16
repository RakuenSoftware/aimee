/* test_management_identity_journal.c — the login seam's C half.
 *
 * The Postgres side is gated by scripts/per-user-identity-authority-pg17-test.sql
 * (which invokes the real function against a real schema). What is worth proving
 * here, with no database, is everything the facade decides on its own:
 *
 *   - the three identifiers are independently generated, never derived
 *   - a CSPRNG failure yields no operation at all, never a weak identifier
 *   - the exact parameter binding, including which slot the auth mode lands in
 *   - a returned row that disagrees with what was asked for is INTEGRITY, not a
 *     token minted under someone else's claims
 *   - the SQLSTATE taxonomy maps onto the shared result enum
 *   - a lost COMMIT is COMMIT_AMBIGUOUS with cleared outputs, so a retry reuses
 *     the caller's identifiers rather than filing a second intent
 */
#include "modules/db2/c/management_identity_journal.h"
#include "modules/db2/c/db2_tenant.h"
#include "modules/db2/c/db_postgres.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct aimee_pg_stmt
{
   int step;
};

static struct aimee_pg_stmt mock_stmt;
static int mock_bad_shape, mock_bad_field, mock_duplicate_row, mock_commit_failure,
    mock_rollback_count, mock_begin_result, mock_random_failure, mock_prepare_failure;
static const char *mock_sqlstate;
static char bound_text[11][700];
static int64_t bound_i64[11];

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
   /* The facade must call the identity writer, not the action one. */
   assert(strstr(sql, "kb_management_identity_intent_start"));
   mock_stmt.step = 0;
   memset(bound_text, 0, sizeof(bound_text));
   memset(bound_i64, 0, sizeof(bound_i64));
   if (kind)
      *kind = AIMEE_PG_PREPARE_OK;
   return mock_prepare_failure ? NULL : &mock_stmt;
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
   (void)parameter_index(name);
   return 0;
}

int aimee_pg_column_count(aimee_pg_stmt_t *st)
{
   (void)st;
   return mock_bad_shape ? 1 : 19;
}

int aimee_pg_column_is_null(aimee_pg_stmt_t *st, int col)
{
   (void)st;
   (void)col;
   return 0;
}

/* Echo back exactly what was bound, so the facade's field-by-field comparison is
 * satisfied on the happy path; mock_bad_field perturbs one field at a time. */
const char *aimee_pg_column_text(aimee_pg_stmt_t *st, int col)
{
   (void)st;
   switch (col)
   {
   case 0:
      return "f"; /* replayed */
   case 1:
      return bound_text[1]; /* correlation_id */
   case 2:
      return bound_text[2]; /* jti */
   case 3:
      return mock_bad_field == 3 ? "short" : bound_text[3]; /* token_jti */
   case 4:
      return "7"; /* team_id */
   case 5:
      /* Subject comes from aimee.principal, never from a bind. A value that is
       * not a canonical identity key must be refused.
       *
       * The invalid fixture has a SPACE in it deliberately. Now that a bare
       * host-account name is a valid subject, an unprefixed string is no longer
       * invalid by default — "not-an-identity-key" is a perfectly legal POSIX
       * username. So the only values left to reject are ones that match neither
       * the prefixed forms nor a username, and that is exactly what the C grammar
       * and the schema CHECK now have to carry between them. */
      return mock_bad_field == 1   ? "not an identity key"
             : mock_bad_field == 6 ? "alice"    /* bare: a PAM host account */
             : mock_bad_field == 7 ? "-badname" /* bare, but not a username  */
                                   : "oidc:https%3A//issuer:alice";
   case 6:
      return mock_bad_field == 2 ? "ldap" : bound_text[6]; /* auth_mode */
   case 7:
      return bound_text[5]; /* target_server_id */
   case 8:
      return bound_text[7]; /* token_issuer */
   case 9:
      /* audience; mock_bad_field 4 makes it diverge from the target server */
      return mock_bad_field == 4 ? "other-server" : bound_text[5];
   case 10:
      return bound_text[8]; /* kid */
   case 11:
      return "1000"; /* issued_at */
   case 12:
      return mock_bad_field == 5 ? "9000" : "1300"; /* expires_at (ttl 300) */
   case 13:
      return bound_text[10]; /* installation_id */
   case 14:
      return "1";
   case 15:
      return "9";
   case 16:
      return "10";
   case 17:
      return "4";
   case 18:
      return "999";
   }
   return NULL;
}

static void reset_mocks(void)
{
   mock_bad_shape = mock_bad_field = mock_duplicate_row = mock_commit_failure = 0;
   mock_rollback_count = mock_begin_result = mock_random_failure = mock_prepare_failure = 0;
   mock_sqlstate = NULL;
}

static db2_identity_intent_operation_t make_operation(void)
{
   db2_identity_intent_operation_t op;
   assert(db2_identity_intent_operation_init(7, "server-a", DB2_IDENTITY_AUTH_MODE_OIDC,
                                             "https://kb.example.test", "identity-key-1", 300,
                                             "0123456789abcdef0123456789abcdef",
                                             &op) == DB2_MANAGEMENT_ACTION_OK);
   return op;
}

static kb_principal_t make_principal(void)
{
   kb_principal_t p;
   memset(&p, 0, sizeof(p));
   p.kind = KB_PRIN_OIDC;
   snprintf(p.issuer, sizeof(p.issuer), "%s", "https://issuer");
   snprintf(p.subject, sizeof(p.subject), "%s", "alice");
   p.authenticated = 1;
   return p;
}

static void test_auth_mode_strings(void)
{
   assert(!strcmp(db2_identity_auth_mode_str(DB2_IDENTITY_AUTH_MODE_OIDC), "oidc"));
   assert(!strcmp(db2_identity_auth_mode_str(DB2_IDENTITY_AUTH_MODE_PAM), "pam"));
   assert(db2_identity_auth_mode_str((db2_identity_auth_mode_t)0) == NULL);
   assert(db2_identity_auth_mode_str((db2_identity_auth_mode_t)99) == NULL);
}

static void test_operation_init(void)
{
   reset_mocks();
   db2_identity_intent_operation_t op = make_operation();
   assert(strlen(op.correlation_id) == 64 && strlen(op.jti) == 64);
   assert(strlen(op.token_jti) == 64);
   /* Three independent draws: no identifier may be derivable from another, or a
    * caller holding a token could predict the handle that minted it. */
   assert(strcmp(op.correlation_id, op.jti) && strcmp(op.correlation_id, op.token_jti) &&
          strcmp(op.jti, op.token_jti));
   assert(op.team_id == 7 && op.ttl_seconds == 300);
   assert(op.auth_mode == DB2_IDENTITY_AUTH_MODE_OIDC);
   assert(!strcmp(op.target_server_id, "server-a"));
   assert(!strcmp(op.installation_id, "0123456789abcdef0123456789abcdef"));

   /* Rejected inputs. Each one is a schema CHECK the C side refuses first. */
   db2_identity_intent_operation_t bad;
   assert(db2_identity_intent_operation_init(0, "server-a", DB2_IDENTITY_AUTH_MODE_OIDC, "kb", "k",
                                             300, "0123456789abcdef0123456789abcdef",
                                             &bad) == DB2_MANAGEMENT_ACTION_INVALID);
   assert(db2_identity_intent_operation_init(7, "server-a", (db2_identity_auth_mode_t)0, "kb", "k",
                                             300, "0123456789abcdef0123456789abcdef",
                                             &bad) == DB2_MANAGEMENT_ACTION_INVALID);
   /* A TTL the server's verifier would throw away. */
   assert(db2_identity_intent_operation_init(7, "server-a", DB2_IDENTITY_AUTH_MODE_OIDC, "kb", "k",
                                             DB2_IDENTITY_TTL_MAX_SECONDS + 1,
                                             "0123456789abcdef0123456789abcdef",
                                             &bad) == DB2_MANAGEMENT_ACTION_INVALID);
   assert(db2_identity_intent_operation_init(7, "server-a", DB2_IDENTITY_AUTH_MODE_OIDC, "kb", "k",
                                             0, "0123456789abcdef0123456789abcdef",
                                             &bad) == DB2_MANAGEMENT_ACTION_INVALID);
   /* A kid outside the token charset, and a non-hex installation id. */
   assert(db2_identity_intent_operation_init(7, "server-a", DB2_IDENTITY_AUTH_MODE_OIDC, "kb",
                                             "bad/kid", 300, "0123456789abcdef0123456789abcdef",
                                             &bad) == DB2_MANAGEMENT_ACTION_INVALID);
   assert(db2_identity_intent_operation_init(7, "server-a", DB2_IDENTITY_AUTH_MODE_OIDC, "kb", "k",
                                             300, "not-hex",
                                             &bad) == DB2_MANAGEMENT_ACTION_INVALID);
   assert(db2_identity_intent_operation_init(7, NULL, DB2_IDENTITY_AUTH_MODE_OIDC, "kb", "k", 300,
                                             "0123456789abcdef0123456789abcdef",
                                             &bad) == DB2_MANAGEMENT_ACTION_INVALID);
   assert(db2_identity_intent_operation_init(7, "server-a", DB2_IDENTITY_AUTH_MODE_OIDC, "kb", "k",
                                             300, "0123456789abcdef0123456789abcdef",
                                             NULL) == DB2_MANAGEMENT_ACTION_INVALID);

   /* A CSPRNG failure must produce no operation at all. */
   mock_random_failure = 1;
   memset(&bad, 0xff, sizeof(bad));
   assert(db2_identity_intent_operation_init(7, "server-a", DB2_IDENTITY_AUTH_MODE_OIDC, "kb", "k",
                                             300, "0123456789abcdef0123456789abcdef",
                                             &bad) == DB2_MANAGEMENT_ACTION_UNAVAILABLE);
   db2_identity_intent_operation_t zero;
   memset(&zero, 0, sizeof(zero));
   assert(!memcmp(&bad, &zero, sizeof(bad)));
   mock_random_failure = 0;
}

static void test_start_happy_path(void)
{
   reset_mocks();
   db2_identity_intent_operation_t op = make_operation();
   kb_principal_t principal = make_principal();
   db2_identity_intent_t out;
   assert(db2_identity_intent_start(&principal, &op, &out) == DB2_MANAGEMENT_ACTION_OK);

   /* Binding order is the SQL signature's, and the auth mode goes out as its
    * wire string in slot 6 — not as an integer, and not in the subject's place
    * (there is no subject slot). */
   assert(!strcmp(bound_text[1], op.correlation_id));
   assert(!strcmp(bound_text[2], op.jti));
   assert(!strcmp(bound_text[3], op.token_jti));
   assert(bound_i64[4] == 7);
   assert(!strcmp(bound_text[5], "server-a"));
   assert(!strcmp(bound_text[6], "oidc"));
   assert(!strcmp(bound_text[7], "https://kb.example.test"));
   assert(!strcmp(bound_text[8], "identity-key-1"));
   assert(bound_i64[9] == 300);
   assert(!strcmp(bound_text[10], "0123456789abcdef0123456789abcdef"));

   assert(out.replayed == 0);
   assert(!strcmp(out.correlation_id, op.correlation_id));
   assert(!strcmp(out.token_jti, op.token_jti));
   assert(out.auth_mode == DB2_IDENTITY_AUTH_MODE_OIDC);
   /* The subject is whatever the database resolved from the scope. */
   assert(!strcmp(out.subject, "oidc:https%3A//issuer:alice"));
   assert(!strcmp(out.audience, out.target_server_id));
   assert(out.expires_at - out.issued_at == 300);
   assert(mock_rollback_count == 0);

   /* PAM rides the identical path; only the recorded mode differs. */
   assert(db2_identity_intent_operation_init(7, "server-a", DB2_IDENTITY_AUTH_MODE_PAM,
                                             "https://kb.example.test", "identity-key-1", 300,
                                             "0123456789abcdef0123456789abcdef",
                                             &op) == DB2_MANAGEMENT_ACTION_OK);
   assert(db2_identity_intent_start(&principal, &op, &out) == DB2_MANAGEMENT_ACTION_OK);
   assert(!strcmp(bound_text[6], "pam"));
   assert(out.auth_mode == DB2_IDENTITY_AUTH_MODE_PAM);
}

static void test_start_rejects_bad_arguments(void)
{
   reset_mocks();
   db2_identity_intent_operation_t op = make_operation();
   kb_principal_t principal = make_principal();
   db2_identity_intent_t out;
   assert(db2_identity_intent_start(NULL, &op, &out) == DB2_MANAGEMENT_ACTION_INVALID);
   assert(db2_identity_intent_start(&principal, NULL, &out) == DB2_MANAGEMENT_ACTION_INVALID);
   assert(db2_identity_intent_start(&principal, &op, NULL) == DB2_MANAGEMENT_ACTION_INVALID);

   /* A tampered operation record — a token_jti below the schema's minimum, and a
    * correlation id that is not canonical 64-hex. */
   db2_identity_intent_operation_t tampered = op;
   memset(tampered.token_jti, 0, sizeof(tampered.token_jti));
   strcpy(tampered.token_jti, "short");
   assert(db2_identity_intent_start(&principal, &tampered, &out) == DB2_MANAGEMENT_ACTION_INVALID);

   tampered = op;
   tampered.correlation_id[10] = 'z';
   assert(db2_identity_intent_start(&principal, &tampered, &out) == DB2_MANAGEMENT_ACTION_INVALID);

   /* A non-zero tail past the terminator must not slip through. */
   tampered = op;
   tampered.kid[strlen(tampered.kid) + 2] = 'x';
   assert(db2_identity_intent_start(&principal, &tampered, &out) == DB2_MANAGEMENT_ACTION_INVALID);

   /* Nothing above should have opened a transaction. */
   assert(mock_rollback_count == 0);
}

static void test_start_denies_unauthenticated_scope(void)
{
   reset_mocks();
   db2_identity_intent_operation_t op = make_operation();
   kb_principal_t principal = make_principal();
   db2_identity_intent_t out;
   mock_begin_result = DB2_ERR_TENANT_UNAUTHENTICATED;
   assert(db2_identity_intent_start(&principal, &op, &out) == DB2_MANAGEMENT_ACTION_DENIED);
   mock_begin_result = DB2_ERR_TENANT_DENIED;
   assert(db2_identity_intent_start(&principal, &op, &out) == DB2_MANAGEMENT_ACTION_DENIED);
   mock_begin_result = -99;
   assert(db2_identity_intent_start(&principal, &op, &out) == DB2_MANAGEMENT_ACTION_UNAVAILABLE);
}

static void test_start_maps_sqlstates(void)
{
   db2_identity_intent_operation_t op = make_operation();
   kb_principal_t principal = make_principal();
   db2_identity_intent_t out;
   struct
   {
      const char *state;
      db2_management_action_result_t expected;
   } cases[] = {
       {"22023", DB2_MANAGEMENT_ACTION_INVALID},
       /* An ungranted subject and a non-member both arrive as 42501. */
       {"42501", DB2_MANAGEMENT_ACTION_DENIED},
       {"28000", DB2_MANAGEMENT_ACTION_DENIED},
       {"23505", DB2_MANAGEMENT_ACTION_CONFLICT},
       {"40001", DB2_MANAGEMENT_ACTION_RETRY},
       {"25006", DB2_MANAGEMENT_ACTION_RETRY},
       {"55000", DB2_MANAGEMENT_ACTION_INTEGRITY},
       {"P0002", DB2_MANAGEMENT_ACTION_INTEGRITY},
       {"XX000", DB2_MANAGEMENT_ACTION_UNAVAILABLE},
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
   {
      reset_mocks();
      mock_sqlstate = cases[i].state;
      memset(&out, 0xff, sizeof(out));
      assert(db2_identity_intent_start(&principal, &op, &out) == cases[i].expected);
      /* Every refusal rolls back and clears the output. */
      assert(mock_rollback_count == 1);
      db2_identity_intent_t zero;
      memset(&zero, 0, sizeof(zero));
      assert(!memcmp(&out, &zero, sizeof(out)));
   }
}

static void test_start_rejects_mismatched_row(void)
{
   db2_identity_intent_operation_t op = make_operation();
   kb_principal_t principal = make_principal();
   db2_identity_intent_t out;

   /* A BARE username is a valid subject — the PAM login's form, and what
    * kb_identity_token.h documents the `sub` as. It must round-trip, or the C
    * grammar and the schema CHECK disagree and one silently becomes the rule.
    * Unprefixed because a host account has one authority and the two login modes
    * are mutually exclusive, so there is no namespace to collide with. */
   reset_mocks();
   mock_bad_field = 6;
   assert(db2_identity_intent_start(&principal, &op, &out) == DB2_MANAGEMENT_ACTION_OK);
   assert(!strcmp(out.subject, "alice"));

   /* But not anything unprefixed: a leading '-' is not a username, and admitting
    * it would widen the subject column to arbitrary text. */
   reset_mocks();
   mock_bad_field = 7;
   assert(db2_identity_intent_start(&principal, &op, &out) == DB2_MANAGEMENT_ACTION_INTEGRITY);

   /* Each perturbation is a way the returned row could disagree with what was
    * asked for; every one must be INTEGRITY rather than a usable intent. */
   for (int field = 1; field <= 5; ++field)
   {
      reset_mocks();
      mock_bad_field = field;
      assert(db2_identity_intent_start(&principal, &op, &out) == DB2_MANAGEMENT_ACTION_INTEGRITY);
      assert(mock_rollback_count == 1);
   }

   /* A column count that is not the function's RETURNS TABLE width. */
   reset_mocks();
   mock_bad_shape = 1;
   assert(db2_identity_intent_start(&principal, &op, &out) == DB2_MANAGEMENT_ACTION_INTEGRITY);

   /* More than one row from a single-row function. */
   reset_mocks();
   mock_duplicate_row = 1;
   assert(db2_identity_intent_start(&principal, &op, &out) == DB2_MANAGEMENT_ACTION_INTEGRITY);
}

static void test_start_commit_ambiguous(void)
{
   reset_mocks();
   db2_identity_intent_operation_t op = make_operation();
   kb_principal_t principal = make_principal();
   db2_identity_intent_t out;
   mock_commit_failure = 1;
   memset(&out, 0xff, sizeof(out));
   assert(db2_identity_intent_start(&principal, &op, &out) ==
          DB2_MANAGEMENT_ACTION_COMMIT_AMBIGUOUS);
   /* Outputs are unusable, but the caller's operation is untouched so the retry
    * reuses the same identifiers instead of filing a second intent. */
   db2_identity_intent_operation_t again = make_operation();
   assert(strcmp(again.correlation_id, op.correlation_id));
   assert(strlen(op.correlation_id) == 64);
}

static void test_start_prepare_failure(void)
{
   reset_mocks();
   db2_identity_intent_operation_t op = make_operation();
   kb_principal_t principal = make_principal();
   db2_identity_intent_t out;
   mock_prepare_failure = 1;
   assert(db2_identity_intent_start(&principal, &op, &out) == DB2_MANAGEMENT_ACTION_UNAVAILABLE);
   assert(mock_rollback_count == 1);
}

int main(void)
{
   test_auth_mode_strings();
   test_operation_init();
   test_start_happy_path();
   test_start_rejects_bad_arguments();
   test_start_denies_unauthenticated_scope();
   test_start_maps_sqlstates();
   test_start_rejects_mismatched_row();
   test_start_commit_ambiguous();
   test_start_prepare_failure();
   printf("test_management_identity_journal: ok\n");
   return 0;
}
