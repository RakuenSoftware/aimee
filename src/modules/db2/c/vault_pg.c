/* db2/vault_pg.c: the Postgres storage backend for the kb credential vault (P10
 * slice 2). Implements vault_store_backend_t over DB2 (db2_conn() + aimee_pg_*),
 * with the SAME envelope crypto as the jsonfile backend (vault_crypto): a fresh
 * random DEK per credential, AES-KW-wrapped under the caller-supplied KEK, the
 * secret AES-256-GCM'd under the DEK with canonical versioned slot AAD.
 * Persists ONLY ciphertext — never the KEK, DEK, or plaintext. See vault_pg.h.
 *
 * Reads/writes go through the SECURITY DEFINER vault functions in db2/schema.sql
 * (org_vault_put, org_vault_get_current, org_vault_has, org_vault_list, org_vault_delete,
 * the salt/kek_check helpers, and org_vault_rewrap), so the immutable-version +
 * current-pointer discipline and the advisory-locked version counter live in one place.
 * This file is kb-only (joins KB_DB2_OBJS), never the server link. */
#include "vault_pg.h"
#include "db2_internal.h" /* db2_conn */
#include "db_postgres.h"  /* aimee_pg_* */
#include "vault_crypto.h"
#include "vault_store.h"     /* VAULT_STORE_NO_ENTRY, vault_store_entry_t */
#include "vault_principal.h" /* VAULT_PRINCIPAL_MAX */
#include "../support/db2_log.h"

#include <openssl/crypto.h> /* OPENSSL_cleanse */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VP_ERR        256
#define VP_SECRET_MAX 4096 /* max credential plaintext length (matches jsonfile) */
#define VP_AAD_MAX    VAULT_ENVELOPE_AAD_MAX
#define VP_MAX_SLOTS  512 /* per-principal slot cap for list/rekey iteration */

static db2_vault_crypto_provider_t vp_crypto;

void aimee_db2_register_vault_crypto_provider(const db2_vault_crypto_provider_t *provider)
{
   if (provider)
      vp_crypto = *provider;
   else
      memset(&vp_crypto, 0, sizeof(vp_crypto));
}

int db2_vault_crypto_random(uint8_t *out, size_t len)
{
   if (!out || len == 0 || len > VP_SECRET_MAX || !vp_crypto.random ||
       vp_crypto.random(out, len) != 0)
   {
      if (out && len <= VP_SECRET_MAX)
         OPENSSL_cleanse(out, len);
      return -1;
   }
   return 0;
}

static int vp_aad_build(int legacy, const char *principal, const char *agent, const char *cred,
                        int64_t version, uint8_t *out, size_t cap, size_t *out_len)
{
   if (out_len)
      *out_len = 0;
   if (!principal || !agent || !cred || !out || !out_len || cap == 0 || cap > VP_AAD_MAX)
      return -1;
   int rc =
       legacy
           ? (vp_crypto.aad_build_v1_safe
                  ? vp_crypto.aad_build_v1_safe(principal, agent, cred, version, out, cap, out_len)
                  : -1)
           : (vp_crypto.aad_build_v2
                  ? vp_crypto.aad_build_v2(principal, agent, cred, version, out, cap, out_len)
                  : -1);
   if (rc != 0 || *out_len == 0 || *out_len > cap)
   {
      OPENSSL_cleanse(out, cap);
      *out_len = 0;
      return -1;
   }
   return 0;
}

int db2_vault_aad_build_v2(const char *principal, const char *agent, const char *cred,
                           int64_t version, uint8_t *out, size_t cap, size_t *out_len)
{
   return vp_aad_build(0, principal, agent, cred, version, out, cap, out_len);
}

int db2_vault_aad_build_v1_safe(const char *principal, const char *agent, const char *cred,
                                int64_t version, uint8_t *out, size_t cap, size_t *out_len)
{
   return vp_aad_build(1, principal, agent, cred, version, out, cap, out_len);
}

int db2_vault_dek_wrap(const uint8_t kek[VAULT_KEK_LEN], const uint8_t dek[VAULT_DEK_LEN],
                       uint8_t wrapped[VAULT_WRAPPED_DEK_LEN])
{
   if (!kek || !dek || !wrapped || !vp_crypto.dek_wrap ||
       vp_crypto.dek_wrap(kek, dek, wrapped) != 0)
   {
      if (wrapped)
         OPENSSL_cleanse(wrapped, VAULT_WRAPPED_DEK_LEN);
      return -1;
   }
   return 0;
}

