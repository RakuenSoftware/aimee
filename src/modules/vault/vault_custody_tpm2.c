/* vault_custody_tpm2.c: the tpm2 external-anchor custody provider (P7-tpm2a).
 *
 * BUILD-GUARDED. Two implementations selected at compile time:
 *   - #ifdef WITH_TPM2 : the real libtss2/ESAPI seal barrier. Seals the vault
 *     server KEK as a KEYEDHASH object under a persistent OWNER-hierarchy primary,
 *     and materializes it only after an authValue-gated Unseal over a salted +
 *     response-ENCRYPTED HMAC session. Validated on swtpm (see
 *     scripts/p7_tpm2_swtpm_test.sh); the parent builds this WITH_TPM2=1.
 *   - #else : a fail-closed STUB (this file compiles + links on hosts without
 *     libtss2, incl. CI). vault_custody_tpm2_provider() returns a provider that
 *     boots SEALED, is_sealed()==1 forever, and get_kek/unseal fail with a clear
 *     "aimee built without TPM2 support" — keeping KB_CUSTODY_TPM2 a known,
 *     fail-closed value with NO libtss2 dependency.
 *
 * Mirrors vault_custody_mock.c's ctx/mutex/zeroize discipline. Reuses the vault's
 * HKDF-SHA256 (vault_kek_derive) for the secret->authValue derivation (a
 * domain-separation step, NOT password-hardening — the operator secret is a
 * high-entropy credential and the TPM's dictionary-attack protection is ON). */
#include "vault_custody_tpm2.h"
#include <string.h>

static void tpm2_set_err(char *errbuf, size_t errlen, const char *msg)
{
   if (errbuf && errlen)
   {
      strncpy(errbuf, msg, errlen - 1);
      errbuf[errlen - 1] = '\0';
   }
}

/* ══════════════════════════════════════════════════════════════════════════════
 * WITH_TPM2: the real ESAPI provider
 * ════════════════════════════════════════════════════════════════════════════ */
#ifdef WITH_TPM2

#include "config.h"        /* config_load, config_default_dir, CONFIG_DEFAULT_VAULT_TPM2_TCTI */
#include "platform_path.h" /* platform_mkdir_p */
#include "vault_crypto.h"  /* vault_kek_derive, VAULT_ROOT_KEY_LEN, VAULT_SALT_LEN */
#include <fcntl.h>
#include <openssl/crypto.h> /* OPENSSL_cleanse */
#include <openssl/sha.h>    /* SHA256 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h> /* mlock (best-effort) */
#include <sys/stat.h>
#include <unistd.h>

#include <tss2/tss2_esys.h>
#include <tss2/tss2_mu.h>
#include <tss2/tss2_tctildr.h>

/* The persistent handle in the OWNER hierarchy that holds our storage primary.
 * Fixed default per the plan; the blob is only loadable under THIS primary. */
#define TPM2_KEK_PERSIST_HANDLE 0x81018001

/* On-disk blob framing: magic(8) | be32 pub_len | be32 priv_len | pub | priv. */
static const uint8_t TPM2_BLOB_MAGIC[8] = {'A', 'I', 'M', 'T', 'P', 'M', '2', '\0'};
#define TPM2_BLOB_HDR_LEN 16
#define TPM2_BLOB_MAX     8192

typedef struct
{
   pthread_mutex_t mu; /* guards every field below (mirrors the mock's discipline) */
   int init_done;      /* config resolved + TCTI/ESYS initialized */
   int sealed;         /* 1 = sealed (get_kek fails); starts 1 */
   int kek_ready;      /* 1 once a KEK has been unsealed into `kek` */
   uint8_t kek[VAULT_KEK_LEN];

   TSS2_TCTI_CONTEXT *tcti;
   ESYS_CONTEXT *esys;
   ESYS_TR primary; /* verified persistent primary; ESYS_TR_NONE until set */
   TPMI_DH_PERSISTENT persistent_handle;
   char blob_path[1024];
   char tcti_conf[256];
} tpm2_ctx_t;

static tpm2_ctx_t g_ctx = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .sealed = 1,
    .primary = ESYS_TR_NONE,
    .persistent_handle = TPM2_KEK_PERSIST_HANDLE,
};

/* Domain-separation salt for the secret->authValue HKDF (distinct from the file
 * provider's SERVER_KEK_SALT and the mock's salt). NOT password-hardening. */
