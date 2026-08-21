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
#include "vault_server_key.h"
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

#include "config.h" /* legacy_config_read, config_default_dir, CONFIG_DEFAULT_VAULT_TPM2_TCTI */
#include "platform_path.h" /* platform_mkdir_p */
#include "vault_crypto.h"  /* vault_kek_derive, VAULT_ROOT_KEY_LEN, VAULT_SALT_LEN */
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h> /* OPENSSL_cleanse */
#include <openssl/sha.h>    /* SHA256 */
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h> /* mlock (best-effort) */
#include <sys/file.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <tss2/tss2_esys.h>
#include <tss2/tss2_mu.h>
#include <tss2/tss2_tctildr.h>

/* The persistent handle in the OWNER hierarchy that holds our storage primary.
 * Fixed default per the plan; the blob is only loadable under THIS primary. */
#define TPM2_KEK_PERSIST_HANDLE 0x81018001

/* On-disk blob framing.
 *   v1 (tpm2a, generation-less): magic8 = "AIMTPM2\0" | be32 pub_len | be32 priv_len
 *      | pub | priv.  P7-tpm2b REFUSES a v1 blob (no PolicyNV binding = rollback hole).
 *   v2 (tpm2b, PolicyNV): magic8 = "AIMTPM2B" | be64 generation | be32 pub_len
 *      | be32 priv_len | pub | priv.  The generation stored here is a defence-in-depth
 *      software copy; the TPM's PolicyNV on the sealed object is the AUTHORITATIVE
 *      anti-rollback control.
 * The two magics are distinct 8-byte tags so blob_read discriminates the version
 * from the first 8 bytes alone (v1 -> refuse; v2 -> parse). */
static const uint8_t TPM2_BLOB_MAGIC_V1[8] = {'A', 'I', 'M', 'T', 'P', 'M', '2', '\0'};
static const uint8_t TPM2_BLOB_MAGIC_V2[8] = {'A', 'I', 'M', 'T', 'P', 'M', '2', 'B'};
#define TPM2_BLOB_HDR_LEN_V1 16
#define TPM2_BLOB_HDR_LEN_V2 24
#define TPM2_BLOB_MAX        8192
/* Sealed sensitive-data layout: KEK (VAULT_KEK_LEN) || be64 generation. */
#define TPM2_SEALED_LEN (VAULT_KEK_LEN + 8)

static const uint8_t TPM2_RESEAL_BUNDLE_MAGIC[8] = {'A', 'I', 'M', 'R', 'S', 'B', '1', '\0'};
#define TPM2_RESEAL_BUNDLE_HDR 176
#define TPM2_RESEAL_BUNDLE_MAX (TPM2_RESEAL_BUNDLE_HDR + 2 * TPM2_BLOB_MAX)

/* The NV index TYPE (counter) is encoded in the TPM2_NT sub-field of the TPMA_NV
 * attributes bitfield — there is no standalone TPMA_NV_COUNTER flag. */
#define TPM2_NV_COUNTER_ATTR ((TPMA_NV)TPM2_NT_COUNTER << TPMA_NV_TPM2_NT_SHIFT)

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
   ESYS_TR nv_handle;         /* resolved NV monotonic-counter handle; ESYS_TR_NONE until set */
   TPMI_RH_NV_INDEX nv_index; /* the NV index (config vault.tpm2.nv_index) */
   int nv_auth_ready;         /* 1 once nv_auth is derived + set on nv_handle this session */
   char blob_path[1024];
   char tcti_conf[256];
} tpm2_ctx_t;

static tpm2_ctx_t g_ctx = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .sealed = 1,
    .primary = ESYS_TR_NONE,
    .persistent_handle = TPM2_KEK_PERSIST_HANDLE,
    .nv_handle = ESYS_TR_NONE,
    .nv_index = 0,
};
static volatile sig_atomic_t g_tpm2_fork_child_invalid;
static pthread_once_t g_tpm2_atfork_once = PTHREAD_ONCE_INIT;
static int g_tpm2_atfork_status = -1;
static volatile sig_atomic_t g_reseal_lock_fd = -1;

static void tpm2_after_fork_child_impl(void *vctx)
{
   tpm2_ctx_t *ctx = vctx;
   g_tpm2_fork_child_invalid = 1;
   if (ctx)
   {
      volatile uint8_t *p = ctx->kek;
      for (size_t i = 0; i < sizeof(ctx->kek); i++)
         p[i] = 0;
      ctx->kek_ready = 0;
      ctx->sealed = 1;
      ctx->nv_auth_ready = 0;
   }
   if (g_reseal_lock_fd >= 0)
      (void)close(g_reseal_lock_fd);
   g_reseal_lock_fd = -1;
}

static void tpm2_after_fork_child(void)
{
   tpm2_after_fork_child_impl(&g_ctx);
}

static void register_tpm2_atfork_once(void)
{
   g_tpm2_atfork_status = pthread_atfork(NULL, NULL, tpm2_after_fork_child);
}

static int tpm2_process_ready(void)
{
   return !g_tpm2_fork_child_invalid &&
                  pthread_once(&g_tpm2_atfork_once, register_tpm2_atfork_once) == 0 &&
                  g_tpm2_atfork_status == 0
              ? 0
              : -1;
}

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

static void put_be64(uint8_t *p, uint64_t v)
{
   p[0] = (uint8_t)(v >> 56);
   p[1] = (uint8_t)(v >> 48);
   p[2] = (uint8_t)(v >> 40);
   p[3] = (uint8_t)(v >> 32);
   p[4] = (uint8_t)(v >> 24);
   p[5] = (uint8_t)(v >> 16);
   p[6] = (uint8_t)(v >> 8);
   p[7] = (uint8_t)v;
}

static uint64_t get_be64(const uint8_t *p)
{
   return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) | ((uint64_t)p[2] << 40) |
          ((uint64_t)p[3] << 32) | ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
          ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

/* ESAPI carries TPM response codes in the low 16 bits and software/TCTI layer
 * failures above them.  Strip the format-one handle/session selector before
 * comparing an authentication response; no provider prose is consulted. */
static uint32_t tpm2_base_rc(TSS2_RC rc)
{
   uint32_t code = (uint32_t)rc & 0xffffU;
   return code & ~0x0f00U;
}

static int tpm2_rc_is_auth(TSS2_RC rc)
{
   if (((uint32_t)rc & 0xffff0000U) != 0)
      return 0;
   uint32_t code = tpm2_base_rc(rc);
   return code == tpm2_base_rc(TPM2_RC_BAD_AUTH) || code == tpm2_base_rc(TPM2_RC_AUTH_FAIL);
}