int db2_vault_dek_unwrap(const uint8_t kek[VAULT_KEK_LEN],
                         const uint8_t wrapped[VAULT_WRAPPED_DEK_LEN], uint8_t dek[VAULT_DEK_LEN])
{
   if (!kek || !wrapped || !dek || !vp_crypto.dek_unwrap ||
       vp_crypto.dek_unwrap(kek, wrapped, dek) != 0)
   {
      if (dek)
         OPENSSL_cleanse(dek, VAULT_DEK_LEN);
      return -1;
   }
   return 0;
}

int db2_vault_secret_encrypt(const uint8_t dek[VAULT_DEK_LEN], const uint8_t *aad, size_t aad_len,
                             const uint8_t *plaintext, size_t plaintext_len,
                             uint8_t nonce[VAULT_GCM_NONCE_LEN], uint8_t *ciphertext,
                             uint8_t tag[VAULT_GCM_TAG_LEN])
{
   if (!dek || (!aad && aad_len) || aad_len > VP_AAD_MAX || !plaintext ||
       plaintext_len > VP_SECRET_MAX || !nonce || !ciphertext || !tag ||
       !vp_crypto.secret_encrypt ||
       vp_crypto.secret_encrypt(dek, aad, aad_len, plaintext, plaintext_len, nonce, ciphertext,
                                tag) != 0)
   {
      if (nonce)
         OPENSSL_cleanse(nonce, VAULT_GCM_NONCE_LEN);
      if (ciphertext && plaintext_len <= VP_SECRET_MAX)
         OPENSSL_cleanse(ciphertext, plaintext_len);
      if (tag)
         OPENSSL_cleanse(tag, VAULT_GCM_TAG_LEN);
      return -1;
   }
   return 0;
}

int db2_vault_secret_decrypt(const uint8_t dek[VAULT_DEK_LEN], const uint8_t *aad, size_t aad_len,
                             const uint8_t nonce[VAULT_GCM_NONCE_LEN], const uint8_t *ciphertext,
                             size_t ciphertext_len, const uint8_t tag[VAULT_GCM_TAG_LEN],
                             uint8_t *plaintext)
{
   if (!dek || (!aad && aad_len) || aad_len > VP_AAD_MAX || !nonce || !ciphertext ||
       ciphertext_len > VP_SECRET_MAX || !tag || !plaintext || !vp_crypto.secret_decrypt ||
       vp_crypto.secret_decrypt(dek, aad, aad_len, nonce, ciphertext, ciphertext_len, tag,
                                plaintext) != 0)
   {
      if (plaintext && ciphertext_len <= VP_SECRET_MAX)
         OPENSSL_cleanse(plaintext, ciphertext_len);
      return -1;
   }
   return 0;
}

int db2_vault_kek_check_wrap(const uint8_t kek[VAULT_KEK_LEN],
                             uint8_t wrapped[VAULT_WRAPPED_DEK_LEN])
{
   if (!kek || !wrapped || !vp_crypto.kek_check_wrap || vp_crypto.kek_check_wrap(kek, wrapped) != 0)
   {
      if (wrapped)
         OPENSSL_cleanse(wrapped, VAULT_WRAPPED_DEK_LEN);
      return -1;
   }
   return 0;
}

int db2_vault_kek_check_verify(const uint8_t kek[VAULT_KEK_LEN],
                               const uint8_t wrapped[VAULT_WRAPPED_DEK_LEN])
{
   return kek && wrapped && vp_crypto.kek_check_verify &&
                  vp_crypto.kek_check_verify(kek, wrapped) == 0
              ? 0
              : -1;
}

/* Derive the tenant team_id from a slot principal. A "team:<digits>" principal
 * (optionally followed by a ':' separator) is a team-scoped tenant secret
 * (team_id = <digits>); anything else is platform-scoped (team_id NULL, e.g. the
 * future org:pki:ca-key). Returns 1 and sets *team when team-scoped, 0 for
 * platform-scoped. Overflow, no digits, or trailing non-':' garbage (e.g.
 * "team:123evil") is rejected fail-safe as platform-scoped (return 0). */