static const uint8_t TPM2_AUTH_SALT[VAULT_SALT_LEN] = {'a', 'i', 'm', 'e', 'e', '-', 't',  'p',
                                                       'm', '2', '-', 'a', 'v', '1', 0x00, 0x00};

static void put_be32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)(v >> 24);
   p[1] = (uint8_t)(v >> 16);
   p[2] = (uint8_t)(v >> 8);
   p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p)
{
   return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* HKDF-derive a 32-byte authValue from the operator secret (domain-separated). */
static int derive_authvalue(const void *secret, size_t len, TPM2B_AUTH *out)
{
   if (!out || (!secret && len))
      return -1;
   uint8_t root[VAULT_ROOT_KEY_LEN];
   SHA256((const unsigned char *)secret, len, root); /* secret -> 32-byte root */
   uint8_t av[VAULT_KEK_LEN];
   int rc = vault_kek_derive(root, sizeof(root), TPM2_AUTH_SALT, sizeof(TPM2_AUTH_SALT), av);
   OPENSSL_cleanse(root, sizeof(root));
   if (rc != 0)
   {
      OPENSSL_cleanse(av, sizeof(av));
      return -1;
   }
   out->size = VAULT_KEK_LEN;
   memcpy(out->buffer, av, VAULT_KEK_LEN);
   OPENSSL_cleanse(av, sizeof(av));
   return 0;
}

/* FIXED, deterministic OWNER-hierarchy storage/parent primary (ECC P-256): so
 * its Name is reproducible from the template + the TPM seed and can be VERIFIED. */
static TPM2B_PUBLIC primary_template(void)
{
   TPM2B_PUBLIC t;
   memset(&t, 0, sizeof(t));
   t.size = 0;
   t.publicArea.type = TPM2_ALG_ECC;
   t.publicArea.nameAlg = TPM2_ALG_SHA256;
   t.publicArea.objectAttributes = TPMA_OBJECT_FIXEDTPM | TPMA_OBJECT_FIXEDPARENT |
                                   TPMA_OBJECT_SENSITIVEDATAORIGIN | TPMA_OBJECT_USERWITHAUTH |
                                   TPMA_OBJECT_RESTRICTED | TPMA_OBJECT_DECRYPT;
   t.publicArea.authPolicy.size = 0;
   t.publicArea.parameters.eccDetail.symmetric.algorithm = TPM2_ALG_AES;
   t.publicArea.parameters.eccDetail.symmetric.keyBits.aes = 128;
   t.publicArea.parameters.eccDetail.symmetric.mode.aes = TPM2_ALG_CFB;
   t.publicArea.parameters.eccDetail.scheme.scheme = TPM2_ALG_NULL;
   t.publicArea.parameters.eccDetail.curveID = TPM2_ECC_NIST_P256;
   t.publicArea.parameters.eccDetail.kdf.scheme = TPM2_ALG_NULL;
   t.publicArea.unique.ecc.x.size = 0;
   t.publicArea.unique.ecc.y.size = 0;
   return t;
}

/* PINNED sealed-data template: KEYEDHASH, scheme NULL (pure sealed data), attrs =
 * fixedTPM|fixedParent|userWithAuth. NO sensitiveDataOrigin (we supply the KEK),
 * NO adminWithPolicy, noDA CLEAR (TPM dictionary-attack protection stays ON). */
static TPM2B_PUBLIC seal_template(void)
{
   TPM2B_PUBLIC t;
   memset(&t, 0, sizeof(t));
   t.size = 0;
   t.publicArea.type = TPM2_ALG_KEYEDHASH;
   t.publicArea.nameAlg = TPM2_ALG_SHA256;
   t.publicArea.objectAttributes =
       TPMA_OBJECT_FIXEDTPM | TPMA_OBJECT_FIXEDPARENT | TPMA_OBJECT_USERWITHAUTH;
   t.publicArea.authPolicy.size = 0;
   t.publicArea.parameters.keyedHashDetail.scheme.scheme = TPM2_ALG_NULL;
   t.publicArea.unique.keyedHash.size = 0;
   return t;
}

/* Resolve config (TCTI string + blob path) and lazily init TCTI + ESYS once.
 * Env overrides AIMEE_VAULT_TPM2_TCTI / AIMEE_VAULT_TPM2_BLOB_PATH win (used by
 * the swtpm test harness); otherwise vault.tpm2.tcti / vault.tpm2.blob_path from
 * config. 0 on success, -1 fail-closed. Caller holds ctx->mu. */
static int ensure_ready(tpm2_ctx_t *ctx)
{
   if (ctx->init_done)
      return 0;

   config_t *cfg = calloc(1, sizeof(*cfg));
   if (!cfg)
      return -1;
   (void)config_load(cfg); /* config_set_defaults runs first, so fields are populated */

   const char *env_tcti = getenv("AIMEE_VAULT_TPM2_TCTI");
   const char *tcti = (env_tcti && env_tcti[0])        ? env_tcti
                      : (cfg->vault_tpm2_tcti[0] != 0) ? cfg->vault_tpm2_tcti
                                                       : CONFIG_DEFAULT_VAULT_TPM2_TCTI;
   snprintf(ctx->tcti_conf, sizeof(ctx->tcti_conf), "%s", tcti);

   const char *env_blob = getenv("AIMEE_VAULT_TPM2_BLOB_PATH");
   if (env_blob && env_blob[0])
      snprintf(ctx->blob_path, sizeof(ctx->blob_path), "%s", env_blob);
   else if (cfg->vault_tpm2_blob_path[0])
      snprintf(ctx->blob_path, sizeof(ctx->blob_path), "%s", cfg->vault_tpm2_blob_path);
   else
   {
      const char *base = config_default_dir();
      if (!base || !base[0])
      {
         free(cfg);
         return -1;
      }
      snprintf(ctx->blob_path, sizeof(ctx->blob_path), "%s/vault/tpm2-kek.blob", base);
   }
   free(cfg);

   TSS2_RC rc = Tss2_TctiLdr_Initialize(ctx->tcti_conf, &ctx->tcti);
   if (rc != TSS2_RC_SUCCESS || !ctx->tcti)
   {
      ctx->tcti = NULL;
      return -1;
   }
   rc = Esys_Initialize(&ctx->esys, ctx->tcti, NULL);
   if (rc != TSS2_RC_SUCCESS || !ctx->esys)
   {
      ctx->esys = NULL;
      Tss2_TctiLdr_Finalize(&ctx->tcti);
      ctx->tcti = NULL;
      return -1;
   }
   (void)mlock(ctx->kek, sizeof(ctx->kek)); /* best-effort: keep the KEK off swap */
   ctx->init_done = 1;
   return 0;
}

/* VERIFY a persistent object's Name equals the Name of a freshly created transient
 * primary from the FIXED template (deterministic under the same TPM seed). A
 * mismatch/absence => fail closed (never seal/unseal under an unknown parent).
 * Caller holds ctx->mu. Returns 0 if the Names match, -1 otherwise. */
static int verify_primary_name(tpm2_ctx_t *ctx, ESYS_TR persist)
{
   TPM2B_NAME *persist_name = NULL;
   TPM2B_NAME *trans_name = NULL;
   TPM2B_PUBLIC *trans_pub = NULL;
   ESYS_TR transient = ESYS_TR_NONE;
   int ok = -1;

   TSS2_RC rc = Esys_ReadPublic(ctx->esys, persist, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, NULL,
                                &persist_name, NULL);
   if (rc != TSS2_RC_SUCCESS)
      return -1;

   TPM2B_PUBLIC tmpl = primary_template();
   TPM2B_SENSITIVE_CREATE in_sens;
   memset(&in_sens, 0, sizeof(in_sens));
   TPML_PCR_SELECTION pcr = {.count = 0};
   TPM2B_DATA outside = {.size = 0};

   rc = Esys_CreatePrimary(ctx->esys, ESYS_TR_RH_OWNER, ESYS_TR_PASSWORD, ESYS_TR_NONE,
                           ESYS_TR_NONE, &in_sens, &tmpl, &outside, &pcr, &transient, &trans_pub,
                           NULL, NULL, NULL);
   if (rc == TSS2_RC_SUCCESS)
   {
      rc = Esys_ReadPublic(ctx->esys, transient, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, NULL,
                           &trans_name, NULL);
      if (rc == TSS2_RC_SUCCESS && trans_name && persist_name &&
          trans_name->size == persist_name->size &&
          memcmp(trans_name->name, persist_name->name, persist_name->size) == 0)
         ok = 0;
      Esys_FlushContext(ctx->esys, transient);
   }

   Esys_Free(trans_pub);
   Esys_Free(trans_name);
   Esys_Free(persist_name);
   return ok;
}

/* Ensure ctx->primary is a VERIFIED handle to our persistent storage primary. If
 * the handle is absent and allow_create is set (provision only), create it and
 * EvictControl it to the persistent handle. Caller holds ctx->mu. 0/-1. */
static int ensure_primary(tpm2_ctx_t *ctx, int allow_create)
{
   if (ctx->primary != ESYS_TR_NONE)
      return 0;

   ESYS_TR persist = ESYS_TR_NONE;
   TSS2_RC rc = Esys_TR_FromTPMPublic(ctx->esys, ctx->persistent_handle, ESYS_TR_NONE, ESYS_TR_NONE,
                                      ESYS_TR_NONE, &persist);
   if (rc == TSS2_RC_SUCCESS)
   {
      if (verify_primary_name(ctx, persist) != 0)
      {
         Esys_TR_Close(ctx->esys, &persist); /* unknown/attacker object -> fail closed */
         return -1;
      }
      ctx->primary = persist;
      return 0;
   }
   if (!allow_create)
      return -1;

   /* Persistent handle empty: create the primary + evict it (provision path). */
   TPM2B_PUBLIC tmpl = primary_template();
   TPM2B_SENSITIVE_CREATE in_sens;
   memset(&in_sens, 0, sizeof(in_sens));
   TPML_PCR_SELECTION pcr = {.count = 0};
   TPM2B_DATA outside = {.size = 0};
   ESYS_TR transient = ESYS_TR_NONE;
   TPM2B_PUBLIC *out_pub = NULL;

   rc =
       Esys_CreatePrimary(ctx->esys, ESYS_TR_RH_OWNER, ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE,
                          &in_sens, &tmpl, &outside, &pcr, &transient, &out_pub, NULL, NULL, NULL);
   if (rc != TSS2_RC_SUCCESS)
      return -1;
   Esys_Free(out_pub);

   ESYS_TR new_persist = ESYS_TR_NONE;
   rc = Esys_EvictControl(ctx->esys, ESYS_TR_RH_OWNER, transient, ESYS_TR_PASSWORD, ESYS_TR_NONE,
                          ESYS_TR_NONE, ctx->persistent_handle, &new_persist);
   Esys_FlushContext(ctx->esys, transient); /* the transient copy is done with */
   if (rc != TSS2_RC_SUCCESS)
      return -1;
   ctx->primary = new_persist;
   return 0;
}

/* Atomically persist the sealed blob (tmp + O_EXCL|O_NOFOLLOW 0600 + fsync +
 * rename + parent fsync). 0/-1. */
static int blob_write(const char *path, const TPM2B_PUBLIC *pub, const TPM2B_PRIVATE *priv)
{
   uint8_t buf[TPM2_BLOB_MAX];
   size_t pub_off = 0, priv_off = 0;
   memcpy(buf, TPM2_BLOB_MAGIC, sizeof(TPM2_BLOB_MAGIC));
   if (Tss2_MU_TPM2B_PUBLIC_Marshal(pub, buf + TPM2_BLOB_HDR_LEN, sizeof(buf) - TPM2_BLOB_HDR_LEN,
                                    &pub_off) != TSS2_RC_SUCCESS)
      return -1;
   if (Tss2_MU_TPM2B_PRIVATE_Marshal(priv, buf + TPM2_BLOB_HDR_LEN + pub_off,
                                     sizeof(buf) - TPM2_BLOB_HDR_LEN - pub_off,
                                     &priv_off) != TSS2_RC_SUCCESS)
      return -1;
   put_be32(buf + 8, (uint32_t)pub_off);
   put_be32(buf + 12, (uint32_t)priv_off);
   size_t total = TPM2_BLOB_HDR_LEN + pub_off + priv_off;

   /* mkdir -p the parent directory (mode 0700). */
   char dir[1024];
   snprintf(dir, sizeof(dir), "%s", path);
   char *slash = strrchr(dir, '/');
   if (slash && slash != dir)
   {
      *slash = '\0';
      if (platform_mkdir_p(dir, 0700) != 0)
         return -1;
   }

   char tmp[1152];
   if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid()) >= sizeof(tmp))
      return -1;
   int rc = -1;
   int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC | O_EXCL | O_NOFOLLOW, 0600);
   if (fd >= 0)
   {
      ssize_t w = write(fd, buf, total);
      if (w == (ssize_t)total && fsync(fd) == 0)
         rc = 0;
      close(fd);
      if (rc == 0 && rename(tmp, path) != 0)
         rc = -1;
      if (rc != 0)
         unlink(tmp);
   }
   if (rc == 0)
   {
      int dfd = open(dir[0] ? dir : ".", O_RDONLY);
      if (dfd >= 0)
      {
         (void)fsync(dfd);
         close(dfd);
      }
   }
   return rc;
}