static vault_custody_auth_result_t tpm2_command_failure(TSS2_RC rc,
                                                        vault_custody_auth_result_t tpm_failure)
{
   if (tpm2_rc_is_auth(rc))
      return VAULT_CUSTODY_AUTH_WRONG_SECRET;
   return ((uint32_t)rc & 0xffff0000U) != 0 ? VAULT_CUSTODY_AUTH_BACKEND_UNAVAILABLE : tpm_failure;
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

/* P7-tpm2b v2 sealed-data template: KEYEDHASH, scheme NULL (pure sealed data),
 * attrs = fixedTPM|fixedParent with userWithAuth CLEAR + an authPolicy = the
 * computed PolicyNV(NV==generation) THEN PolicyAuthValue digest. userWithAuth is
 * CLEAR so USER-role auth (i.e. Unseal) can ONLY be satisfied by a policy session
 * that (a) proves the NV counter equals THIS object's bound generation (TPM-enforced
 * anti-rollback — a stale object's PolicyNV fails at the TPM) and (b) folds in the
 * object authValue via PolicyAuthValue (the operator secret). NO sensitiveDataOrigin
 * (we supply KEK||gen), NO adminWithPolicy, noDA CLEAR (TPM dictionary-attack
 * protection stays ON). */
static TPM2B_PUBLIC seal_template_v2(const TPM2B_DIGEST *policy)
{
   TPM2B_PUBLIC t;
   memset(&t, 0, sizeof(t));
   t.size = 0;
   t.publicArea.type = TPM2_ALG_KEYEDHASH;
   t.publicArea.nameAlg = TPM2_ALG_SHA256;
   t.publicArea.objectAttributes = TPMA_OBJECT_FIXEDTPM | TPMA_OBJECT_FIXEDPARENT;
   t.publicArea.authPolicy.size = policy->size;
   memcpy(t.publicArea.authPolicy.buffer, policy->buffer, policy->size);
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

   /* Copied out: each is compared and then read again as the fallback arm of a
    * ternary, across other config reads. Accessors return declared defaults when
    * config cannot be read, which is what the old (void)legacy_config_read relied on. */
   char cfg_tcti[CONFIG_COPY_MAX];
   char cfg_blob_path[CONFIG_COPY_MAX];
   char cfg_nv_index[CONFIG_COPY_MAX];
   config_vault_tpm2_tcti_copy(cfg_tcti, sizeof(cfg_tcti));
   config_vault_tpm2_blob_path_copy(cfg_blob_path, sizeof(cfg_blob_path));
   config_vault_tpm2_nv_index_copy(cfg_nv_index, sizeof(cfg_nv_index));

   const char *env_tcti = getenv("AIMEE_VAULT_TPM2_TCTI");
   const char *tcti = (env_tcti && env_tcti[0]) ? env_tcti
                      : (cfg_tcti[0] != 0)      ? cfg_tcti
                                                : CONFIG_DEFAULT_VAULT_TPM2_TCTI;
   snprintf(ctx->tcti_conf, sizeof(ctx->tcti_conf), "%s", tcti);

   const char *env_blob = getenv("AIMEE_VAULT_TPM2_BLOB_PATH");
   if (env_blob && env_blob[0])
      snprintf(ctx->blob_path, sizeof(ctx->blob_path), "%s", env_blob);
   else if (cfg_blob_path[0])
      snprintf(ctx->blob_path, sizeof(ctx->blob_path), "%s", cfg_blob_path);
   else
   {
      const char *base = config_default_dir();
      if (!base || !base[0])
      {
         return -1;
      }
      snprintf(ctx->blob_path, sizeof(ctx->blob_path), "%s/vault/tpm2-kek.blob", base);
   }

   /* NV monotonic-counter index (P7-tpm2b): env AIMEE_VAULT_TPM2_NV_INDEX wins,
    * else vault.tpm2.nv_index, else the compiled default. Parsed base-0 so both hex
    * (0x01500001) and decimal round-trip; a bad/zero value fails closed. */
   const char *env_nv = getenv("AIMEE_VAULT_TPM2_NV_INDEX");
   const char *nvs = (env_nv && env_nv[0])    ? env_nv
                     : (cfg_nv_index[0] != 0) ? cfg_nv_index
                                              : CONFIG_DEFAULT_VAULT_TPM2_NV_INDEX;
   char *nv_end = NULL;
   unsigned long nv_val = strtoul(nvs, &nv_end, 0);
   if (!nv_end || *nv_end != '\0' || nv_val == 0 || nv_val > 0xFFFFFFFFUL)
   {
      return -1;
   }
   ctx->nv_index = (TPMI_RH_NV_INDEX)nv_val;

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

/* ── NV monotonic counter (P7-tpm2b anti-rollback authority) ─────────────────────
 *
 * ONE TPM2 NV index (config vault.tpm2.nv_index) defined as a TPMA_NV_COUNTER whose
 * authValue is a KDF of the operator secret (so ONLY the secret-holder can bump it,
 * not a bare TPM-owner-auth holder). Its live value is the current GENERATION; each
 * sealed blob binds a generation via PolicyNV, so restoring a stale blob is refused
 * BY THE TPM (its PolicyNV asserts NV == old-gen, now false). The counter is
 * monotonic + power-cycle-persistent — the trust anchor for "which generation is
 * current". */

/* VERIFY an existing NV index's public matches what we would define (collision-safe:
 * a foreign/squatting index at our handle -> fail closed, never used). The TPM sets
 * status bits (WRITTEN once activated, WRITE/READ-LOCKED) that are NOT part of the
 * defined shape, so mask them before comparing attributes. Caller holds ctx->mu.
 * 0 if it matches the expected public, -1 otherwise. */
static int nv_verify_public(tpm2_ctx_t *ctx, ESYS_TR nv)
{
   TPM2B_NV_PUBLIC *pub = NULL;
   TSS2_RC rc =
       Esys_NV_ReadPublic(ctx->esys, nv, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, &pub, NULL);
   if (rc != TSS2_RC_SUCCESS || !pub)
      return -1;
   const TPMA_NV want = TPM2_NV_COUNTER_ATTR | TPMA_NV_AUTHREAD | TPMA_NV_AUTHWRITE | TPMA_NV_NO_DA;
   TPMA_NV got = pub->nvPublic.attributes;
   got &= ~(TPMA_NV)(TPMA_NV_WRITTEN | TPMA_NV_WRITELOCKED | TPMA_NV_READLOCKED);
   int ok = (pub->nvPublic.nvIndex == ctx->nv_index && pub->nvPublic.nameAlg == TPM2_ALG_SHA256 &&
             pub->nvPublic.dataSize == 8 && pub->nvPublic.authPolicy.size == 0 && got == want)
                ? 0
                : -1;
   Esys_Free(pub);
   return ok;
}

static vault_custody_auth_result_t nv_authorize_existing_typed(tpm2_ctx_t *ctx, const void *secret,
                                                               size_t secret_len)
{
   TPM2B_AUTH auth;
   memset(&auth, 0, sizeof(auth));
   if (!ctx || (!secret && secret_len) || derive_authvalue(secret, secret_len, &auth) != 0)
      return VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;

   vault_custody_auth_result_t result = VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;
   if (ctx->nv_handle == ESYS_TR_NONE)
   {
      ESYS_TR nv = ESYS_TR_NONE;
      TSS2_RC trc = Esys_TR_FromTPMPublic(ctx->esys, ctx->nv_index, ESYS_TR_NONE, ESYS_TR_NONE,
                                          ESYS_TR_NONE, &nv);
      if (trc != TSS2_RC_SUCCESS)
      {
         result = tpm2_command_failure(trc, VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE);
         goto out;
      }
      ctx->nv_handle = nv;
   }

   TPM2B_NV_PUBLIC *pub = NULL;
   TSS2_RC trc = Esys_NV_ReadPublic(ctx->esys, ctx->nv_handle, ESYS_TR_NONE, ESYS_TR_NONE,
                                    ESYS_TR_NONE, &pub, NULL);
   if (trc != TSS2_RC_SUCCESS)
   {
      result = tpm2_command_failure(trc, VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE);
      Esys_Free(pub);
      goto out;
   }
   const TPMA_NV want = TPM2_NV_COUNTER_ATTR | TPMA_NV_AUTHREAD | TPMA_NV_AUTHWRITE | TPMA_NV_NO_DA;
   TPMA_NV got = pub->nvPublic.attributes;
   got &= ~(TPMA_NV)(TPMA_NV_WRITTEN | TPMA_NV_WRITELOCKED | TPMA_NV_READLOCKED);
   if (pub->nvPublic.nvIndex != ctx->nv_index || pub->nvPublic.nameAlg != TPM2_ALG_SHA256 ||
       pub->nvPublic.dataSize != 8 || pub->nvPublic.authPolicy.size != 0 || got != want)
   {
      Esys_Free(pub);
      goto out;
   }
   Esys_Free(pub);
   trc = Esys_TR_SetAuth(ctx->esys, ctx->nv_handle, &auth);
   if (trc != TSS2_RC_SUCCESS)
   {
      result = tpm2_command_failure(trc, VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE);
      goto out;
   }
   ctx->nv_auth_ready = 1;
   result = VAULT_CUSTODY_AUTHORIZED;
out:
   OPENSSL_cleanse(&auth, sizeof(auth));
   return result;
}

/* Ensure ctx->nv_handle is a VERIFIED handle to our NV monotonic counter with the
 * secret-derived authValue set on it (required for AUTHREAD/AUTHWRITE). If the index
 * is absent and allow_create is set (provision/reseal), DefineSpace it under owner
 * auth with the derived authValue, then do the FIRST Esys_NV_Increment to ACTIVATE
 * the freshly-defined counter (inactive until first increment — NV_Read fails
 * otherwise). If the index exists, verify its public (foreign shape -> fail closed).
 * secret/secret_len is the operator credential. Caller holds ctx->mu. 0/-1. */
static int nv_ensure(tpm2_ctx_t *ctx, const void *secret, size_t secret_len, int allow_create)
{
   TPM2B_AUTH nvauth;
   memset(&nvauth, 0, sizeof(nvauth));
   if (derive_authvalue(secret, secret_len, &nvauth) != 0)
      return -1;

   int rc = -1;
   if (ctx->nv_handle == ESYS_TR_NONE)
   {
      ESYS_TR nv = ESYS_TR_NONE;
      TSS2_RC trc = Esys_TR_FromTPMPublic(ctx->esys, ctx->nv_index, ESYS_TR_NONE, ESYS_TR_NONE,
                                          ESYS_TR_NONE, &nv);
      if (trc == TSS2_RC_SUCCESS)
      {
         if (nv_verify_public(ctx, nv) != 0) /* squatting/foreign index -> fail closed */
         {
            Esys_TR_Close(ctx->esys, &nv);
            goto out;
         }
      }
      else
      {
         if (!allow_create)
            goto out;
         TPM2B_NV_PUBLIC pubinfo;
         memset(&pubinfo, 0, sizeof(pubinfo));
         pubinfo.size = 0; /* ESAPI recomputes the marshaled size */
         pubinfo.nvPublic.nvIndex = ctx->nv_index;
         pubinfo.nvPublic.nameAlg = TPM2_ALG_SHA256;
         pubinfo.nvPublic.attributes =
             TPM2_NV_COUNTER_ATTR | TPMA_NV_AUTHREAD | TPMA_NV_AUTHWRITE | TPMA_NV_NO_DA;
         pubinfo.nvPublic.authPolicy.size = 0;
         pubinfo.nvPublic.dataSize = 8;
         trc = Esys_NV_DefineSpace(ctx->esys, ESYS_TR_RH_OWNER, ESYS_TR_PASSWORD, ESYS_TR_NONE,
                                   ESYS_TR_NONE, &nvauth, &pubinfo, &nv);
         if (trc != TSS2_RC_SUCCESS)
            goto out;
         /* Set the just-defined authValue on the handle, then ACTIVATE the counter
          * with its first increment (auth = the NV authValue via ESYS_TR_PASSWORD). */
         trc = Esys_TR_SetAuth(ctx->esys, nv, &nvauth);
         if (trc != TSS2_RC_SUCCESS)
         {
            Esys_TR_Close(ctx->esys, &nv);
            goto out;
         }
         trc = Esys_NV_Increment(ctx->esys, nv, nv, ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE);
         if (trc != TSS2_RC_SUCCESS)
         {
            Esys_TR_Close(ctx->esys, &nv);
            goto out;
         }
      }
      ctx->nv_handle = nv;
   }

   /* (Re)assert the secret-derived authValue on the resolved handle so subsequent
    * NV_Read/NV_Increment/PolicyNV (AUTHREAD/AUTHWRITE) authorize. */
   if (Esys_TR_SetAuth(ctx->esys, ctx->nv_handle, &nvauth) != TSS2_RC_SUCCESS)
      goto out;
   ctx->nv_auth_ready = 1;
   rc = 0;

out:
   OPENSSL_cleanse(&nvauth, sizeof(nvauth));
   return rc;
}

/* Read the 8-byte NV counter (AUTHREAD, auth = the NV authValue set by nv_ensure).
 * The TPM stores/returns the counter big-endian. Caller holds ctx->mu. 0/-1. */
static int nv_read(tpm2_ctx_t *ctx, uint64_t *out_gen)
{
   if (ctx->nv_handle == ESYS_TR_NONE || !ctx->nv_auth_ready)
      return -1;
   TPM2B_MAX_NV_BUFFER *data = NULL;
   TSS2_RC rc = Esys_NV_Read(ctx->esys, ctx->nv_handle, ctx->nv_handle, ESYS_TR_PASSWORD,
                             ESYS_TR_NONE, ESYS_TR_NONE, 8, 0, &data);
   if (rc != TSS2_RC_SUCCESS || !data || data->size != 8)
   {
      Esys_Free(data);
      return -1;
   }
   *out_gen = get_be64(data->buffer);
   Esys_Free(data);
   return 0;
}

static vault_custody_auth_result_t nv_read_typed(tpm2_ctx_t *ctx, uint64_t *out_gen)
{
   if (!ctx || !out_gen || ctx->nv_handle == ESYS_TR_NONE || !ctx->nv_auth_ready)
      return VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;
   *out_gen = 0;
   TPM2B_MAX_NV_BUFFER *data = NULL;
   TSS2_RC rc = Esys_NV_Read(ctx->esys, ctx->nv_handle, ctx->nv_handle, ESYS_TR_PASSWORD,
                             ESYS_TR_NONE, ESYS_TR_NONE, 8, 0, &data);
   if (rc != TSS2_RC_SUCCESS)
   {
      Esys_Free(data);
      return tpm2_command_failure(rc, VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE);
   }
   if (!data || data->size != 8)
   {
      Esys_Free(data);
      return VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;
   }
   *out_gen = get_be64(data->buffer);
   Esys_Free(data);
   return VAULT_CUSTODY_AUTHORIZED;
}

/* Increment the NV counter -> a new generation (AUTHWRITE, auth = the NV authValue).
 * The TPM enforces monotonicity: the value can ONLY increase, never decrement, even
 * across power cycles. Caller holds ctx->mu. 0/-1. */
static int nv_increment(tpm2_ctx_t *ctx)
{
   if (ctx->nv_handle == ESYS_TR_NONE || !ctx->nv_auth_ready)
      return -1;
   TSS2_RC rc = Esys_NV_Increment(ctx->esys, ctx->nv_handle, ctx->nv_handle, ESYS_TR_PASSWORD,
                                  ESYS_TR_NONE, ESYS_TR_NONE);
   return (rc == TSS2_RC_SUCCESS) ? 0 : -1;
}

/* Compute the authPolicy digest a generation-bound sealed object must carry:
 * PolicyNV(nvIndex, operandB = be64(generation), offset 0, TPM2_EO_EQ) THEN
 * PolicyAuthValue — via a TRIAL policy session. Because operandB (the generation) is
 * folded into the digest, EACH generation yields a DISTINCT policy bound to
 * "NV counter == this generation AND the object authValue". The trial session needs
 * the nv_handle (its Name is hashed into the digest) with its AUTHREAD auth set
 * (nv_ensure did so); the resulting digest is auth-method-independent so it matches
 * what the real (policy-session) evaluation computes at unseal. Caller holds ctx->mu.
 * 0/-1. Flushes the trial session on every path. */
static int compute_seal_policy(tpm2_ctx_t *ctx, uint64_t generation, TPM2B_DIGEST *out_policy)
{
   if (ctx->nv_handle == ESYS_TR_NONE || !ctx->nv_auth_ready || !out_policy)
      return -1;

   int rc = -1;
   ESYS_TR trial = ESYS_TR_NONE;
   TPM2B_DIGEST *digest = NULL;

   TPMT_SYM_DEF sym;
   memset(&sym, 0, sizeof(sym));
   sym.algorithm = TPM2_ALG_NULL; /* trial session needs no parameter encryption */

   TSS2_RC trc =
       Esys_StartAuthSession(ctx->esys, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
                             ESYS_TR_NONE, NULL, TPM2_SE_TRIAL, &sym, TPM2_ALG_SHA256, &trial);
   if (trc != TSS2_RC_SUCCESS)
      goto out;

   TPM2B_OPERAND operand;
   memset(&operand, 0, sizeof(operand));
   operand.size = 8;
   put_be64(operand.buffer, generation);

   /* authHandle = nvIndex (AUTHREAD via ESYS_TR_PASSWORD using the NV authValue). */
   trc = Esys_PolicyNV(ctx->esys, ctx->nv_handle, ctx->nv_handle, trial, ESYS_TR_PASSWORD,
                       ESYS_TR_NONE, ESYS_TR_NONE, &operand, 0, TPM2_EO_EQ);
   if (trc != TSS2_RC_SUCCESS)
      goto out;
   trc = Esys_PolicyAuthValue(ctx->esys, trial, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE);
   if (trc != TSS2_RC_SUCCESS)
      goto out;
   trc = Esys_PolicyGetDigest(ctx->esys, trial, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, &digest);
   if (trc != TSS2_RC_SUCCESS || !digest || digest->size > sizeof(out_policy->buffer))
      goto out;
   out_policy->size = digest->size;
   memcpy(out_policy->buffer, digest->buffer, digest->size);
   rc = 0;

out:
   Esys_Free(digest);
   if (trial != ESYS_TR_NONE)
      Esys_FlushContext(ctx->esys, trial);
   return rc;
}

static int parent_path(const char *path, char out[1024])
{
   if (!path || (size_t)snprintf(out, 1024, "%s", path) >= 1024)
      return -1;
   char *slash = strrchr(out, '/');
   if (!slash)
      snprintf(out, 1024, ".");
   else if (slash == out)
      slash[1] = '\0';
   else
      *slash = '\0';
   return 0;
}

static int validate_parent(const char *path)
{
   char dir[1024];
   struct stat st;
   if (parent_path(path, dir) != 0 || lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) ||
       st.st_uid != geteuid() || (st.st_mode & 0022) != 0)
      return -1;
   return 0;
}

static int fsync_parent(const char *path)
{
   char dir[1024];
   if (parent_path(path, dir) != 0 || validate_parent(path) != 0)
      return -1;
   int fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   if (fd >= 0)
   {
      int rc = fsync(fd);
      close(fd);
      return rc == 0 ? 0 : -1;
   }
   return -1;
}

static int raw_read(const char *path, uint8_t *buf, size_t cap, size_t *out_len)
{
   if (validate_parent(path) != 0)
      return -1;
   int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
   if (fd < 0)
      return -1;
   struct stat st;
   if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 ||
       (st.st_mode & 0777) != 0600 || st.st_size <= 0 || (uint64_t)st.st_size > cap)
   {
      close(fd);
      return -1;
   }
   size_t want = (size_t)st.st_size, got = 0;
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
   *out_len = got;
   return 0;
}