static int principal_team_id(const char *principal, int64_t *team)
{
   if (!principal || strncmp(principal, "team:", 5) != 0)
      return 0;
   const char *p = principal + 5;
   if (*p < '0' || *p > '9')
      return 0;
   errno = 0;
   char *end = NULL;
   long long v = strtoll(p, &end, 10);
   /* ERANGE => overflow; end==p => no digits; v<0 => defensive; a trailing char that
    * is neither end-of-string nor a ':' slot separator => malformed (reject). */
   if (errno == ERANGE || end == p || v < 0 || (*end != '\0' && *end != ':'))
      return 0;
   *team = (int64_t)v;
   return 1;
}

/* Run a scalar-BIGINT-returning definer function taking (p1[,p2,p3]) TEXT args.
 * `argc` in [1,3]. Writes the result to *out. Returns 0 on success, -1 on error. */
static int pg_scalar_i64(const char *sql, const char *a1, const char *a2, const char *a3, int argc,
                         int64_t *out)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[VP_ERR] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   if (argc >= 1)
      aimee_pg_bind_text(st, "?1", a1 ? a1 : "");
   if (argc >= 2)
      aimee_pg_bind_text(st, "?2", a2 ? a2 : "");
   if (argc >= 3)
      aimee_pg_bind_text(st, "?3", a3 ? a3 : "");
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int ok = -1;
   if (rc == AIMEE_PG_ROW)
   {
      if (out)
         *out = aimee_pg_column_int64(st, 0);
      ok = 0;
   }
   aimee_pg_finalize(st);
   return ok;
}

/* ── salt / verifier ──────────────────────────────────────────────────────── */

static int vault_pg_get_or_create_salt(void *ctx, const char *principal,
                                       uint8_t salt[VAULT_SALT_LEN])
{
   (void)ctx;
   if (!principal || !principal[0] || !salt)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   uint8_t fresh[VAULT_SALT_LEN];
   if (db2_vault_crypto_random(fresh, sizeof(fresh)) != 0)
      return -1;

   char err[VP_ERR] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT org_vault_salt_ensure(?1, ?2)", err, sizeof(err));
   if (!st)
   {
      OPENSSL_cleanse(fresh, sizeof(fresh));
      return -1;
   }
   aimee_pg_bind_text(st, "?1", principal);
   aimee_pg_bind_blob(st, "?2", fresh, (int)sizeof(fresh));
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int ok = -1;
   if (rc == AIMEE_PG_ROW && !aimee_pg_column_is_null(st, 0))
   {
      const void *blob = aimee_pg_column_blob(st, 0);
      int blen = aimee_pg_column_bytes(st, 0);
      if (blob && blen == VAULT_SALT_LEN)
      {
         memcpy(salt, blob, VAULT_SALT_LEN);
         ok = 0;
      }
   }
   aimee_pg_finalize(st);
   OPENSSL_cleanse(fresh, sizeof(fresh));
   if (ok != 0)
      OPENSSL_cleanse(salt, VAULT_SALT_LEN);
   return ok;
}

static int vault_pg_salt_readonly(void *ctx, const char *principal, uint8_t salt[VAULT_SALT_LEN])
{
   (void)ctx;
   if (!principal || !principal[0] || !salt)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[VP_ERR] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, "SELECT org_vault_salt_read(?1)", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", principal);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int ok = -1;
   if (rc == AIMEE_PG_ROW && !aimee_pg_column_is_null(st, 0))
   {
      const void *blob = aimee_pg_column_blob(st, 0);
      int blen = aimee_pg_column_bytes(st, 0);
      if (blob && blen == VAULT_SALT_LEN)
      {
         memcpy(salt, blob, VAULT_SALT_LEN);
         ok = 0;
      }
   }
   aimee_pg_finalize(st);
   if (ok != 0)
      OPENSSL_cleanse(salt, VAULT_SALT_LEN);
   return ok;
}

/* Read the principal's kek_check verifier. Returns: 1 => established (copied into
 * `out`, exactly VAULT_WRAPPED_DEK_LEN bytes), 0 => salt row exists but verifier not
 * yet set (empty bytea), -1 => no salt row / error. */