/* Load + defensively unmarshal the sealed blob (bounded read; reject truncated/
 * oversized/bad-magic). 0/-1. */
static int blob_read(const char *path, TPM2B_PUBLIC *pub, TPM2B_PRIVATE *priv)
{
   int fd = open(path, O_RDONLY | O_NOFOLLOW);
   if (fd < 0)
      return -1;
   struct stat st;
   if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < TPM2_BLOB_HDR_LEN ||
       st.st_size > TPM2_BLOB_MAX)
   {
      close(fd);
      return -1;
   }
   uint8_t buf[TPM2_BLOB_MAX];
   size_t want = (size_t)st.st_size;
   size_t got = 0;
   while (got < want)
   {
      ssize_t n = read(fd, buf + got, want - got);
      if (n <= 0)
         break;
      got += (size_t)n;
   }
   close(fd);
   if (got != want)
      return -1;
   if (memcmp(buf, TPM2_BLOB_MAGIC, sizeof(TPM2_BLOB_MAGIC)) != 0)
      return -1;
   uint32_t pub_len = get_be32(buf + 8);
   uint32_t priv_len = get_be32(buf + 12);
   /* Bounds: header + pub + priv must exactly cover the file (no slack/overflow). */
   if ((uint64_t)TPM2_BLOB_HDR_LEN + pub_len + priv_len != (uint64_t)want)
      return -1;

   size_t off = 0;
   memset(pub, 0, sizeof(*pub));
   memset(priv, 0, sizeof(*priv));
   if (Tss2_MU_TPM2B_PUBLIC_Unmarshal(buf + TPM2_BLOB_HDR_LEN, pub_len, &off, pub) !=
           TSS2_RC_SUCCESS ||
       off != pub_len)
      return -1;
   off = 0;
   if (Tss2_MU_TPM2B_PRIVATE_Unmarshal(buf + TPM2_BLOB_HDR_LEN + pub_len, priv_len, &off, priv) !=
           TSS2_RC_SUCCESS ||
       off != priv_len)
      return -1;
   return 0;
}