static int raw_write_atomic(const char *path, const uint8_t *buf, size_t len, int replace)
{
   char dir[1024], tmp[1152];
   if (!path || !buf || !len || (size_t)snprintf(dir, sizeof(dir), "%s", path) >= sizeof(dir))
      return -1;
   char *slash = strrchr(dir, '/');
   if (slash && slash != dir)
   {
      *slash = '\0';
      if (platform_mkdir_p(dir, 0700) != 0)
         return -1;
   }
   if (validate_parent(path) != 0)
      return -1;
   if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid()) >= sizeof(tmp))
      return -1;
   int fd = open(tmp, O_CREAT | O_WRONLY | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
   if (fd < 0)
      return -1;
   size_t off = 0;
   while (off < len)
   {
      ssize_t n = write(fd, buf + off, len - off);
      if (n <= 0)
         break;
      off += (size_t)n;
   }
   int rc = (off == len && fsync(fd) == 0) ? 0 : -1;
   close(fd);
   if (rc == 0)
   {
      if (replace)
         rc = rename(tmp, path) == 0 ? 0 : -1;
      else
#ifdef SYS_renameat2
         rc = syscall(SYS_renameat2, AT_FDCWD, tmp, AT_FDCWD, path, 1 /* RENAME_NOREPLACE */) == 0
                  ? 0
                  : -1;
#else
         rc = -1; /* fail closed: link/unlink has an unrecoverable two-link crash window */
#endif
   }
   if (rc != 0)
      (void)unlink(tmp);
   else if (fsync_parent(path) != 0)
      rc = -1;
   return rc;
}

/* Atomically persist the v2 sealed blob (magic "AIMTPM2B" | be64 generation | be32
 * pub_len | be32 priv_len | pub | priv) via tmp + O_EXCL|O_NOFOLLOW 0600 + fsync +
 * rename + parent fsync. The generation is a defence-in-depth software copy; the
 * TPM's PolicyNV is authoritative. 0/-1. */
static int blob_write(const char *path, uint64_t generation, const TPM2B_PUBLIC *pub,
                      const TPM2B_PRIVATE *priv)
{
   uint8_t buf[TPM2_BLOB_MAX];
   size_t pub_off = 0, priv_off = 0;
   memcpy(buf, TPM2_BLOB_MAGIC_V2, sizeof(TPM2_BLOB_MAGIC_V2));
   put_be64(buf + 8, generation);
   if (Tss2_MU_TPM2B_PUBLIC_Marshal(pub, buf + TPM2_BLOB_HDR_LEN_V2,
                                    sizeof(buf) - TPM2_BLOB_HDR_LEN_V2,
                                    &pub_off) != TSS2_RC_SUCCESS)
      return -1;
   if (Tss2_MU_TPM2B_PRIVATE_Marshal(priv, buf + TPM2_BLOB_HDR_LEN_V2 + pub_off,
                                     sizeof(buf) - TPM2_BLOB_HDR_LEN_V2 - pub_off,
                                     &priv_off) != TSS2_RC_SUCCESS)
      return -1;
   put_be32(buf + 16, (uint32_t)pub_off);
   put_be32(buf + 20, (uint32_t)priv_off);
   size_t total = TPM2_BLOB_HDR_LEN_V2 + pub_off + priv_off;

   return raw_write_atomic(path, buf, total, 1);
}

/* Load + defensively unmarshal the sealed blob (bounded read; reject truncated/
 * oversized/bad-magic). Version-aware:
 *   - a v1 (tpm2a "AIMTPM2\0") blob -> *out_version = 1 and RETURN 0 WITHOUT
 *     unmarshaling (the caller REFUSES it: v1 has no PolicyNV binding = rollback hole);
 *   - a v2 ("AIMTPM2B") blob -> *out_version = 2, *out_gen = the bound generation,
 *     pub/priv unmarshaled.
 * Returns 0 if the blob was RECOGNIZED (v1 or v2; caller inspects *out_version), -1
 * on any read/format/unmarshal failure. */
static int blob_read(const char *path, int *out_version, uint64_t *out_gen, TPM2B_PUBLIC *pub,
                     TPM2B_PRIVATE *priv)
{
   int fd = open(path, O_RDONLY | O_NOFOLLOW);
   if (fd < 0)
      return -1;
   struct stat st;
   if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 8 || st.st_size > TPM2_BLOB_MAX)
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

   /* A v1 (tpm2a) blob is RECOGNIZED but not parsed — the caller fails it closed. */
   if (memcmp(buf, TPM2_BLOB_MAGIC_V1, sizeof(TPM2_BLOB_MAGIC_V1)) == 0)
   {
      *out_version = 1;
      *out_gen = 0;
      return 0;
   }
   if (memcmp(buf, TPM2_BLOB_MAGIC_V2, sizeof(TPM2_BLOB_MAGIC_V2)) != 0)
      return -1;
   if (want < TPM2_BLOB_HDR_LEN_V2)
      return -1;
   uint64_t generation = get_be64(buf + 8);
   uint32_t pub_len = get_be32(buf + 16);
   uint32_t priv_len = get_be32(buf + 20);
   /* Bounds: header + pub + priv must exactly cover the file (no slack/overflow). */
   if ((uint64_t)TPM2_BLOB_HDR_LEN_V2 + pub_len + priv_len != (uint64_t)want)
      return -1;

   size_t off = 0;
   memset(pub, 0, sizeof(*pub));
   memset(priv, 0, sizeof(*priv));
   if (Tss2_MU_TPM2B_PUBLIC_Unmarshal(buf + TPM2_BLOB_HDR_LEN_V2, pub_len, &off, pub) !=
           TSS2_RC_SUCCESS ||
       off != pub_len)
      return -1;
   off = 0;
   if (Tss2_MU_TPM2B_PRIVATE_Unmarshal(buf + TPM2_BLOB_HDR_LEN_V2 + pub_len, priv_len, &off,
                                       priv) != TSS2_RC_SUCCESS ||
       off != priv_len)
      return -1;
   *out_version = 2;
   *out_gen = generation;
   return 0;
}

/* Read-only D3b authorization/PolicyNV preflight.  Caller holds ctx->mu.  This
 * deliberately re-reads the primary Name and NV public area even when ESAPI
 * handles are cached: a successful earlier lookup is not authority for a later
 * operator mutation. */
static vault_custody_auth_result_t tpm2_authorization_preflight_current_locked(tpm2_ctx_t *ctx,
                                                                               const void *secret,
                                                                               size_t secret_len,
                                                                               uint64_t *generation)
{
   if (generation)
      *generation = 0;
   if (!ctx || !generation || !secret || secret_len == 0 ||
       secret_len > VAULT_CUSTODY_AUTH_SECRET_MAX)
      return VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;
   if (ensure_ready(ctx) != 0)
      return VAULT_CUSTODY_AUTH_BACKEND_UNAVAILABLE;
   if (ensure_primary(ctx, 0) != 0 || verify_primary_name(ctx, ctx->primary) != 0)
      return VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;

   int version = 0;
   uint64_t bound_generation = 0;
   TPM2B_PUBLIC pub;
   TPM2B_PRIVATE priv;
   TPM2B_DIGEST policy;
   memset(&pub, 0, sizeof(pub));
   memset(&priv, 0, sizeof(priv));
   memset(&policy, 0, sizeof(policy));
   vault_custody_auth_result_t result = VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;

   if (blob_read(ctx->blob_path, &version, &bound_generation, &pub, &priv) != 0 || version != 2 ||
       bound_generation == 0 || bound_generation > INT64_MAX)
      goto out;
   result = nv_authorize_existing_typed(ctx, secret, secret_len);
   if (result != VAULT_CUSTODY_AUTHORIZED)
      goto out;

   uint64_t live_generation = 0;
   result = nv_read_typed(ctx, &live_generation);
   if (result != VAULT_CUSTODY_AUTHORIZED)
      goto out;
   if (live_generation != bound_generation)
   {
      result = VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;
      goto out;
   }

   /* A trial PolicyNV digest is a pure description of the live policy.  Exact
    * comparison with the active public object proves the blob is bound to this
    * canonical NV Name, generation, and PolicyAuthValue sequence. */
   if (compute_seal_policy(ctx, live_generation, &policy) != 0)
   {
      result = VAULT_CUSTODY_AUTH_BACKEND_UNAVAILABLE;
      goto out;
   }
   TPM2B_PUBLIC expected = seal_template_v2(&policy);
   if (pub.publicArea.type != expected.publicArea.type ||
       pub.publicArea.nameAlg != expected.publicArea.nameAlg ||
       pub.publicArea.objectAttributes != expected.publicArea.objectAttributes ||
       pub.publicArea.parameters.keyedHashDetail.scheme.scheme != TPM2_ALG_NULL ||
       pub.publicArea.unique.keyedHash.size != 0 ||
       pub.publicArea.authPolicy.size != expected.publicArea.authPolicy.size ||
       CRYPTO_memcmp(pub.publicArea.authPolicy.buffer, expected.publicArea.authPolicy.buffer,
                     expected.publicArea.authPolicy.size) != 0)
   {
      result = VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;
      goto out;
   }
   result = VAULT_CUSTODY_AUTHORIZED;
   *generation = live_generation;

out:
   if (result != VAULT_CUSTODY_AUTHORIZED)
      *generation = 0;
   OPENSSL_cleanse(&policy, sizeof(policy));
   OPENSSL_cleanse(&priv, sizeof(priv));
   OPENSSL_cleanse(&pub, sizeof(pub));
   return result;
}

static vault_custody_auth_result_t tpm2_authorization_preflight_locked(tpm2_ctx_t *ctx,
                                                                       const void *secret,
                                                                       size_t secret_len,
                                                                       uint64_t expected_generation)
{
   uint64_t generation = 0;
   vault_custody_auth_result_t result =
       tpm2_authorization_preflight_current_locked(ctx, secret, secret_len, &generation);
   if (result != VAULT_CUSTODY_AUTHORIZED)
      return result;
   return generation == expected_generation ? VAULT_CUSTODY_AUTHORIZED
                                            : VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;
}

static int tpm2_authorization_preflight(void *vctx, const void *secret, size_t secret_len,
                                        uint64_t expected_generation)
{
   tpm2_ctx_t *ctx = vctx;
   if (tpm2_process_ready() != 0 || !ctx)
      return VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;
   pthread_mutex_lock(&ctx->mu);
   vault_custody_auth_result_t result =
       tpm2_authorization_preflight_locked(ctx, secret, secret_len, expected_generation);
   pthread_mutex_unlock(&ctx->mu);
   return result;
}