static int kek_check_read(void *conn, const char *principal, uint8_t out[VAULT_WRAPPED_DEK_LEN])
{
   char err[VP_ERR] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT org_vault_kek_check_read(?1)", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", principal);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int result = -1;
   if (rc == AIMEE_PG_ROW)
   {
      if (aimee_pg_column_is_null(st, 0))
      {
         result = -1; /* no salt row */
      }
      else
      {
         const void *blob = aimee_pg_column_blob(st, 0);
         int blen = aimee_pg_column_bytes(st, 0);
         if (blen == 0)
            result = 0; /* verifier not yet established */
         else if (blob && blen == VAULT_WRAPPED_DEK_LEN)
         {
            memcpy(out, blob, VAULT_WRAPPED_DEK_LEN);
            result = 1;
         }
      }
   }
   aimee_pg_finalize(st);
   return result;
}

static int kek_check_set(void *conn, const char *principal, const uint8_t kek[VAULT_KEK_LEN])
{
   uint8_t wrapped[VAULT_WRAPPED_DEK_LEN];
   if (db2_vault_kek_check_wrap(kek, wrapped) != 0)
      return -1;
   char err[VP_ERR] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT org_vault_kek_check_set(?1, ?2)", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", principal);
   aimee_pg_bind_blob(st, "?2", wrapped, (int)sizeof(wrapped));
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_ERR) ? -1 : 0;
}

/* True if `kek` unwraps `wrapped` to the sentinel. */
static int vault_pg_unlock_check(void *ctx, const char *principal, const uint8_t kek[VAULT_KEK_LEN])
{
   (void)ctx;
   if (!principal || !kek)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   uint8_t wrapped[VAULT_WRAPPED_DEK_LEN];
   int st = kek_check_read(conn, principal, wrapped);
   if (st < 0)
      return -1; /* no salt row: unlock before salt establishment */
   if (st == 0)
   {
      /* First unlock: try to establish the verifier. The set is conditional (only the
       * first writer, while kek_check is still empty, wins), so a concurrent first-
       * unlock with a DIFFERENT KEK may have won the row. Re-read the stored verifier
       * and confirm OUR KEK matches it — fail-closed if a rival KEK won. */
      if (kek_check_set(conn, principal, kek) != 0)
         return -1;
      if (kek_check_read(conn, principal, wrapped) != 1)
         return -1;
      return db2_vault_kek_check_verify(kek, wrapped);
   }
   return db2_vault_kek_check_verify(kek, wrapped);
}

/* ── set / get ────────────────────────────────────────────────────────────── */

static int vault_pg_set(void *ctx, const char *principal, const uint8_t kek[VAULT_KEK_LEN],
                        const char *agent, const char *cred, const char *secret)
{
   (void)ctx;
   if (!principal || !kek || !agent || !agent[0] || !cred || !cred[0] || !secret)
      return -1;
   size_t pt_len = strlen(secret);
   if (pt_len > VP_SECRET_MAX)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* Next version = current + 1; the AAD binds it, and org_vault_put re-checks it
    * under the advisory lock (fail-closed on a concurrent write). */
   int64_t cur = 0;
   if (pg_scalar_i64("SELECT org_vault_has(?1, ?2, ?3)", principal, agent, cred, 3, &cur) != 0)
      return -1;
   int64_t version = cur + 1;

   int64_t team = 0;
   int have_team = principal_team_id(principal, &team);

   int rc = -1;
   uint8_t dek[VAULT_DEK_LEN] = {0};
   uint8_t wrapped[VAULT_WRAPPED_DEK_LEN], nonce[VAULT_GCM_NONCE_LEN], tag[VAULT_GCM_TAG_LEN];
   uint8_t aad[VP_AAD_MAX];
   size_t aad_len = 0;
   uint8_t *ct = malloc(pt_len ? pt_len : 1);
   if (!ct)
      goto out;
   if (db2_vault_aad_build_v2(principal, agent, cred, version, aad, sizeof(aad), &aad_len) != 0)
      goto out;
   if (db2_vault_crypto_random(dek, sizeof(dek)) != 0)
      goto out;
   if (db2_vault_secret_encrypt(dek, aad, aad_len, (const uint8_t *)secret, pt_len, nonce, ct,
                                tag) != 0)
      goto out;
   if (db2_vault_dek_wrap(kek, dek, wrapped) != 0)
      goto out;

   {
      char err[VP_ERR] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(
          conn, "SELECT org_vault_put(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)", err, sizeof(err));
      if (!st)
         goto out;
      aimee_pg_bind_text(st, "?1", principal);
      if (have_team)
         aimee_pg_bind_int64(st, "?2", team);
      else
         aimee_pg_bind_null(st, "?2");
      aimee_pg_bind_text(st, "?3", agent);
      aimee_pg_bind_text(st, "?4", cred);
      aimee_pg_bind_int64(st, "?5", version);
      aimee_pg_bind_blob(st, "?6", wrapped, (int)sizeof(wrapped));
      aimee_pg_bind_blob(st, "?7", nonce, (int)sizeof(nonce));
      aimee_pg_bind_blob(st, "?8", ct, (int)pt_len);
      aimee_pg_bind_blob(st, "?9", tag, (int)sizeof(tag));
      aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
      aimee_pg_finalize(st);
      rc = (step == AIMEE_PG_ROW) ? 0 : -1;
   }

out:
   OPENSSL_cleanse(dek, sizeof(dek));
   if (ct)
   {
      if (pt_len)
         OPENSSL_cleanse(ct, pt_len);
      free(ct);
   }
   return rc;
}