/* ── vtable ops ──────────────────────────────────────────────────────────────── */

static int tpm2_get_kek(void *vctx, uint8_t kek[VAULT_KEK_LEN])
{
   tpm2_ctx_t *ctx = vctx;
   if (!kek || !ctx)
      return -1;
   pthread_mutex_lock(&ctx->mu);
   int rc = 0;
   if (ctx->sealed || !ctx->kek_ready)
   {
      OPENSSL_cleanse(kek, VAULT_KEK_LEN); /* sealed anchor yields no key (P7 §3) */
      rc = -1;
   }
   else
      memcpy(kek, ctx->kek, VAULT_KEK_LEN);
   pthread_mutex_unlock(&ctx->mu);
   return rc;
}

static int tpm2_rotate(void *vctx, const char *server_principal, int *out_principals,
                       int *out_creds, char *backup_path, size_t backup_path_len, char *errbuf,
                       size_t errlen)
{
   (void)vctx;
   (void)server_principal;
   (void)out_principals;
   (void)out_creds;
   (void)backup_path;
   (void)backup_path_len;
   tpm2_set_err(errbuf, errlen, "rotate unsupported on tpm2 until P7-tpm2b (NV anti-rollback)");
   return -1;
}

static int tpm2_is_sealed(void *vctx)
{
   tpm2_ctx_t *ctx = vctx;
   if (!ctx)
      return 1;
   pthread_mutex_lock(&ctx->mu);
   int s = ctx->sealed;
   pthread_mutex_unlock(&ctx->mu);
   return s;
}