static int tpm2_authorization_preflight_current(void *vctx, const void *secret, size_t secret_len,
                                                uint64_t *generation)
{
   if (generation)
      *generation = 0;
   tpm2_ctx_t *ctx = vctx;
   if (tpm2_process_ready() != 0 || !ctx || !generation)
      return VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;
   pthread_mutex_lock(&ctx->mu);
   vault_custody_auth_result_t result =
       tpm2_authorization_preflight_current_locked(ctx, secret, secret_len, generation);
   pthread_mutex_unlock(&ctx->mu);
   return result;
}

/* Unmarshal one in-memory v2 blob. Prepared recovery uses this directly on the
 * capsule embedded in the canonical bundle: it must never publish the capsule
 * at the active-blob path merely to reuse blob_read(). */
static int blob_bytes_unmarshal_v2(const uint8_t *buf, size_t len, uint64_t *out_gen,
                                   TPM2B_PUBLIC *pub, TPM2B_PRIVATE *priv)
{
   if (out_gen)
      *out_gen = 0;
   if (pub)
      memset(pub, 0, sizeof(*pub));
   if (priv)
      memset(priv, 0, sizeof(*priv));
   if (!buf || !out_gen || !pub || !priv || len < TPM2_BLOB_HDR_LEN_V2 || len > TPM2_BLOB_MAX ||
       memcmp(buf, TPM2_BLOB_MAGIC_V2, 8) != 0)
      return -1;
   uint32_t pub_len = get_be32(buf + 16), priv_len = get_be32(buf + 20);
   if ((uint64_t)TPM2_BLOB_HDR_LEN_V2 + pub_len + priv_len != len)
      return -1;
   size_t off = 0;
   if (Tss2_MU_TPM2B_PUBLIC_Unmarshal(buf + TPM2_BLOB_HDR_LEN_V2, pub_len, &off, pub) !=
           TSS2_RC_SUCCESS ||
       off != pub_len)
      goto fail;
   off = 0;
   if (Tss2_MU_TPM2B_PRIVATE_Unmarshal(buf + TPM2_BLOB_HDR_LEN_V2 + pub_len, priv_len, &off,
                                       priv) != TSS2_RC_SUCCESS ||
       off != priv_len)
      goto fail;
   *out_gen = get_be64(buf + 8);
   return 0;
fail:
   OPENSSL_cleanse(pub, sizeof(*pub));
   OPENSSL_cleanse(priv, sizeof(*priv));
   return -1;
}

static int blob_bytes_validate(tpm2_ctx_t *ctx, const uint8_t *buf, size_t len,
                               uint64_t expected_generation)
{
   if (!ctx)
      return -1;
   TPM2B_PUBLIC pub;
   TPM2B_PRIVATE priv;
   uint64_t generation = 0;
   if (blob_bytes_unmarshal_v2(buf, len, &generation, &pub, &priv) != 0)
      return -1;
   if (generation != expected_generation)
   {
      OPENSSL_cleanse(&priv, sizeof(priv));
      OPENSSL_cleanse(&pub, sizeof(pub));
      return -1;
   }
   TPM2B_DIGEST policy;
   memset(&policy, 0, sizeof(policy));
   int rc = compute_seal_policy(ctx, expected_generation, &policy);
   TPM2B_PUBLIC expected = seal_template_v2(&policy);
   if (rc != 0 || pub.publicArea.type != expected.publicArea.type ||
       pub.publicArea.nameAlg != expected.publicArea.nameAlg ||
       pub.publicArea.objectAttributes != expected.publicArea.objectAttributes ||
       pub.publicArea.authPolicy.size != expected.publicArea.authPolicy.size ||
       memcmp(pub.publicArea.authPolicy.buffer, expected.publicArea.authPolicy.buffer,
              expected.publicArea.authPolicy.size) != 0 ||
       pub.publicArea.parameters.keyedHashDetail.scheme.scheme != TPM2_ALG_NULL)
      rc = -1;
   OPENSSL_cleanse(&priv, sizeof(priv));
   OPENSSL_cleanse(&pub, sizeof(pub));
   OPENSSL_cleanse(&policy, sizeof(policy));
   return rc;
}

/* Read one active blob snapshot, validate its complete v2 encoding and freshly
 * recomputed policy for the expected generation, then hash those same bytes. */
static int blob_file_digest_validate(tpm2_ctx_t *ctx, const char *path,
                                     uint64_t expected_generation, uint8_t digest[32])
{
   uint8_t buf[TPM2_BLOB_MAX];
   size_t len = 0;
   if (raw_read(path, buf, sizeof(buf), &len) != 0 ||
       blob_bytes_validate(ctx, buf, len, expected_generation) != 0)
   {
      OPENSSL_cleanse(buf, sizeof(buf));
      return -1;
   }
   SHA256(buf, len, digest);
   OPENSSL_cleanse(buf, sizeof(buf));
   return 0;
}

/* ── vtable ops ──────────────────────────────────────────────────────────────── */

static int tpm2_get_kek(void *vctx, uint8_t kek[VAULT_KEK_LEN])
{
   tpm2_ctx_t *ctx = vctx;
   if (tpm2_process_ready() != 0 || !kek || !ctx)
   {
      if (kek)
         OPENSSL_cleanse(kek, VAULT_KEK_LEN);
      return -1;
   }
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
   if (tpm2_process_ready() != 0)
      return -1;
   (void)vctx;
   (void)server_principal;
   (void)out_principals;
   (void)out_creds;
   (void)backup_path;
   (void)backup_path_len;
   tpm2_set_err(errbuf, errlen,
                "tpm2 rotate must go through the kb vault-rotate flow which re-wraps DEKs then "
                "calls vault_custody_tpm2_reseal; a standalone custody KEK rotation would strand "
                "DEKs");
   return -1;
}

static int tpm2_is_sealed(void *vctx)
{
   tpm2_ctx_t *ctx = vctx;
   if (tpm2_process_ready() != 0 || !ctx)
      return 1;
   pthread_mutex_lock(&ctx->mu);
   int s = ctx->sealed;
   pthread_mutex_unlock(&ctx->mu);
   return s;
}

/* D3a status is deliberately local-only: it does not initialize TCTI/ESYS,
 * inspect the blob, read NV, or otherwise touch the TPM. */
static int tpm2_local_status(void *vctx, unsigned timeout_ms)
{
   tpm2_ctx_t *ctx = vctx;
   if (tpm2_process_ready() != 0 || !ctx || timeout_ms == 0)
      return VAULT_CUSTODY_LOCAL_UNAVAILABLE;

   struct timespec start, now, pause = {.tv_nsec = 1000L * 1000L};
   if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
      return VAULT_CUSTODY_LOCAL_UNAVAILABLE;
   for (;;)
   {
      int lock_rc = pthread_mutex_trylock(&ctx->mu);
      if (lock_rc == 0)
         break;
      if (lock_rc != EBUSY || clock_gettime(CLOCK_MONOTONIC, &now) != 0)
         return VAULT_CUSTODY_LOCAL_UNAVAILABLE;
      uint64_t elapsed_ms = (uint64_t)(now.tv_sec - start.tv_sec) * 1000U;
      if (now.tv_nsec >= start.tv_nsec)
         elapsed_ms += (uint64_t)(now.tv_nsec - start.tv_nsec) / 1000000U;
      else
         elapsed_ms -= (uint64_t)(start.tv_nsec - now.tv_nsec) / 1000000U;
      if (elapsed_ms >= timeout_ms)
         return VAULT_CUSTODY_LOCAL_UNAVAILABLE;
      (void)nanosleep(&pause, NULL);
   }

   int result;
   if ((ctx->sealed != 0 && ctx->sealed != 1) || (ctx->kek_ready != 0 && ctx->kek_ready != 1) ||
       ctx->sealed == ctx->kek_ready)
      result = VAULT_CUSTODY_LOCAL_MALFORMED;
   else
      result = ctx->sealed ? VAULT_CUSTODY_LOCAL_AVAILABLE_SEALED
                           : VAULT_CUSTODY_LOCAL_AVAILABLE_UNSEALED;
   pthread_mutex_unlock(&ctx->mu);
   return result;
}

/* Unseal one already-parsed PolicyNV-bound object while ctx->mu is held. The
 * plaintext is published only after the TPM policy and the embedded/live
 * generation checks all pass. This primitive deliberately does not touch the
 * provider cache or sealed flag, so prepared-capsule recovery can remain sealed. */
static int unseal_loaded_locked(tpm2_ctx_t *ctx, TPM2B_PUBLIC *pub, TPM2B_PRIVATE *priv,
                                uint64_t bound_gen, const void *params, size_t len,
                                uint8_t out_kek[VAULT_KEK_LEN],
                                vault_custody_auth_result_t *typed_result)
{
   if (typed_result)
      *typed_result = VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;
   if (out_kek)
      OPENSSL_cleanse(out_kek, VAULT_KEK_LEN);
   if (!ctx || !pub || !priv || !out_kek || (!params && len))
      return -1;
   int rc = -1;
   ESYS_TR sealed = ESYS_TR_NONE;
   ESYS_TR session = ESYS_TR_NONE;
   TPM2B_SENSITIVE_DATA *out_data = NULL;
   TPM2B_AUTH auth;
   memset(&auth, 0, sizeof(auth));
   /* Resolve the NV counter (must already exist) + set its secret-derived auth so the
    * policy-session PolicyNV can read it. NO create on the unseal path. */
   if (nv_ensure(ctx, params, len, 0) != 0)
   {
      if (typed_result)
         *typed_result = VAULT_CUSTODY_AUTH_BACKEND_UNAVAILABLE;
      goto out;
   }

   TSS2_RC trc = Esys_Load(ctx->esys, ctx->primary, ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE,
                           priv, pub, &sealed);
   if (trc != TSS2_RC_SUCCESS)
   {
      if (typed_result)
         *typed_result = tpm2_command_failure(trc, VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE);
      goto out;
   }

   if (derive_authvalue(params, len, &auth) != 0)
      goto out;
   trc = Esys_TR_SetAuth(ctx->esys, sealed, &auth); /* auth folded via PolicyAuthValue */
   if (trc != TSS2_RC_SUCCESS)
      goto out;

   /* POLICY session, salted (tpmKey = our primary) + response-parameter-ENCRYPTED so
    * the unsealed KEK never crosses the TCTI transport in plaintext. */
   TPMT_SYM_DEF sym;
   memset(&sym, 0, sizeof(sym));
   sym.algorithm = TPM2_ALG_AES;
   sym.keyBits.aes = 128;
   sym.mode.aes = TPM2_ALG_CFB;
   trc = Esys_StartAuthSession(ctx->esys, ctx->primary, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
                               ESYS_TR_NONE, NULL, TPM2_SE_POLICY, &sym, TPM2_ALG_SHA256, &session);
   if (trc != TSS2_RC_SUCCESS)
   {
      if (typed_result)
         *typed_result = tpm2_command_failure(trc, VAULT_CUSTODY_AUTH_BACKEND_UNAVAILABLE);
      goto out;
   }

   /* PolicyNV: the TPM asserts NV counter == the blob's bound generation. A STALE
    * blob (bound_gen < live NV after a reseal) FAILS HERE, at the TPM. operandB is
    * the same be64(gen) that was folded into the object's authPolicy at seal time. */
   TPM2B_OPERAND operand;
   memset(&operand, 0, sizeof(operand));
   operand.size = 8;
   put_be64(operand.buffer, bound_gen);
   trc = Esys_PolicyNV(ctx->esys, ctx->nv_handle, ctx->nv_handle, session, ESYS_TR_PASSWORD,
                       ESYS_TR_NONE, ESYS_TR_NONE, &operand, 0, TPM2_EO_EQ);
   if (trc != TSS2_RC_SUCCESS)
   {
      if (typed_result)
         *typed_result = tpm2_command_failure(trc, VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE);
      goto out;
   }
   trc = Esys_PolicyAuthValue(ctx->esys, session, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE);
   if (trc != TSS2_RC_SUCCESS)
   {
      if (typed_result)
         *typed_result = tpm2_command_failure(trc, VAULT_CUSTODY_AUTH_BACKEND_UNAVAILABLE);
      goto out;
   }

   /* CONTINUESESSION so the TPM does NOT auto-flush the policy session after Unseal —
    * we own its FlushContext below; ENCRYPT so the unsealed KEK response is encrypted. */
   trc = Esys_TRSess_SetAttributes(ctx->esys, session,
                                   TPMA_SESSION_CONTINUESESSION | TPMA_SESSION_ENCRYPT,
                                   TPMA_SESSION_CONTINUESESSION | TPMA_SESSION_ENCRYPT);
   if (trc != TSS2_RC_SUCCESS)
   {
      if (typed_result)
         *typed_result = tpm2_command_failure(trc, VAULT_CUSTODY_AUTH_BACKEND_UNAVAILABLE);
      goto out;
   }

   trc = Esys_Unseal(ctx->esys, sealed, session, ESYS_TR_NONE, ESYS_TR_NONE, &out_data);
   if (trc != TSS2_RC_SUCCESS)
   {
      if (typed_result)
         *typed_result = tpm2_command_failure(trc, VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE);
      goto out;
   }
   if (!out_data || out_data->size != TPM2_SEALED_LEN)
      goto out;

   /* Defence-in-depth (PolicyNV is the load-bearing control): the sealed data's
    * embedded generation must equal the blob's bound generation AND the live NV
    * counter. A mismatch here would only arise if PolicyNV were somehow bypassed. */
   {
      uint64_t sealed_gen = get_be64(out_data->buffer + VAULT_KEK_LEN);
      uint64_t live_gen = 0;
      if (sealed_gen != bound_gen)
         goto out;
      vault_custody_auth_result_t read_result = nv_read_typed(ctx, &live_gen);
      if (read_result != VAULT_CUSTODY_AUTHORIZED)
      {
         if (typed_result)
            *typed_result = read_result;
         goto out;
      }
      if (live_gen != bound_gen)
         goto out;
   }

   memcpy(out_kek, out_data->buffer, VAULT_KEK_LEN);
   if (typed_result)
      *typed_result = VAULT_CUSTODY_AUTHORIZED;
   rc = 0;

out:
   if (out_data)
   {
      OPENSSL_cleanse(out_data->buffer, sizeof(out_data->buffer));
      Esys_Free(out_data);
   }
   OPENSSL_cleanse(&auth, sizeof(auth));
   if (session != ESYS_TR_NONE)
      Esys_FlushContext(ctx->esys, session);
   if (sealed != ESYS_TR_NONE)
      Esys_FlushContext(ctx->esys, sealed);
   if (rc != 0)
      OPENSSL_cleanse(out_kek, VAULT_KEK_LEN);
   return rc;
}