static int vault_pg_get(void *ctx, const char *principal, const uint8_t kek[VAULT_KEK_LEN],
                        const char *agent, const char *cred, char *out, size_t out_len)
{
   (void)ctx;
   if (out && out_len)
      out[0] = '\0';
   if (!principal || !kek || !agent || !cred || !out || !out_len)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[VP_ERR] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT version, wrapped_dek, nonce, ciphertext, tag "
                                          "FROM org_vault_get_current(?1, ?2, ?3)",
                                          err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", principal);
   aimee_pg_bind_text(st, "?2", agent);
   aimee_pg_bind_text(st, "?3", cred);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      return (step == AIMEE_PG_ERR) ? -1 : VAULT_STORE_NO_ENTRY;
   }

   int rc = -1;
   uint8_t dek[VAULT_DEK_LEN] = {0};
   uint8_t *pt = NULL;
   uint8_t *ctbuf = NULL;
   size_t pt_alloc = 0;
   {
      /* aimee_pg_column_blob caches exactly ONE unescaped bytea at a time — each call
       * frees the previous column's buffer. This row has four bytea columns, so copy
       * each out IMMEDIATELY after reading it, before touching the next column (else
       * wrapped/nonce/ciphertext would dangle by the time the tag is read). */
      int64_t version = aimee_pg_column_int64(st, 0);
      uint8_t wrapped[VAULT_WRAPPED_DEK_LEN], nonce[VAULT_GCM_NONCE_LEN], tag[VAULT_GCM_TAG_LEN];

      const void *w = aimee_pg_column_blob(st, 1);
      int wlen = aimee_pg_column_bytes(st, 1);
      if (!w || wlen != VAULT_WRAPPED_DEK_LEN)
         goto out;
      memcpy(wrapped, w, VAULT_WRAPPED_DEK_LEN);

      const void *n = aimee_pg_column_blob(st, 2);
      int nlen = aimee_pg_column_bytes(st, 2);
      if (!n || nlen != VAULT_GCM_NONCE_LEN)
         goto out;
      memcpy(nonce, n, VAULT_GCM_NONCE_LEN);

      const void *c = aimee_pg_column_blob(st, 3);
      int clen = aimee_pg_column_bytes(st, 3);
      if (clen < 0 || (size_t)clen >= out_len)
         goto out;
      ctbuf = malloc(clen ? (size_t)clen : 1);
      if (!ctbuf)
         goto out;
      memcpy(ctbuf, c, (size_t)clen);

      const void *t = aimee_pg_column_blob(st, 4);
      int tlen = aimee_pg_column_bytes(st, 4);
      if (!t || tlen != VAULT_GCM_TAG_LEN)
         goto out;
      memcpy(tag, t, VAULT_GCM_TAG_LEN);

      pt_alloc = (size_t)clen + 1;
      pt = malloc(pt_alloc);
      if (!pt)
         goto out;
      uint8_t aad[VP_AAD_MAX];
      size_t aad_len = 0;
      if (db2_vault_aad_build_v2(principal, agent, cred, version, aad, sizeof(aad), &aad_len) != 0)
         goto out;
      if (db2_vault_dek_unwrap(kek, wrapped, dek) != 0 ||
          (db2_vault_secret_decrypt(dek, aad, aad_len, nonce, (const uint8_t *)ctbuf, (size_t)clen,
                                    tag, pt) != 0 &&
           (db2_vault_aad_build_v1_safe(principal, agent, cred, version, aad, sizeof(aad),
                                        &aad_len) != 0 ||
            db2_vault_secret_decrypt(dek, aad, aad_len, nonce, (const uint8_t *)ctbuf, (size_t)clen,
                                     tag, pt) != 0)))
         goto out; /* fail-closed: wrong KEK / tamper / AAD mismatch */
      memcpy(out, pt, (size_t)clen);
      out[clen] = '\0';
      rc = 0;
   }