/* Unseal: Load the blob under the verified primary, set the sealed object's auth
 * to the derived authValue, then Unseal over a SALTED (to the primary) + response-
 * ENCRYPTED HMAC session so the recovered KEK is transport-encrypted TPM->caller.
 * Any failure -> stays sealed, -1. Flushes the sealed object + session on EVERY
 * path (incl. error). */
static int tpm2_unseal(void *vctx, const void *params, size_t len)
{
   tpm2_ctx_t *ctx = vctx;
   if (!ctx || (!params && len))
      return -1;

   pthread_mutex_lock(&ctx->mu);
   int rc = -1;
   ESYS_TR sealed = ESYS_TR_NONE;
   ESYS_TR session = ESYS_TR_NONE;
   TPM2B_SENSITIVE_DATA *out_data = NULL;
   TPM2B_PUBLIC pub;
   TPM2B_PRIVATE priv;
   TPM2B_AUTH auth;
   memset(&auth, 0, sizeof(auth));
   memset(&pub, 0, sizeof(pub));
   memset(&priv, 0, sizeof(priv));

   if (ensure_ready(ctx) != 0)
      goto out;
   if (ensure_primary(ctx, 0) != 0) /* NO create on the unseal path */
      goto out;
   if (blob_read(ctx->blob_path, &pub, &priv) != 0)
      goto out;

   TSS2_RC trc = Esys_Load(ctx->esys, ctx->primary, ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE,
                           &priv, &pub, &sealed);
   if (trc != TSS2_RC_SUCCESS)
      goto out;

   if (derive_authvalue(params, len, &auth) != 0)
      goto out;
   trc = Esys_TR_SetAuth(ctx->esys, sealed, &auth); /* auth lives ON the object */
   if (trc != TSS2_RC_SUCCESS)
      goto out;

   /* Salted (tpmKey = our primary) + response-parameter-encrypted HMAC session so
    * the unsealed KEK never crosses the TCTI transport in plaintext. */
   TPMT_SYM_DEF sym;
   memset(&sym, 0, sizeof(sym));
   sym.algorithm = TPM2_ALG_AES;
   sym.keyBits.aes = 128;
   sym.mode.aes = TPM2_ALG_CFB;
   trc = Esys_StartAuthSession(ctx->esys, ctx->primary, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
                               ESYS_TR_NONE, NULL, TPM2_SE_HMAC, &sym, TPM2_ALG_SHA256, &session);
   if (trc != TSS2_RC_SUCCESS)
      goto out;
   trc = Esys_TRSess_SetAttributes(ctx->esys, session,
                                   TPMA_SESSION_CONTINUESESSION | TPMA_SESSION_ENCRYPT,
                                   TPMA_SESSION_CONTINUESESSION | TPMA_SESSION_ENCRYPT);
   if (trc != TSS2_RC_SUCCESS)
      goto out;

   trc = Esys_Unseal(ctx->esys, sealed, session, ESYS_TR_NONE, ESYS_TR_NONE, &out_data);
   if (trc != TSS2_RC_SUCCESS)
      goto out;
   if (!out_data || out_data->size != VAULT_KEK_LEN)
      goto out;

   memcpy(ctx->kek, out_data->buffer, VAULT_KEK_LEN);
   ctx->kek_ready = 1;
   ctx->sealed = 0;
   rc = 0;

out:
   if (out_data)
   {
      OPENSSL_cleanse(out_data->buffer, sizeof(out_data->buffer));
      Esys_Free(out_data);
   }
   OPENSSL_cleanse(&auth, sizeof(auth));
   OPENSSL_cleanse(&priv, sizeof(priv));
   if (session != ESYS_TR_NONE)
      Esys_FlushContext(ctx->esys, session);
   if (sealed != ESYS_TR_NONE)
      Esys_FlushContext(ctx->esys, sealed);
   pthread_mutex_unlock(&ctx->mu);
   return rc;
}