/* Unseal (P7-tpm2b, TPM-ENFORCED anti-rollback). Load the v2 blob under the verified
 * primary, then unseal it over a POLICY session whose policyDigest must match the
 * object's authPolicy = PolicyNV(NV == the blob's bound generation) THEN
 * PolicyAuthValue(the operator secret). A stale blob (bound to an old generation after
 * a reseal bumped the NV counter) fails at the PolicyNV step INSIDE THE TPM — the TPM
 * itself refuses to unseal, not our software. The session is salted (to the primary)
 * + response-ENCRYPTED so the recovered KEK is transport-encrypted TPM->caller. A v1
 * (tpm2a, generation-less) blob is REFUSED (no PolicyNV binding). A post-unseal
 * software gen==NV check is cheap defence-in-depth. Any failure -> stays sealed, -1. */
static int tpm2_unseal(void *vctx, const void *params, size_t len)
{
   tpm2_ctx_t *ctx = vctx;
   if (tpm2_process_ready() != 0 || !ctx || (!params && len))
      return -1;

   pthread_mutex_lock(&ctx->mu);
   OPENSSL_cleanse(ctx->kek, sizeof(ctx->kek));
   ctx->kek_ready = 0;
   ctx->sealed = 1;
   int rc = -1;
   int version = 0;
   uint64_t bound_gen = 0;
   TPM2B_PUBLIC pub;
   TPM2B_PRIVATE priv;
   uint8_t recovered[VAULT_KEK_LEN];
   memset(&pub, 0, sizeof(pub));
   memset(&priv, 0, sizeof(priv));
   memset(recovered, 0, sizeof(recovered));

   if (ensure_ready(ctx) != 0 || ensure_primary(ctx, 0) != 0 ||
       blob_read(ctx->blob_path, &version, &bound_gen, &pub, &priv) != 0 || version != 2 ||
       unseal_loaded_locked(ctx, &pub, &priv, bound_gen, params, len, recovered, NULL) != 0)
      goto out;
   memcpy(ctx->kek, recovered, sizeof(ctx->kek));
   ctx->kek_ready = 1;
   ctx->sealed = 0;
   rc = 0;
out:
   OPENSSL_cleanse(recovered, sizeof(recovered));
   OPENSSL_cleanse(&priv, sizeof(priv));
   OPENSSL_cleanse(&pub, sizeof(pub));
   pthread_mutex_unlock(&ctx->mu);
   return rc;
}

static int tpm2_unseal_typed(void *vctx, const void *params, size_t len)
{
   tpm2_ctx_t *ctx = vctx;
   if (tpm2_process_ready() != 0 || !ctx || (!params && len))
      return VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;

   pthread_mutex_lock(&ctx->mu);
   OPENSSL_cleanse(ctx->kek, sizeof(ctx->kek));
   ctx->kek_ready = 0;
   ctx->sealed = 1;
   vault_custody_auth_result_t result = VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;
   int version = 0;
   uint64_t bound_gen = 0;
   TPM2B_PUBLIC pub;
   TPM2B_PRIVATE priv;
   uint8_t recovered[VAULT_KEK_LEN];
   memset(&pub, 0, sizeof(pub));
   memset(&priv, 0, sizeof(priv));
   memset(recovered, 0, sizeof(recovered));

   if (ensure_ready(ctx) != 0)
   {
      result = VAULT_CUSTODY_AUTH_BACKEND_UNAVAILABLE;
      goto out;
   }
   if (ensure_primary(ctx, 0) != 0 ||
       blob_read(ctx->blob_path, &version, &bound_gen, &pub, &priv) != 0 || version != 2)
      goto out;
   result = tpm2_authorization_preflight_locked(ctx, params, len, bound_gen);
   if (result != VAULT_CUSTODY_AUTHORIZED)
      goto out;
   if (unseal_loaded_locked(ctx, &pub, &priv, bound_gen, params, len, recovered, &result) != 0)
      goto out;
   memcpy(ctx->kek, recovered, sizeof(ctx->kek));
   ctx->kek_ready = 1;
   ctx->sealed = 0;
   result = VAULT_CUSTODY_AUTHORIZED;
out:
   if (result != VAULT_CUSTODY_AUTHORIZED)
   {
      OPENSSL_cleanse(ctx->kek, sizeof(ctx->kek));
      ctx->kek_ready = 0;
      ctx->sealed = 1;
   }
   OPENSSL_cleanse(recovered, sizeof(recovered));
   OPENSSL_cleanse(&priv, sizeof(priv));
   OPENSSL_cleanse(&pub, sizeof(pub));
   pthread_mutex_unlock(&ctx->mu);
   return result;
}

static int tpm2_seal(void *vctx)
{
   tpm2_ctx_t *ctx = vctx;
   if (tpm2_process_ready() != 0 || !ctx)
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
    .local_status = tpm2_local_status,
    .after_fork_child = tpm2_after_fork_child_impl,
    .authorization_preflight = tpm2_authorization_preflight,
    .typed_unseal = tpm2_unseal_typed,
    .authorization_preflight_current = tpm2_authorization_preflight_current,
};

const vault_custody_provider_t *vault_custody_tpm2_provider(void)
{
   (void)tpm2_process_ready();
   return &g_tpm2_provider;
}

/* Seal `kek || be64(generation)` as a v2 (PolicyNV-bound) keyedhash object and
 * ATOMICALLY write it to ctx->blob_path. Requires ctx->primary verified and the NV
 * counter resolved with its auth set (nv_ensure done) — the caller owns that ordering
 * (provision reads the current gen; reseal increments FIRST). Computes the
 * generation-bound authPolicy (compute_seal_policy), builds the userWithAuth-CLEAR
 * template, and Creates the object over a salted + command-DECRYPT session so the KEK
 * is encrypted toward the TPM. Caller holds ctx->mu. 0/-1. */
static int seal_generation_path(tpm2_ctx_t *ctx, const uint8_t kek[VAULT_KEK_LEN],
                                uint64_t generation, const void *secret, size_t secret_len,
                                const char *path, char *errbuf, size_t errlen)
{
   int rc = -1;
   ESYS_TR sess = ESYS_TR_NONE;
   TPM2B_DIGEST policy;
   memset(&policy, 0, sizeof(policy));
   TPM2B_AUTH auth;
   memset(&auth, 0, sizeof(auth));
   TPM2B_SENSITIVE_CREATE in_sens;
   memset(&in_sens, 0, sizeof(in_sens));
   TPM2B_PRIVATE *out_priv = NULL;
   TPM2B_PUBLIC *out_pub = NULL;

   if (compute_seal_policy(ctx, generation, &policy) != 0)
   {
      tpm2_set_err(errbuf, errlen, "tpm2 seal: compute PolicyNV authPolicy failed");
      goto out;
   }
   if (derive_authvalue(secret, secret_len, &auth) != 0)
   {
      tpm2_set_err(errbuf, errlen, "tpm2 seal: authValue derivation failed");
      goto out;
   }

   TPM2B_PUBLIC tmpl = seal_template_v2(&policy);
   in_sens.sensitive.userAuth = auth; /* struct copy: object authValue (PolicyAuthValue) */
   in_sens.sensitive.data.size = TPM2_SEALED_LEN;
   memcpy(in_sens.sensitive.data.buffer, kek, VAULT_KEK_LEN);
   put_be64(in_sens.sensitive.data.buffer + VAULT_KEK_LEN, generation);
   TPML_PCR_SELECTION pcr = {.count = 0};
   TPM2B_DATA outside = {.size = 0};

   /* Salted (tpmKey = our primary) + command-parameter-DECRYPT session so the
    * inSensitive area (KEK||gen) is ENCRYPTED toward the TPM. */
   TPMT_SYM_DEF psym;
   memset(&psym, 0, sizeof(psym));
   psym.algorithm = TPM2_ALG_AES;
   psym.keyBits.aes = 128;
   psym.mode.aes = TPM2_ALG_CFB;
   TSS2_RC trc =
       Esys_StartAuthSession(ctx->esys, ctx->primary, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
                             ESYS_TR_NONE, NULL, TPM2_SE_HMAC, &psym, TPM2_ALG_SHA256, &sess);
   if (trc != TSS2_RC_SUCCESS)
   {
      tpm2_set_err(errbuf, errlen, "tpm2 seal: StartAuthSession failed");
      goto out;
   }
   trc = Esys_TRSess_SetAttributes(ctx->esys, sess,
                                   TPMA_SESSION_CONTINUESESSION | TPMA_SESSION_DECRYPT,
                                   TPMA_SESSION_CONTINUESESSION | TPMA_SESSION_DECRYPT);
   if (trc != TSS2_RC_SUCCESS)
   {
      tpm2_set_err(errbuf, errlen, "tpm2 seal: session attrs failed");
      goto out;
   }

   trc = Esys_Create(ctx->esys, ctx->primary, sess, ESYS_TR_NONE, ESYS_TR_NONE, &in_sens, &tmpl,
                     &outside, &pcr, &out_priv, &out_pub, NULL, NULL, NULL);
   OPENSSL_cleanse(&in_sens, sizeof(in_sens)); /* KEK + gen + authValue leave RAM ASAP */
   OPENSSL_cleanse(&auth, sizeof(auth));
   if (trc != TSS2_RC_SUCCESS)
   {
      tpm2_set_err(errbuf, errlen, "tpm2 seal: Esys_Create(seal) failed");
      goto out;
   }
   if (blob_write(path, generation, out_pub, out_priv) != 0)
   {
      tpm2_set_err(errbuf, errlen, "tpm2 seal: sealed blob write failed");
      goto out;
   }
   rc = 0;

out:
   OPENSSL_cleanse(&in_sens, sizeof(in_sens));
   OPENSSL_cleanse(&auth, sizeof(auth));
   OPENSSL_cleanse(&policy, sizeof(policy));
   if (sess != ESYS_TR_NONE)
      Esys_FlushContext(ctx->esys, sess);
   Esys_Free(out_pub);
   Esys_Free(out_priv);
   return rc;
}