out:
   aimee_pg_finalize(st);
   OPENSSL_cleanse(dek, sizeof(dek));
   if (ctbuf)
      free(ctbuf);
   if (pt)
   {
      OPENSSL_cleanse(pt, pt_alloc);
      free(pt);
   }
   if (rc != 0 && out && out_len)
      OPENSSL_cleanse(out, out_len);
   return rc;
}

/* ── has / list / delete / list_principals ────────────────────────────────── */

static int vault_pg_has_entry(void *ctx, const char *principal, const char *agent, const char *cred)
{
   (void)ctx;
   if (!principal || !agent || !cred)
      return 0;
   int64_t v = 0;
   if (pg_scalar_i64("SELECT org_vault_has(?1, ?2, ?3)", principal, agent, cred, 3, &v) != 0)
      return 0;
   return v > 0 ? 1 : 0;
}

static int vault_pg_list(void *ctx, const char *principal, vault_store_entry_t *out, int max)
{
   (void)ctx;
   if (!principal || (max > 0 && !out))
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[VP_ERR] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT agent, cred FROM org_vault_list(?1)", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", principal);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *a = aimee_pg_column_text(st, 0);
      const char *c = aimee_pg_column_text(st, 1);
      snprintf(out[n].agent, sizeof(out[n].agent), "%s", a ? a : "");
      snprintf(out[n].cred, sizeof(out[n].cred), "%s", c ? c : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

static int vault_pg_delete(void *ctx, const char *principal, const char *agent, const char *cred)
{
   (void)ctx;
   if (!principal || !agent || !cred)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[VP_ERR] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT org_vault_delete(?1, ?2, ?3)", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", principal);
   aimee_pg_bind_text(st, "?2", agent);
   aimee_pg_bind_text(st, "?3", cred);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_ERR) ? -1 : 0;
}