static int tpm2_seal(void *vctx)
{
   tpm2_ctx_t *ctx = vctx;
   if (!ctx)
      return -1;
   pthread_mutex_lock(&ctx->mu);
   OPENSSL_cleanse(ctx->kek, sizeof(ctx->kek)); /* flush the materialized KEK */
   ctx->kek_ready = 0;
   ctx->sealed = 1;
   pthread_mutex_unlock(&ctx->mu);
   return 0; /* the on-disk blob stays; a later unseal re-materializes */
}

static const vault_custody_provider_t g_tpm2_provider = {
    .name = "tpm2",
    .ctx = &g_ctx,
    .get_kek = tpm2_get_kek,
    .rotate = tpm2_rotate,
    .is_sealed = tpm2_is_sealed,
    .unseal = tpm2_unseal,
    .seal = tpm2_seal,
};

const vault_custody_provider_t *vault_custody_tpm2_provider(void)
{
   return &g_tpm2_provider;
}

int vault_custody_tpm2_provision(const uint8_t kek[VAULT_KEK_LEN], const char *secret, char *errbuf,
                                 size_t errlen)
{
   if (!kek || !secret)
   {
      tpm2_set_err(errbuf, errlen, "tpm2 provision: null kek or secret");
      return -1;
   }
   pthread_mutex_lock(&g_ctx.mu);
   int rc = -1;
   ESYS_TR sess = ESYS_TR_NONE;
   TPM2B_AUTH auth;
   memset(&auth, 0, sizeof(auth));
   TPM2B_SENSITIVE_CREATE in_sens;
   memset(&in_sens, 0, sizeof(in_sens));
   TPM2B_PRIVATE *out_priv = NULL;
   TPM2B_PUBLIC *out_pub = NULL;

   if (ensure_ready(&g_ctx) != 0)
   {
      tpm2_set_err(errbuf, errlen, "tpm2 provision: TCTI/ESYS init failed");
      goto out;
   }
   if (access(g_ctx.blob_path, F_OK) == 0) /* CREATE-ONCE: refuse if a blob exists */
   {
      tpm2_set_err(errbuf, errlen, "tpm2 provision: blob already exists (create-once)");
      goto out;
   }
   if (ensure_primary(&g_ctx, 1) != 0)
   {
      tpm2_set_err(errbuf, errlen, "tpm2 provision: primary create/verify failed");
      goto out;
   }
   if (derive_authvalue(secret, strlen(secret), &auth) != 0)
   {
      tpm2_set_err(errbuf, errlen, "tpm2 provision: authValue derivation failed");
      goto out;
   }

   TPM2B_PUBLIC tmpl = seal_template();
   in_sens.sensitive.userAuth = auth; /* struct copy */
   in_sens.sensitive.data.size = VAULT_KEK_LEN;
   memcpy(in_sens.sensitive.data.buffer, kek, VAULT_KEK_LEN);
   TPML_PCR_SELECTION pcr = {.count = 0};
   TPM2B_DATA outside = {.size = 0};

   /* Salted (tpmKey = our primary) + command-parameter-DECRYPT session so the
    * inSensitive area (the KEK) is ENCRYPTED toward the TPM — symmetric with the
    * unseal path's response ENCRYPT. Without this the KEK would cross the TCTI
    * transport in plaintext on the seal command. */
   TPMT_SYM_DEF psym;
   memset(&psym, 0, sizeof(psym));
   psym.algorithm = TPM2_ALG_AES;
   psym.keyBits.aes = 128;
   psym.mode.aes = TPM2_ALG_CFB;
   TSS2_RC trc = Esys_StartAuthSession(g_ctx.esys, g_ctx.primary, ESYS_TR_NONE, ESYS_TR_NONE,
                                       ESYS_TR_NONE, ESYS_TR_NONE, NULL, TPM2_SE_HMAC, &psym,
                                       TPM2_ALG_SHA256, &sess);
   if (trc != TSS2_RC_SUCCESS)
   {
      tpm2_set_err(errbuf, errlen, "tpm2 provision: StartAuthSession failed");
      goto out;
   }
   trc = Esys_TRSess_SetAttributes(g_ctx.esys, sess,
                                   TPMA_SESSION_CONTINUESESSION | TPMA_SESSION_DECRYPT,
                                   TPMA_SESSION_CONTINUESESSION | TPMA_SESSION_DECRYPT);
   if (trc != TSS2_RC_SUCCESS)
   {
      tpm2_set_err(errbuf, errlen, "tpm2 provision: session attrs failed");
      goto out;
   }

   trc = Esys_Create(g_ctx.esys, g_ctx.primary, sess, ESYS_TR_NONE, ESYS_TR_NONE, &in_sens, &tmpl,
                     &outside, &pcr, &out_priv, &out_pub, NULL, NULL, NULL);
   OPENSSL_cleanse(&in_sens, sizeof(in_sens)); /* KEK + authValue leave RAM ASAP */
   OPENSSL_cleanse(&auth, sizeof(auth));
   if (trc != TSS2_RC_SUCCESS)
   {
      tpm2_set_err(errbuf, errlen, "tpm2 provision: Esys_Create(seal) failed");
      goto out;
   }
   if (blob_write(g_ctx.blob_path, out_pub, out_priv) != 0)
   {
      tpm2_set_err(errbuf, errlen, "tpm2 provision: sealed blob write failed");
      goto out;
   }
   rc = 0;

out:
   OPENSSL_cleanse(&in_sens, sizeof(in_sens));
   OPENSSL_cleanse(&auth, sizeof(auth));
   if (sess != ESYS_TR_NONE)
      Esys_FlushContext(g_ctx.esys, sess);
   Esys_Free(out_pub);
   Esys_Free(out_priv);
   pthread_mutex_unlock(&g_ctx.mu);
   return rc;
}