static int seal_generation(tpm2_ctx_t *ctx, const uint8_t kek[VAULT_KEK_LEN], uint64_t generation,
                           const void *secret, size_t secret_len, char *errbuf, size_t errlen)
{
   return seal_generation_path(ctx, kek, generation, secret, secret_len, ctx->blob_path, errbuf,
                               errlen);
}

int vault_custody_tpm2_provision(const uint8_t kek[VAULT_KEK_LEN], const char *secret, char *errbuf,
                                 size_t errlen)
{
   if (tpm2_process_ready() != 0 || !kek || !secret)
   {
      tpm2_set_err(errbuf, errlen, "tpm2 provision: null kek or secret");
      return -1;
   }
   pthread_mutex_lock(&g_ctx.mu);
   int rc = -1;
   uint64_t generation = 0;

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
   /* Ensure the NV counter exists (define + first-increment activation if fresh),
    * verified + secret-authorized, then read the CURRENT generation to bind. */
   if (nv_ensure(&g_ctx, secret, strlen(secret), 1) != 0)
   {
      tpm2_set_err(errbuf, errlen, "tpm2 provision: NV counter ensure failed");
      goto out;
   }
   if (nv_read(&g_ctx, &generation) != 0)
   {
      tpm2_set_err(errbuf, errlen, "tpm2 provision: NV counter read failed");
      goto out;
   }
   if (seal_generation(&g_ctx, kek, generation, secret, strlen(secret), errbuf, errlen) != 0)
      goto out; /* seal_generation set errbuf */
   rc = 0;

out:
   pthread_mutex_unlock(&g_ctx.mu);
   return rc;
}

static void reseal_mark_sealed(void)
{
   OPENSSL_cleanse(g_ctx.kek, sizeof(g_ctx.kek));
   g_ctx.kek_ready = 0;
   g_ctx.sealed = 1;
}

static int reseal_path(char *out, size_t cap, const char *suffix)
{
   return (size_t)snprintf(out, cap, "%s%s", g_ctx.blob_path, suffix) < cap ? 0 : -1;
}

static int reseal_lock(void)
{
   char path[1152];
   if (reseal_path(path, sizeof(path), ".reseal.lock") != 0 || validate_parent(path) != 0)
      return -1;
   int fd = open(path, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
   if (fd < 0)
      return -1;
   /* Publish before validation/flock so a fork at every post-open instruction
    * lets the child handler close its copy. Before flock this is harmless; after
    * flock it prevents the child from pinning the parent's lock. */
   g_reseal_lock_fd = fd;
   struct stat st;
   if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 ||
       (st.st_mode & 0777) != 0600 || flock(fd, LOCK_EX | LOCK_NB) != 0)
   {
      (void)flock(fd, LOCK_UN);
      g_reseal_lock_fd = -1;
      close(fd);
      return -1;
   }
   return fd;
}

static void reseal_unlock(int fd)
{
   if (fd >= 0)
   {
      /* Unlock before unpublishing. A fork before the unlock closes the tracked
       * child copy; a fork after it may inherit an fd but cannot pin a flock. */
      (void)flock(fd, LOCK_UN);
      if (g_reseal_lock_fd == fd)
         g_reseal_lock_fd = -1;
      close(fd);
   }
}

static void receipt_from_header(const uint8_t *h, vault_tpm2_reseal_receipt_t *r)
{
   memset(r, 0, sizeof(*r));
   memcpy(r->operation_id, h + 8, 16);
   r->old_generation = get_be64(h + 24);
   r->new_generation = get_be64(h + 32);
   memcpy(r->predecessor_digest, h + 40, 32);
   memcpy(r->capsule_digest, h + 72, 32);
   memcpy(r->future_digest, h + 104, 32);
   memcpy(r->new_kek_digest, h + 136, 32);
   SHA256(h, TPM2_RESEAL_BUNDLE_HDR, r->manifest_digest);
}

typedef struct
{
   uint8_t bytes[TPM2_RESEAL_BUNDLE_MAX];
   size_t len;
   const uint8_t *capsule;
   size_t capsule_len;
   const uint8_t *future;
   size_t future_len;
   vault_tpm2_reseal_receipt_t receipt;
} reseal_bundle_t;

static int bundle_load(reseal_bundle_t *b)
{
   char path[1152];
   memset(b, 0, sizeof(*b));
   if (reseal_path(path, sizeof(path), ".reseal.bundle") != 0)
      return -2;
   if (raw_read(path, b->bytes, sizeof(b->bytes), &b->len) != 0)
   {
      struct stat st;
      if (lstat(path, &st) != 0 && errno == ENOENT)
         return -1;
      /* An existing but unreadable, unsafe, empty, or concurrently replaced
       * bundle is corruption, never the ABSENT state. */
      return -2;
   }
   if (b->len < TPM2_RESEAL_BUNDLE_HDR || memcmp(b->bytes, TPM2_RESEAL_BUNDLE_MAGIC, 8) != 0)
      return -2;
   uint32_t clen = get_be32(b->bytes + 168), flen = get_be32(b->bytes + 172);
   if (!clen || !flen || clen > TPM2_BLOB_MAX || flen > TPM2_BLOB_MAX ||
       (uint64_t)TPM2_RESEAL_BUNDLE_HDR + clen + flen != b->len)
      return -2;
   b->capsule = b->bytes + TPM2_RESEAL_BUNDLE_HDR;
   b->capsule_len = clen;
   b->future = b->capsule + clen;
   b->future_len = flen;
   uint8_t digest[32];
   SHA256(b->capsule, b->capsule_len, digest);
   if (memcmp(digest, b->bytes + 72, 32) != 0)
      return -2;
   SHA256(b->future, b->future_len, digest);
   if (memcmp(digest, b->bytes + 104, 32) != 0)
      return -2;
   receipt_from_header(b->bytes, &b->receipt);
   if (blob_bytes_validate(&g_ctx, b->capsule, b->capsule_len, b->receipt.old_generation) != 0 ||
       blob_bytes_validate(&g_ctx, b->future, b->future_len, b->receipt.new_generation) != 0)
      return -2;
   return 0;
}

static int reseal_status_locked(const vault_tpm2_reseal_receipt_t *receipt, const char *secret,
                                vault_tpm2_reseal_status_t *out, reseal_bundle_t *bundle)
{
   reseal_bundle_t local;
   reseal_bundle_t *b = bundle ? bundle : &local;
   uint64_t nv = 0;
   if (nv_ensure(&g_ctx, secret, strlen(secret), 0) != 0 || nv_read(&g_ctx, &nv) != 0)
      return VAULT_TPM2_RESEAL_ERR;
   int br = bundle_load(b);
   if (br == -2)
   {
      *out = VAULT_TPM2_RESEAL_CORRUPT;
      return VAULT_TPM2_RESEAL_INTEGRITY;
   }
   if (br != 0)
   {
      if (!receipt)
      {
         *out = VAULT_TPM2_RESEAL_ABSENT;
         return VAULT_TPM2_RESEAL_OK;
      }
      if (receipt->old_generation == UINT64_MAX ||
          receipt->new_generation != receipt->old_generation + 1)
      {
         *out = VAULT_TPM2_RESEAL_CONFLICT;
         return VAULT_TPM2_RESEAL_INTEGRITY;
      }
      if (nv == receipt->new_generation)
      {
         uint8_t active[32];
         if (blob_file_digest_validate(&g_ctx, g_ctx.blob_path, receipt->new_generation, active) ==
                 0 &&
             memcmp(active, receipt->future_digest, 32) == 0 && fsync_parent(g_ctx.blob_path) == 0)
         {
            *out = VAULT_TPM2_RESEAL_CLEANED;
            return VAULT_TPM2_RESEAL_OK;
         }
         *out = VAULT_TPM2_RESEAL_CONFLICT;
         return VAULT_TPM2_RESEAL_INTEGRITY;
      }
      if (nv == receipt->old_generation)
      {
         uint8_t active[32];
         if (blob_file_digest_validate(&g_ctx, g_ctx.blob_path, receipt->old_generation, active) !=
                 0 ||
             memcmp(active, receipt->predecessor_digest, 32) != 0 ||
             fsync_parent(g_ctx.blob_path) != 0)
         {
            *out = VAULT_TPM2_RESEAL_CONFLICT;
            return VAULT_TPM2_RESEAL_INTEGRITY;
         }
         *out = VAULT_TPM2_RESEAL_ABSENT;
         return VAULT_TPM2_RESEAL_OK;
      }
      *out = VAULT_TPM2_RESEAL_CONFLICT;
      return VAULT_TPM2_RESEAL_INTEGRITY;
   }
   if ((receipt && !vault_reseal_receipt_equal(receipt, &b->receipt)) ||
       b->receipt.old_generation == UINT64_MAX ||
       b->receipt.new_generation != b->receipt.old_generation + 1)
   {
      *out = VAULT_TPM2_RESEAL_CONFLICT;
      return VAULT_TPM2_RESEAL_INTEGRITY;
   }
   uint8_t active[32];
   if (nv == b->receipt.old_generation &&
       blob_file_digest_validate(&g_ctx, g_ctx.blob_path, b->receipt.old_generation, active) == 0 &&
       memcmp(active, b->receipt.predecessor_digest, 32) == 0)
      *out = VAULT_TPM2_RESEAL_PREPARED;
   else if (nv == b->receipt.new_generation &&
            blob_file_digest_validate(&g_ctx, g_ctx.blob_path, b->receipt.new_generation, active) ==
                0 &&
            memcmp(active, b->receipt.future_digest, 32) == 0)
   {
      if (fsync_parent(g_ctx.blob_path) != 0)
         return VAULT_TPM2_RESEAL_ERR;
      *out = VAULT_TPM2_RESEAL_INSTALLED;
   }
   else if (nv == b->receipt.new_generation &&
            blob_file_digest_validate(&g_ctx, g_ctx.blob_path, b->receipt.old_generation, active) ==
                0 &&
            memcmp(active, b->receipt.predecessor_digest, 32) == 0)
      *out = VAULT_TPM2_RESEAL_NV_ADVANCED;
   else
   {
      *out = VAULT_TPM2_RESEAL_CONFLICT;
      return VAULT_TPM2_RESEAL_INTEGRITY;
   }
   return VAULT_TPM2_RESEAL_OK;
}