static int vault_pg_list_principals(void *ctx, char (*out)[VAULT_PRINCIPAL_MAX], int max)
{
   (void)ctx;
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[VP_ERR] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, "SELECT principal FROM org_vault_list_principals()",
                                          err, sizeof(err));
   if (!st)
      return -1;
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *p = aimee_pg_column_text(st, 0);
      snprintf(out[n], VAULT_PRINCIPAL_MAX, "%s", p ? p : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

/* ── rekey (re-wrap DEKs; ciphertext untouched) ───────────────────────────── */

typedef struct
{
   char agent[64];
   char cred[64];
   int64_t version;
   uint8_t rewrapped[VAULT_WRAPPED_DEK_LEN];
} vp_rewrap_t;

/* Load every current slot's (agent, cred, version, wrapped_dek), unwrap under
 * old_kek, re-wrap under new_kek. Two-pass + fail-closed: if ANY unwrap fails, write
 * nothing and return -1. On success `*n_out` gets the count and `plan` is filled with
 * the re-wraps to apply (up to VP_MAX_SLOTS). */
static int rekey_compute(void *conn, const char *principal, const uint8_t old_kek[VAULT_KEK_LEN],
                         const uint8_t new_kek[VAULT_KEK_LEN], vp_rewrap_t *plan, int *n_out)
{
   char err[VP_ERR] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT agent, cred, version, wrapped_dek FROM org_vault_current_wraps(?1)", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", principal);
   int n = 0;
   int rc = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (n >= VP_MAX_SLOTS)
      {
         rc = -1;
         break;
      }
      const char *a = aimee_pg_column_text(st, 0);
      const char *c = aimee_pg_column_text(st, 1);
      int64_t ver = aimee_pg_column_int64(st, 2);
      const void *w = aimee_pg_column_blob(st, 3);
      int wlen = aimee_pg_column_bytes(st, 3);
      uint8_t dek[VAULT_DEK_LEN];
      if (!w || wlen != VAULT_WRAPPED_DEK_LEN ||
          db2_vault_dek_unwrap(old_kek, (const uint8_t *)w, dek) != 0 ||
          db2_vault_dek_wrap(new_kek, dek, plan[n].rewrapped) != 0)
      {
         OPENSSL_cleanse(dek, sizeof(dek));
         rc = -1; /* wrong old KEK / tamper -> abort, write nothing */
         break;
      }
      OPENSSL_cleanse(dek, sizeof(dek));
      snprintf(plan[n].agent, sizeof(plan[n].agent), "%s", a ? a : "");
      snprintf(plan[n].cred, sizeof(plan[n].cred), "%s", c ? c : "");
      plan[n].version = ver;
      n++;
   }
   aimee_pg_finalize(st);
   if (rc != 0)
      return -1;
   *n_out = n;
   return 0;
}