void vault_custody_tpm2_reset(void)
{
   pthread_mutex_lock(&g_ctx.mu);
   OPENSSL_cleanse(g_ctx.kek, sizeof(g_ctx.kek));
   g_ctx.kek_ready = 0;
   g_ctx.sealed = 1;
   if (g_ctx.primary != ESYS_TR_NONE && g_ctx.esys)
      Esys_TR_Close(g_ctx.esys, &g_ctx.primary);
   g_ctx.primary = ESYS_TR_NONE;
   if (g_ctx.esys)
      Esys_Finalize(&g_ctx.esys); /* finalize ESYS before the TCTI (lifecycle order) */
   g_ctx.esys = NULL;
   if (g_ctx.tcti)
      Tss2_TctiLdr_Finalize(&g_ctx.tcti);
   g_ctx.tcti = NULL;
   g_ctx.init_done = 0;
   pthread_mutex_unlock(&g_ctx.mu);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * #else : the fail-closed STUB (default build; no libtss2)
 * ════════════════════════════════════════════════════════════════════════════ */
#else

#include <stddef.h>

/* The fail-closed message surfaced when built without TPM2 support. */
static const char TPM2_NO_SUPPORT_MSG[] = "aimee built without TPM2 support (rebuild WITH_TPM2=1)";

static int stub_get_kek(void *vctx, uint8_t kek[VAULT_KEK_LEN])
{
   (void)vctx;
   if (kek)
      memset(kek, 0, VAULT_KEK_LEN);
   return -1; /* built without TPM2 -> never yields a key (fail closed) */
}

static int stub_rotate(void *vctx, const char *server_principal, int *out_principals,
                       int *out_creds, char *backup_path, size_t backup_path_len, char *errbuf,
                       size_t errlen)
{
   (void)vctx;
   (void)server_principal;
   (void)out_principals;
   (void)out_creds;
   (void)backup_path;
   (void)backup_path_len;
   tpm2_set_err(errbuf, errlen, TPM2_NO_SUPPORT_MSG);
   return -1;
}

static int stub_is_sealed(void *vctx)
{
   (void)vctx;
   return 1; /* always sealed -> kb_vault_live_keys_allowed() stays false */
}

static int stub_unseal(void *vctx, const void *params, size_t len)
{
   (void)vctx;
   (void)params;
   (void)len;
   return -1; /* cannot unseal without libtss2 (fail closed) */
}

static int stub_seal(void *vctx)
{
   (void)vctx;
   return 0; /* already sealed: no-op success */
}

static const vault_custody_provider_t g_tpm2_stub = {
    .name = "tpm2",
    .ctx = NULL,
    .get_kek = stub_get_kek,
    .rotate = stub_rotate,
    .is_sealed = stub_is_sealed,
    .unseal = stub_unseal,
    .seal = stub_seal,
};

const vault_custody_provider_t *vault_custody_tpm2_provider(void)
{
   return &g_tpm2_stub;
}

int vault_custody_tpm2_provision(const uint8_t kek[VAULT_KEK_LEN], const char *secret, char *errbuf,
                                 size_t errlen)
{
   (void)kek;
   (void)secret;
   tpm2_set_err(errbuf, errlen, TPM2_NO_SUPPORT_MSG);
   return -1;
}

void vault_custody_tpm2_reset(void)
{
   /* nothing to reset in the stub */
}

#endif /* WITH_TPM2 */