int vault_custody_tpm2_reseal_prepare(const uint8_t operation_id[16],
                                      uint64_t expected_old_generation,
                                      const uint8_t new_kek[VAULT_KEK_LEN], const char *secret,
                                      vault_tpm2_reseal_receipt_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (tpm2_process_ready() != 0 || !operation_id || !new_kek || !secret || !out ||
       expected_old_generation == UINT64_MAX)
      return VAULT_TPM2_RESEAL_ERR;
   pthread_mutex_lock(&g_ctx.mu);
   int rc = VAULT_TPM2_RESEAL_ERR, lockfd = -1;
   reseal_mark_sealed();
   if (ensure_ready(&g_ctx) != 0 || ensure_primary(&g_ctx, 0) != 0 ||
       nv_ensure(&g_ctx, secret, strlen(secret), 0) != 0 || (lockfd = reseal_lock()) < 0)
      goto out;
   reseal_bundle_t existing;
   int er = bundle_load(&existing);
   if (er == 0)
   {
      uint8_t supplied_kek_digest[32];
      SHA256(new_kek, VAULT_KEK_LEN, supplied_kek_digest);
      vault_tpm2_reseal_status_t st = VAULT_TPM2_RESEAL_CORRUPT;
      if (existing.receipt.old_generation == expected_old_generation &&
          memcmp(existing.receipt.operation_id, operation_id, 16) == 0 &&
          memcmp(existing.receipt.new_kek_digest, supplied_kek_digest, 32) == 0 &&
          reseal_status_locked(&existing.receipt, secret, &st, NULL) == 0 &&
          st == VAULT_TPM2_RESEAL_PREPARED && fsync_parent(g_ctx.blob_path) == 0)
      {
         *out = existing.receipt;
         rc = VAULT_TPM2_RESEAL_OK;
      }
      else
         rc = VAULT_TPM2_RESEAL_INTEGRITY;
      goto out;
   }
   if (er == -2)
   {
      rc = VAULT_TPM2_RESEAL_INTEGRITY;
      goto out;
   }
   uint64_t nv = 0;
   uint8_t pred[32];
   if (nv_read(&g_ctx, &nv) != 0 || nv != expected_old_generation ||
       blob_file_digest_validate(&g_ctx, g_ctx.blob_path, expected_old_generation, pred) != 0)
      goto out;
   char cap_path[1152], fut_path[1152], bundle_path[1152];
   if ((size_t)snprintf(cap_path, sizeof(cap_path), "%s.reseal.cap.%d", g_ctx.blob_path,
                        (int)getpid()) >= sizeof(cap_path) ||
       (size_t)snprintf(fut_path, sizeof(fut_path), "%s.reseal.future.%d", g_ctx.blob_path,
                        (int)getpid()) >= sizeof(fut_path) ||
       reseal_path(bundle_path, sizeof(bundle_path), ".reseal.bundle") != 0)
      goto out;
   (void)unlink(cap_path);
   (void)unlink(fut_path);
   if (seal_generation_path(&g_ctx, new_kek, nv, secret, strlen(secret), cap_path, NULL, 0) != 0 ||
       seal_generation_path(&g_ctx, new_kek, nv + 1, secret, strlen(secret), fut_path, NULL, 0) !=
           0)
      goto prep_done;
   uint8_t cap[TPM2_BLOB_MAX], fut[TPM2_BLOB_MAX], bytes[TPM2_RESEAL_BUNDLE_MAX];
   size_t clen = 0, flen = 0;
   if (raw_read(cap_path, cap, sizeof(cap), &clen) != 0 ||
       raw_read(fut_path, fut, sizeof(fut), &flen) != 0)
      goto prep_cleanse;
   memset(bytes, 0, TPM2_RESEAL_BUNDLE_HDR);
   memcpy(bytes, TPM2_RESEAL_BUNDLE_MAGIC, 8);
   memcpy(bytes + 8, operation_id, 16);
   put_be64(bytes + 24, nv);
   put_be64(bytes + 32, nv + 1);
   memcpy(bytes + 40, pred, 32);
   SHA256(cap, clen, bytes + 72);
   SHA256(fut, flen, bytes + 104);
   SHA256(new_kek, VAULT_KEK_LEN, bytes + 136);
   put_be32(bytes + 168, (uint32_t)clen);
   put_be32(bytes + 172, (uint32_t)flen);
   memcpy(bytes + TPM2_RESEAL_BUNDLE_HDR, cap, clen);
   memcpy(bytes + TPM2_RESEAL_BUNDLE_HDR + clen, fut, flen);
   if (raw_write_atomic(bundle_path, bytes, TPM2_RESEAL_BUNDLE_HDR + clen + flen, 0) == 0)
   {
      receipt_from_header(bytes, out);
      rc = VAULT_TPM2_RESEAL_OK;
   }
   OPENSSL_cleanse(bytes, sizeof(bytes));
prep_cleanse:
   OPENSSL_cleanse(cap, sizeof(cap));
   OPENSSL_cleanse(fut, sizeof(fut));
prep_done:
   (void)unlink(cap_path);
   (void)unlink(fut_path);
   fsync_parent(g_ctx.blob_path);
out:
   reseal_unlock(lockfd);
   pthread_mutex_unlock(&g_ctx.mu);
   return rc;
}

int vault_custody_tpm2_reseal_discover(const uint8_t operation_id[16],
                                       uint64_t expected_old_generation, const char *secret,
                                       vault_tpm2_reseal_receipt_t *receipt,
                                       vault_tpm2_reseal_status_t *status)
{
   if (receipt)
      OPENSSL_cleanse(receipt, sizeof(*receipt));
   if (status)
      *status = VAULT_TPM2_RESEAL_CORRUPT;
   if (tpm2_process_ready() != 0 || !operation_id || !secret || !receipt || !status ||
       expected_old_generation == UINT64_MAX)
      return VAULT_TPM2_RESEAL_ERR;

   pthread_mutex_lock(&g_ctx.mu);
   int rc = VAULT_TPM2_RESEAL_ERR, lockfd = -1;
   reseal_bundle_t b;
   memset(&b, 0, sizeof(b));
   reseal_mark_sealed();
   if (ensure_ready(&g_ctx) != 0 || ensure_primary(&g_ctx, 0) != 0)
      goto out;
   if ((lockfd = reseal_lock()) < 0)
   {
      rc = VAULT_TPM2_RESEAL_BUSY;
      goto out;
   }
   rc = reseal_status_locked(NULL, secret, status, &b);
   if (rc != VAULT_TPM2_RESEAL_OK)
      goto out;
   if (*status == VAULT_TPM2_RESEAL_ABSENT)
      goto out;
   if ((*status != VAULT_TPM2_RESEAL_PREPARED && *status != VAULT_TPM2_RESEAL_NV_ADVANCED &&
        *status != VAULT_TPM2_RESEAL_INSTALLED) ||
       b.receipt.old_generation != expected_old_generation ||
       b.receipt.new_generation != expected_old_generation + 1 ||
       CRYPTO_memcmp(b.receipt.operation_id, operation_id, sizeof(b.receipt.operation_id)) != 0)
   {
      *status = VAULT_TPM2_RESEAL_CONFLICT;
      rc = VAULT_TPM2_RESEAL_INTEGRITY;
      goto out;
   }
   *receipt = b.receipt;
out:
   if (rc != VAULT_TPM2_RESEAL_OK || *status == VAULT_TPM2_RESEAL_ABSENT)
      OPENSSL_cleanse(receipt, sizeof(*receipt));
   OPENSSL_cleanse(&b, sizeof(b));
   reseal_mark_sealed();
   reseal_unlock(lockfd);
   pthread_mutex_unlock(&g_ctx.mu);
   return rc;
}

int vault_custody_tpm2_reseal_recover_kek(const vault_tpm2_reseal_receipt_t *receipt,
                                          const char *secret, uint8_t new_kek[VAULT_KEK_LEN])
{
   if (new_kek)
      OPENSSL_cleanse(new_kek, VAULT_KEK_LEN);
   if (tpm2_process_ready() != 0 || !receipt || !secret || !new_kek)
      return VAULT_TPM2_RESEAL_ERR;

   pthread_mutex_lock(&g_ctx.mu);
   int rc = VAULT_TPM2_RESEAL_ERR, lockfd = -1;
   reseal_bundle_t b;
   vault_tpm2_reseal_status_t status = VAULT_TPM2_RESEAL_CORRUPT;
   TPM2B_PUBLIC pub;
   TPM2B_PRIVATE priv;
   uint64_t capsule_generation = 0;
   uint8_t recovered[VAULT_KEK_LEN], digest[32];
   memset(&b, 0, sizeof(b));
   memset(&pub, 0, sizeof(pub));
   memset(&priv, 0, sizeof(priv));
   memset(recovered, 0, sizeof(recovered));
   memset(digest, 0, sizeof(digest));
   reseal_mark_sealed();
   if (ensure_ready(&g_ctx) != 0 || ensure_primary(&g_ctx, 0) != 0)
      goto out;
   if ((lockfd = reseal_lock()) < 0)
   {
      rc = VAULT_TPM2_RESEAL_BUSY;
      goto out;
   }
   rc = reseal_status_locked(receipt, secret, &status, &b);
   if (rc != VAULT_TPM2_RESEAL_OK)
      goto out;
   if (status != VAULT_TPM2_RESEAL_PREPARED)
   {
      rc = VAULT_TPM2_RESEAL_INTEGRITY;
      goto out;
   }
   if (blob_bytes_unmarshal_v2(b.capsule, b.capsule_len, &capsule_generation, &pub, &priv) != 0 ||
       capsule_generation != receipt->old_generation ||
       unseal_loaded_locked(&g_ctx, &pub, &priv, capsule_generation, secret, strlen(secret),
                            recovered, NULL) != 0)
   {
      rc = VAULT_TPM2_RESEAL_INTEGRITY;
      goto out;
   }
   SHA256(recovered, sizeof(recovered), digest);
   if (CRYPTO_memcmp(digest, receipt->new_kek_digest, sizeof(digest)) != 0)
   {
      rc = VAULT_TPM2_RESEAL_INTEGRITY;
      goto out;
   }
   memcpy(new_kek, recovered, VAULT_KEK_LEN);
   rc = VAULT_TPM2_RESEAL_OK;
out:
   if (rc != VAULT_TPM2_RESEAL_OK)
      OPENSSL_cleanse(new_kek, VAULT_KEK_LEN);
   OPENSSL_cleanse(digest, sizeof(digest));
   OPENSSL_cleanse(recovered, sizeof(recovered));
   OPENSSL_cleanse(&priv, sizeof(priv));
   OPENSSL_cleanse(&pub, sizeof(pub));
   OPENSSL_cleanse(&b, sizeof(b));
   reseal_mark_sealed();
   reseal_unlock(lockfd);
   pthread_mutex_unlock(&g_ctx.mu);
   return rc;
}

int vault_custody_tpm2_reseal_status(const vault_tpm2_reseal_receipt_t *receipt, const char *secret,
                                     vault_tpm2_reseal_status_t *out)
{
   if (out)
      *out = VAULT_TPM2_RESEAL_CORRUPT;
   if (tpm2_process_ready() != 0 || !secret || !out)
      return VAULT_TPM2_RESEAL_ERR;
   pthread_mutex_lock(&g_ctx.mu);
   int rc = VAULT_TPM2_RESEAL_ERR, lockfd = -1;
   reseal_mark_sealed();
   if (ensure_ready(&g_ctx) == 0 && ensure_primary(&g_ctx, 0) == 0 && (lockfd = reseal_lock()) >= 0)
      rc = reseal_status_locked(receipt, secret, out, NULL);
   reseal_unlock(lockfd);
   pthread_mutex_unlock(&g_ctx.mu);
   return rc;
}

int vault_custody_tpm2_reseal_commit(const vault_tpm2_reseal_receipt_t *receipt, const char *secret,
                                     vault_tpm2_reseal_status_t *out)
{
   if (out)
      *out = VAULT_TPM2_RESEAL_CORRUPT;
   if (tpm2_process_ready() != 0 || !receipt || !secret || !out)
      return VAULT_TPM2_RESEAL_ERR;
   pthread_mutex_lock(&g_ctx.mu);
   int rc = VAULT_TPM2_RESEAL_ERR, lockfd = -1;
   reseal_bundle_t b;
   reseal_mark_sealed();
   if (ensure_ready(&g_ctx) != 0 || ensure_primary(&g_ctx, 0) != 0 || (lockfd = reseal_lock()) < 0)
      goto out;
   rc = reseal_status_locked(receipt, secret, out, &b);
   if (rc != 0 || *out == VAULT_TPM2_RESEAL_CLEANED)
   {
      if (*out == VAULT_TPM2_RESEAL_CLEANED)
         rc = VAULT_TPM2_RESEAL_INTEGRITY;
      goto out;
   }
   if (*out == VAULT_TPM2_RESEAL_INSTALLED)
   {
      rc = VAULT_TPM2_RESEAL_OK;
      goto out;
   }
   /* From this point onward success requires a final, verified INSTALLED
    * classification. Never leak status()'s earlier OK across a failed TPM or
    * filesystem mutation. */
   rc = VAULT_TPM2_RESEAL_ERR;
   if (*out == VAULT_TPM2_RESEAL_PREPARED)
   {
      uint64_t before = 0;
      uint8_t active[32];
      if (nv_read(&g_ctx, &before) != 0 || before != receipt->old_generation ||
          blob_file_digest_validate(&g_ctx, g_ctx.blob_path, receipt->old_generation, active) !=
              0 ||
          memcmp(active, receipt->predecessor_digest, 32) != 0)
      {
         *out = VAULT_TPM2_RESEAL_CONFLICT;
         rc = VAULT_TPM2_RESEAL_INTEGRITY;
         goto out;
      }
      if (nv_increment(&g_ctx) != 0)
      {
         uint64_t observed = 0;
         if (nv_read(&g_ctx, &observed) != 0 || observed != receipt->new_generation)
            goto out;
      }
      uint64_t observed = 0;
      if (nv_read(&g_ctx, &observed) != 0 || observed != receipt->new_generation)
         goto out;
      *out = VAULT_TPM2_RESEAL_NV_ADVANCED;
   }
   if (*out == VAULT_TPM2_RESEAL_NV_ADVANCED)
   {
      uint8_t active[32];
      int future_valid = blob_file_digest_validate(&g_ctx, g_ctx.blob_path, receipt->new_generation,
                                                   active) == 0 &&
                         memcmp(active, receipt->future_digest, 32) == 0;
      if (!future_valid)
      {
         if (blob_file_digest_validate(&g_ctx, g_ctx.blob_path, receipt->old_generation, active) !=
                 0 ||
             memcmp(active, receipt->predecessor_digest, 32) != 0)
         {
            *out = VAULT_TPM2_RESEAL_CONFLICT;
            rc = VAULT_TPM2_RESEAL_INTEGRITY;
            goto out;
         }
         if (raw_write_atomic(g_ctx.blob_path, b.future, b.future_len, 1) != 0)
            goto out;
      }
      if (fsync_parent(g_ctx.blob_path) != 0)
         goto out;
      rc = reseal_status_locked(receipt, secret, out, &b);
      if (rc == 0 && *out != VAULT_TPM2_RESEAL_INSTALLED)
         rc = VAULT_TPM2_RESEAL_INTEGRITY;
   }
out:
   reseal_mark_sealed();
   reseal_unlock(lockfd);
   pthread_mutex_unlock(&g_ctx.mu);
   return rc;
}

