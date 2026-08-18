/* management_intent_fields.h — the field grammar shared by the management-intent
 * journals (action, and the data-plane identity intent).
 *
 * These are not general string utilities. Each one encodes a CHECK constraint
 * that db2/schema.sql already enforces, so the C side refuses the same input the
 * database would rather than discovering it in a SQLSTATE. Both journals write
 * into the same kb_management_token_intent_namespace and share that grammar
 * exactly: 64-hex correlation/jti, a 32-hex installation id, token-charset kids
 * and server ids, and the canonical `owner | oidc:<iss>:<sub> | cert:<iss>:<serial>`
 * identity key. Keeping one copy means a schema change lands in one place.
 *
 * `fixed_*` validate a canonical stored record: NUL-terminated with an all-zero
 * unused tail, so a struct that came off the wire cannot smuggle bytes past the
 * terminator. `input_*` validate a caller-supplied C string with no such
 * requirement.
 *
 * Internal to db2; static inline because every caller is in this directory. */
#ifndef AIMEE_DB2_MANAGEMENT_INTENT_FIELDS_H
#define AIMEE_DB2_MANAGEMENT_INTENT_FIELDS_H

#include "db2_bounded_text.h"
#include "db_postgres.h"
#include "platform_random.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Bounds every management intent shares with the schema. */
#define DB2_INTENT_ID_HEX           64U
#define DB2_INTENT_SERVER_MAX       127U
#define DB2_INTENT_TOKEN_ISSUER_MAX 255U
#define DB2_INTENT_ACTOR_MAX        576U
#define DB2_INTENT_KID_MAX          64U
#define DB2_INTENT_INSTALL_ID_HEX   32U
#define DB2_INTENT_SERIAL_MAX       79U

static inline int db2_intent_fixed_text(const char *s, size_t cap, size_t max, int token)
{
   if (!s || cap < 2 || max >= cap)
      return 0;
   size_t n = db2_bounded_len(s, cap);
   if (n == 0 || n > max || n == cap)
      return 0;
   for (size_t i = 0; i < n; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      if (c < 0x20 || c == 0x7f ||
          (token && !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '_' || c == '-')))
         return 0;
   }
   for (size_t i = n + 1; i < cap; ++i)
      if (s[i] != 0)
         return 0;
   return 1;
}

static inline int db2_intent_input_text(const char *s, size_t max, int token)
{
   if (!s)
      return 0;
   size_t n = db2_bounded_len(s, max + 1);
   if (n == 0 || n > max)
      return 0;
   for (size_t i = 0; i < n; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      if (c < 0x20 || c == 0x7f ||
          (token && !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '_' || c == '-')))
         return 0;
   }
   return 1;
}

static inline int db2_intent_fixed_hex(const char *s, size_t cap, size_t n)
{
   if (!s || cap != n + 1 || s[n] != 0)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static inline int db2_intent_input_hex(const char *s, size_t n)
{
   if (!s || db2_bounded_len(s, n + 1) != n)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static inline void db2_intent_hex_encode_32(const uint8_t in[32], char out[65])
{
   static const char digits[] = "0123456789abcdef";
   for (size_t i = 0; i < 32; ++i)
   {
      out[i * 2] = digits[in[i] >> 4];
      out[i * 2 + 1] = digits[in[i] & 15];
   }
   out[64] = 0;
}

/* A fresh 64-hex identifier. Returns 0 on success, -1 if the platform CSPRNG
 * failed — never a weaker fallback, because these identifiers are what makes an
 * intent unguessable and single-use. */
static inline int db2_intent_generate_id(char out[65])
{
   uint8_t raw[32];
   if (platform_random_bytes(raw, sizeof(raw)) != 0)
      return -1;
   db2_intent_hex_encode_32(raw, out);
   return 0;
}

/* One component of an identity key: no control bytes, and a literal ':' must be
 * percent-encoded so the three-part key can never be re-split ambiguously. */
static inline int db2_intent_encoded_component(const char *s, size_t n)
{
   if (!s || n == 0)
      return 0;
   for (size_t i = 0; i < n; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      if (c < 0x20 || c == 0x7f || c == ':')
         return 0;
      if (c == '%')
      {
         if (i + 2 >= n ||
             !((s[i + 1] == '2' && s[i + 2] == '5') || (s[i + 1] == '3' && s[i + 2] == 'A')))
            return 0;
         i += 2;
      }
   }
   return 1;
}

/* A host account name, as authenticated by the PAM login: the bare form of a
 * subject. Unprefixed on purpose — oidc/cert are namespaced because their names
 * are unique only within an issuer, whereas a host account has exactly one
 * authority and OIDC/PAM are mutually exclusive per kb, so there is no second
 * namespace to collide with. Bounds match the Linux 32-character limit and the
 * subject CHECK in db2/schema.sql. */
static inline int db2_intent_bare_username(const char *s)
{
   /* 33 is one past the limit, so an over-long name reports 33 and is
    * rejected without an unbounded scan. */
   size_t n = db2_bounded_len(s, 33);
   if (n == 0 || n > 32)
      return 0;
   unsigned char first = (unsigned char)s[0];
   if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') ||
         (first >= '0' && first <= '9') || first == '_'))
      return 0;
   for (size_t i = 1; i < n; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-'))
         return 0;
   }
   return 1;
}