static int rekey_apply(void *conn, const char *principal, const vp_rewrap_t *plan, int n)
{
   for (int i = 0; i < n; i++)
   {
      char err[VP_ERR] = "";
      aimee_pg_stmt_t *st =
          aimee_pg_prepare(conn, "SELECT org_vault_rewrap(?1, ?2, ?3, ?4, ?5)", err, sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_text(st, "?1", principal);
      aimee_pg_bind_text(st, "?2", plan[i].agent);
      aimee_pg_bind_text(st, "?3", plan[i].cred);
      aimee_pg_bind_int64(st, "?4", plan[i].version);
      aimee_pg_bind_blob(st, "?5", plan[i].rewrapped, (int)sizeof(plan[i].rewrapped));
      aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
      aimee_pg_finalize(st);
      if (rc == AIMEE_PG_ERR)
         return -1;
   }
   return 0;
}

static int vault_pg_rekey(void *ctx, const char *principal, const uint8_t old_kek[VAULT_KEK_LEN],
                          const uint8_t new_kek[VAULT_KEK_LEN])
{
   (void)ctx;
   if (!principal || !old_kek || !new_kek)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char txerr[VP_ERR] = "";
   if (aimee_pg_exec(conn, "BEGIN", txerr, sizeof(txerr)) != 0)
      return -1;

   /* Validate the OLD KEK against the verifier FIRST (independent of credential count,
    * so an empty vault cannot be rekeyed with a wrong old KEK). A principal with no
    * verifier was never unlocked: refuse. */
   uint8_t wrapped[VAULT_WRAPPED_DEK_LEN];
   if (kek_check_read(conn, principal, wrapped) != 1 ||
       db2_vault_kek_check_verify(old_kek, wrapped) != 0)
      goto rollback;

   vp_rewrap_t *plan = calloc(VP_MAX_SLOTS, sizeof(*plan));
   if (!plan)
      goto rollback;
   int n = 0;
   int rc = -1;
   if (rekey_compute(conn, principal, old_kek, new_kek, plan, &n) == 0 &&
       kek_check_set(conn, principal, new_kek) == 0 && rekey_apply(conn, principal, plan, n) == 0)
      rc = 0;
   OPENSSL_cleanse(plan, VP_MAX_SLOTS * sizeof(*plan));
   free(plan);
   if (rc == 0 && aimee_pg_exec(conn, "COMMIT", txerr, sizeof(txerr)) == 0)
      return 0;
rollback:
   (void)aimee_pg_exec(conn, "ROLLBACK", txerr, sizeof(txerr));
   rc = -1;
   return rc;
}

static int vault_pg_rekey_field(void *ctx, const char *principal, const char *field,
                                const uint8_t old_kek[VAULT_KEK_LEN],
                                const uint8_t new_kek[VAULT_KEK_LEN])
{
   (void)ctx;
   if (!principal || !field || !field[0] || !old_kek || !new_kek)
      return -1;
   /* The org vault has a single wrap field ("wrapped_dek"); there is no server-wrap.
    * A rekey of any other field is a no-op (nothing to re-wrap), mirroring the
    * jsonfile backend which leaves creds lacking the field untouched. */
   if (strcmp(field, "wrapped_dek") != 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   vp_rewrap_t *plan = calloc(VP_MAX_SLOTS, sizeof(*plan));
   if (!plan)
      return -1;
   int n = 0;
   int rc = -1;
   if (rekey_compute(conn, principal, old_kek, new_kek, plan, &n) == 0 &&
       rekey_apply(conn, principal, plan, n) == 0)
      rc = n; /* count re-wrapped (>=0) */
   OPENSSL_cleanse(plan, VP_MAX_SLOTS * sizeof(*plan));
   free(plan);
   return rc;
}

/* ── server-autonomous dual-wrap ops: UNSUPPORTED on the kb pg backend ────────
 * These encode the server profile's "server can read a user credential without the
 * user unlocking" model (a second server-KEK wrap). The single-KEK org vault does
 * not use it. They log and return -1 (the vault service surfaces
 * VAULT_ERR_UNSUPPORTED_OP; see vault_service.h). NULL slots are not an option — the
 * facade would crash — so they are explicit typed stubs. */
static int vault_pg_set_dual(void *ctx, const char *principal, const uint8_t kek[VAULT_KEK_LEN],
                             const uint8_t server_kek[VAULT_KEK_LEN], const char *agent,
                             const char *cred, const char *secret)
{
   (void)ctx;
   (void)principal;
   (void)kek;
   (void)server_kek;
   (void)agent;
   (void)cred;
   (void)secret;
   LOG_WARN("vault_pg", "set_dual unsupported on kb pg backend (no server-autonomous dual-wrap)");
   return -1;
}

static int vault_pg_set_server(void *ctx, const char *principal,
                               const uint8_t server_kek[VAULT_KEK_LEN], const char *agent,
                               const char *cred, const char *secret)
{
   (void)ctx;
   (void)principal;
   (void)server_kek;
   (void)agent;
   (void)cred;
   (void)secret;
   LOG_WARN("vault_pg", "set_server unsupported on kb pg backend");
   return -1;
}

static int vault_pg_get_server(void *ctx, const char *principal,
                               const uint8_t server_kek[VAULT_KEK_LEN], const char *agent,
                               const char *cred, char *out, size_t out_len)
{
   (void)ctx;
   (void)principal;
   (void)server_kek;
   (void)agent;
   (void)cred;
   if (out && out_len)
      OPENSSL_cleanse(out, out_len);
   LOG_WARN("vault_pg", "get_server unsupported on kb pg backend");
   return -1;
}

static int vault_pg_add_server_wraps(void *ctx, const char *principal,
                                     const uint8_t user_kek[VAULT_KEK_LEN],
                                     const uint8_t server_kek[VAULT_KEK_LEN])
{
   (void)ctx;
   (void)principal;
   (void)user_kek;
   (void)server_kek;
   LOG_WARN("vault_pg", "add_server_wraps unsupported on kb pg backend");
   return -1;
}

/* ── backend vtable ───────────────────────────────────────────────────────── */
const vault_store_backend_t vault_pg_backend = {
    .name = "postgres",
    .ctx = NULL,
    .get_or_create_salt = vault_pg_get_or_create_salt,
    .salt_readonly = vault_pg_salt_readonly,
    .unlock_check = vault_pg_unlock_check,
    .set = vault_pg_set,
    .set_dual = vault_pg_set_dual,
    .set_server = vault_pg_set_server,
    .get_server = vault_pg_get_server,
    .add_server_wraps = vault_pg_add_server_wraps,
    .get = vault_pg_get,
    .has_entry = vault_pg_has_entry,
    .list = vault_pg_list,
    .delete = vault_pg_delete,
    .rekey = vault_pg_rekey,
    .rekey_field = vault_pg_rekey_field,
    .list_principals = vault_pg_list_principals,
};