int vault_custody_tpm2_reseal_abort(const vault_tpm2_reseal_receipt_t *receipt, const char *secret)
{
   if (tpm2_process_ready() != 0 || !receipt || !secret)
      return VAULT_TPM2_RESEAL_ERR;
   pthread_mutex_lock(&g_ctx.mu);
   int rc = VAULT_TPM2_RESEAL_ERR, lockfd = -1;
   vault_tpm2_reseal_status_t st;
   reseal_mark_sealed();
   if (ensure_ready(&g_ctx) == 0 && ensure_primary(&g_ctx, 0) == 0 &&
       (lockfd = reseal_lock()) >= 0 && reseal_status_locked(receipt, secret, &st, NULL) == 0 &&
       (st == VAULT_TPM2_RESEAL_PREPARED || st == VAULT_TPM2_RESEAL_ABSENT))
   {
      char path[1152];
      if (reseal_path(path, sizeof(path), ".reseal.bundle") == 0 &&
          (st == VAULT_TPM2_RESEAL_ABSENT || unlink(path) == 0))
      {
         rc = fsync_parent(path) == 0 ? VAULT_TPM2_RESEAL_OK : VAULT_TPM2_RESEAL_ERR;
      }
   }
   reseal_mark_sealed();
   reseal_unlock(lockfd);
   pthread_mutex_unlock(&g_ctx.mu);
   return rc;
}

int vault_custody_tpm2_reseal_cleanup(const vault_tpm2_reseal_receipt_t *receipt,
                                      const char *secret,
                                      vault_tpm2_cleanup_authorization_t authorization)
{
   if (tpm2_process_ready() != 0 || !receipt || !secret ||
       authorization != VAULT_TPM2_CLEANUP_TERMINAL_COMPLETED)
      return VAULT_TPM2_RESEAL_ERR;
   pthread_mutex_lock(&g_ctx.mu);
   int rc = VAULT_TPM2_RESEAL_ERR, lockfd = -1;
   vault_tpm2_reseal_status_t st;
   reseal_mark_sealed();
   if (ensure_ready(&g_ctx) == 0 && ensure_primary(&g_ctx, 0) == 0 &&
       (lockfd = reseal_lock()) >= 0 && reseal_status_locked(receipt, secret, &st, NULL) == 0 &&
       (st == VAULT_TPM2_RESEAL_INSTALLED || st == VAULT_TPM2_RESEAL_CLEANED))
   {
      char path[1152];
      if (st == VAULT_TPM2_RESEAL_CLEANED)
         rc = VAULT_TPM2_RESEAL_OK;
      else if (reseal_path(path, sizeof(path), ".reseal.bundle") == 0 && unlink(path) == 0)
      {
         rc = fsync_parent(path) == 0 ? VAULT_TPM2_RESEAL_OK : VAULT_TPM2_RESEAL_ERR;
      }
   }
   reseal_mark_sealed();
   reseal_unlock(lockfd);
   pthread_mutex_unlock(&g_ctx.mu);
   return rc;
}

int vault_custody_tpm2_reseal(const uint8_t new_kek[VAULT_KEK_LEN], const char *secret)
{
   if (tpm2_process_ready() != 0 || !new_kek || !secret)
      return -1;
   pthread_mutex_lock(&g_ctx.mu);
   int rc = -1;
   int lockfd = -1;
   uint64_t generation = 0;

   if (ensure_ready(&g_ctx) != 0)
      goto out;
   if ((lockfd = reseal_lock()) < 0)
      goto out;
   char pending[1152];
   struct stat pending_st;
   if (reseal_path(pending, sizeof(pending), ".reseal.bundle") != 0 ||
       lstat(pending, &pending_st) == 0 || errno != ENOENT)
      goto out; /* compatibility helper never bypasses a prepared operation */
   if (ensure_primary(&g_ctx, 0) != 0) /* the primary must already exist (provisioned) */
      goto out;
   if (nv_ensure(&g_ctx, secret, strlen(secret), 1) != 0)
      goto out;
   /* INCREMENT-BEFORE-WRITE: bump the counter to G' FIRST, THEN seal+atomically write
    * the new blob bound to G'. A crash between leaves the OLD blob bound to G < G'
    * (its PolicyNV asserts NV==G, now false) -> the old blob is un-unsealable =
    * fail-closed (recovery = re-provision). The reverse order would leave a usable
    * old blob at an already-bumped counter. */
   if (nv_increment(&g_ctx) != 0)
      goto out;
   if (nv_read(&g_ctx, &generation) != 0)
      goto out;
   if (seal_generation(&g_ctx, new_kek, generation, secret, strlen(secret), NULL, 0) != 0)
      goto out;
   /* A rotation supersedes the old KEK: drop any cached (old) KEK and mark sealed
    * so get_kek cannot keep returning the obsolete key — the NEW blob must be
    * unsealed to materialize the new KEK. */
   OPENSSL_cleanse(g_ctx.kek, sizeof(g_ctx.kek));
   g_ctx.kek_ready = 0;
   g_ctx.sealed = 1;
   rc = 0;

out:
   reseal_unlock(lockfd);
   pthread_mutex_unlock(&g_ctx.mu);
   return rc;
}

int vault_custody_tpm2_nv_generation(const char *secret, uint64_t *out_gen)
{
   if (out_gen)
      *out_gen = 0;
   if (tpm2_process_ready() != 0 || !secret || !out_gen)
      return -1;
   pthread_mutex_lock(&g_ctx.mu);
   int rc = -1;
   uint64_t gen = 0;
   if (ensure_ready(&g_ctx) != 0)
      goto out;
   if (nv_ensure(&g_ctx, secret, strlen(secret), 0) != 0) /* resolve + AUTHREAD, NO create */
      goto out;
   if (nv_read(&g_ctx, &gen) != 0)
      goto out;
   *out_gen = gen;
   rc = 0;

out:
   pthread_mutex_unlock(&g_ctx.mu);
   return rc;
}

void vault_custody_tpm2_reset(void)
{
   if (tpm2_process_ready() != 0)
      return;
   pthread_mutex_lock(&g_ctx.mu);
   OPENSSL_cleanse(g_ctx.kek, sizeof(g_ctx.kek));
   g_ctx.kek_ready = 0;
   g_ctx.sealed = 1;
   if (g_ctx.nv_handle != ESYS_TR_NONE && g_ctx.esys)
      Esys_TR_Close(g_ctx.esys, &g_ctx.nv_handle);
   g_ctx.nv_handle = ESYS_TR_NONE;
   g_ctx.nv_auth_ready = 0;
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

static int stub_local_status(void *vctx, unsigned timeout_ms)
{
   (void)vctx;
   (void)timeout_ms;
   return VAULT_CUSTODY_LOCAL_UNAVAILABLE;
}

static int stub_authorization_preflight(void *vctx, const void *secret, size_t secret_len,
                                        uint64_t expected_generation)
{
   (void)vctx;
   (void)secret;
   (void)secret_len;
   (void)expected_generation;
   return VAULT_CUSTODY_AUTH_UNSUPPORTED;
}

static int stub_unseal_typed(void *vctx, const void *secret, size_t secret_len)
{
   (void)vctx;
   (void)secret;
   (void)secret_len;
   return VAULT_CUSTODY_AUTH_UNSUPPORTED;
}

static int stub_authorization_preflight_current(void *vctx, const void *secret, size_t secret_len,
                                                uint64_t *generation)
{
   (void)vctx;
   (void)secret;
   (void)secret_len;
   if (generation)
      *generation = 0;
   return VAULT_CUSTODY_AUTH_UNSUPPORTED;
}

static const vault_custody_provider_t g_tpm2_stub = {
    .name = "tpm2",
    .ctx = NULL,
    .get_kek = stub_get_kek,
    .rotate = stub_rotate,
    .is_sealed = stub_is_sealed,
    .unseal = stub_unseal,
    .seal = stub_seal,
    .local_status = stub_local_status,
    .authorization_preflight = stub_authorization_preflight,
    .typed_unseal = stub_unseal_typed,
    .authorization_preflight_current = stub_authorization_preflight_current,
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

int vault_custody_tpm2_reseal(const uint8_t new_kek[VAULT_KEK_LEN], const char *secret)
{
   (void)new_kek;
   (void)secret;
   return -1; /* built without TPM2 -> cannot reseal (fail closed) */
}

int vault_custody_tpm2_reseal_prepare(const uint8_t operation_id[16],
                                      uint64_t expected_old_generation,
                                      const uint8_t new_kek[VAULT_KEK_LEN], const char *secret,
                                      vault_tpm2_reseal_receipt_t *out)
{
   (void)operation_id;
   (void)expected_old_generation;
   (void)new_kek;
   (void)secret;
   if (out)
      memset(out, 0, sizeof(*out));
   return VAULT_TPM2_RESEAL_NOT_BUILT;
}

int vault_custody_tpm2_reseal_discover(const uint8_t operation_id[16],
                                       uint64_t expected_old_generation, const char *secret,
                                       vault_tpm2_reseal_receipt_t *receipt,
                                       vault_tpm2_reseal_status_t *status)
{
   (void)operation_id;
   (void)expected_old_generation;
   (void)secret;
   if (receipt)
      memset(receipt, 0, sizeof(*receipt));
   if (status)
      *status = VAULT_TPM2_RESEAL_ABSENT;
   return VAULT_TPM2_RESEAL_NOT_BUILT;
}

int vault_custody_tpm2_reseal_recover_kek(const vault_tpm2_reseal_receipt_t *receipt,
                                          const char *secret, uint8_t new_kek[VAULT_KEK_LEN])
{
   (void)receipt;
   (void)secret;
   if (new_kek)
      memset(new_kek, 0, VAULT_KEK_LEN);
   return VAULT_TPM2_RESEAL_NOT_BUILT;
}

int vault_custody_tpm2_reseal_status(const vault_tpm2_reseal_receipt_t *receipt, const char *secret,
                                     vault_tpm2_reseal_status_t *out)
{
   (void)receipt;
   (void)secret;
   if (out)
      *out = VAULT_TPM2_RESEAL_ABSENT;
   return VAULT_TPM2_RESEAL_NOT_BUILT;
}

int vault_custody_tpm2_reseal_commit(const vault_tpm2_reseal_receipt_t *receipt, const char *secret,
                                     vault_tpm2_reseal_status_t *out)
{
   return vault_custody_tpm2_reseal_status(receipt, secret, out);
}

int vault_custody_tpm2_reseal_abort(const vault_tpm2_reseal_receipt_t *receipt, const char *secret)
{
   (void)receipt;
   (void)secret;
   return VAULT_TPM2_RESEAL_NOT_BUILT;
}

int vault_custody_tpm2_reseal_cleanup(const vault_tpm2_reseal_receipt_t *receipt,
                                      const char *secret,
                                      vault_tpm2_cleanup_authorization_t authorization)
{
   (void)receipt;
   (void)secret;
   return authorization == VAULT_TPM2_CLEANUP_TERMINAL_COMPLETED ? VAULT_TPM2_RESEAL_NOT_BUILT
                                                                 : VAULT_TPM2_RESEAL_ERR;
}

int vault_custody_tpm2_nv_generation(const char *secret, uint64_t *out_gen)
{
   (void)secret;
   (void)out_gen;
   return -1; /* built without TPM2 -> no NV counter (fail closed) */
}

void vault_custody_tpm2_reset(void)
{
   /* nothing to reset in the stub */
}

#endif /* WITH_TPM2 */