/* The canonical immutable identity key, in one of four forms: `owner`,
 * `oidc:<iss>:<sub>`, `cert:<issuer>:<serial>` (normalized lowercase-hex
 * serial), or a bare host-account `<username>`. Matches the subject CHECK on
 * both kb_write_tier_grant and every management intent — if this and that regex
 * ever disagree, one of them silently becomes the real rule, so they change
 * together. */
static inline int db2_intent_canonical_actor(const char *s, size_t cap)
{
   if (!db2_intent_fixed_text(s, cap, DB2_INTENT_ACTOR_MAX, 0))
      return 0;
   if (db2_bounded_equals(s, "owner"))
      return 1;
   /* No ':' means it can only be the bare form; the prefixed forms are checked
    * below. Note `owner` was matched above, which is why a host account named
    * `owner` is indistinguishable from the bearer principal and is reserved —
    * see the note on kb_write_tier_grant. */
   if (!db2_bounded_find(s, ':'))
      return db2_intent_bare_username(s);
   int cert = db2_bounded_prefix(s, "cert:");
   size_t prefix = cert ? 5 : (db2_bounded_prefix(s, "oidc:") ? 5 : 0);
   if (!prefix)
      return 0;
   const char *middle = db2_bounded_find(s + prefix, ':');
   if (!middle || db2_bounded_find(middle + 1, ':') ||
       !db2_intent_encoded_component(s + prefix, (size_t)(middle - (s + prefix))) ||
       !db2_intent_encoded_component(middle + 1, db2_bounded_len(middle + 1, cap)))
      return 0;
   if (cert)
   {
      size_t serial_len = db2_bounded_len(middle + 1, cap);
      if (serial_len == 0 || serial_len > DB2_INTENT_SERIAL_MAX)
         return 0;
      for (const char *p = middle + 1; *p; ++p)
         if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
            return 0;
   }
   return 1;
}

/* ---- Result-column readers -------------------------------------------------
 * Every one refuses NULL and refuses a value it cannot round-trip, so a decode
 * failure is always reported rather than silently producing a default. */

static inline int db2_intent_col_bool(aimee_pg_stmt_t *st, int col, int *out)
{
   const char *s = aimee_pg_column_text(st, col);
   if (s &&
       (db2_bounded_equals(s, "t") || db2_bounded_equals(s, "true") || db2_bounded_equals(s, "1")))
      *out = 1;
   else if (s && (db2_bounded_equals(s, "f") || db2_bounded_equals(s, "false") ||
                  db2_bounded_equals(s, "0")))
      *out = 0;
   else
      return -1;
   return 0;
}

static inline int db2_intent_col_i64(aimee_pg_stmt_t *st, int col, int64_t *out)
{
   const char *s = aimee_pg_column_text(st, col);
   char *end = NULL;
   if (!s || !*s || aimee_pg_column_is_null(st, col))
      return -1;
   errno = 0;
   long long v = strtoll(s, &end, 10);
   if (errno || !end || *end)
      return -1;
   *out = (int64_t)v;
   return 0;
}

static inline int db2_intent_col_int(aimee_pg_stmt_t *st, int col, int *out)
{
   int64_t value = 0;
   if (db2_intent_col_i64(st, col, &value) || value < INT_MIN || value > INT_MAX)
      return -1;
   *out = (int)value;
   return 0;
}

static inline int db2_intent_copy_col(aimee_pg_stmt_t *st, int col, char *out, size_t cap,
                                      size_t max, int token)
{
   const char *s = aimee_pg_column_text(st, col);
   if (aimee_pg_column_is_null(st, col) || !s)
      return -1;
   size_t n = db2_bounded_len(s, cap);
   if (n == cap || n == 0 || n > max)
      return -1;
   memset(out, 0, cap);
   memcpy(out, s, n);
   return db2_intent_fixed_text(out, cap, max, token) ? 0 : -1;
}

static inline int db2_intent_copy_hex_col(aimee_pg_stmt_t *st, int col, char *out, size_t n)
{
   if (aimee_pg_column_is_null(st, col))
      return -1;
   const char *s = aimee_pg_column_text(st, col);
   if (!s || db2_bounded_len(s, n + 1) != n)
      return -1;
   memset(out, 0, n + 1);
   memcpy(out, s, n);
   return db2_intent_fixed_hex(out, n + 1, n) ? 0 : -1;
}

#endif /* AIMEE_DB2_MANAGEMENT_INTENT_FIELDS_H */
