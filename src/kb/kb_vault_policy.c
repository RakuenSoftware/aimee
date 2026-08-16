/* kb_vault_policy.c: kb-only custody selection + P7 §3 live-key gate. See
 * kb_vault_policy.h. Owns the vault.custody enum and the fail-closed policy for
 * unimplemented anchors — kept out of the shared config layer so the enum + the
 * "real anchor only" rule live in one kb-owned place. */
#include "kb_vault_policy.h"
#include "modules/db2/c/db2_witness_checkpoint.h" /* anchor coverage + freshness (P2b gate) */
#include "kb/kb_witness_cadence.h"                /* last-verification-clean + freshness bound */
#include "vault_custody_mock.h"                   /* vault_custody_mock_provider */
#include "vault_custody_tpm2.h"                   /* vault_custody_tpm2_provider (real or stub) */
#include "vault_custody_pkcs11.h"
#include "vault_custody_kms.h"
#include "vault_internal.h"       /* vault_custody_set_provider */
#include "vault_server_key.h"     /* vault_is_sealed */
#include "vault_witness_signer.h" /* vault_witness_signer_identity (P2b gate) */
#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>

/* The custody kinds. `file` and `mock` are runnable; the three anchors parse but
 * fail closed at bind until their hardware-backed providers land. */
typedef enum
{
   KB_CUSTODY_FILE = 0, /* default */
   KB_CUSTODY_MOCK,     /* test/dev anchor — NEVER live-key-eligible */
   KB_CUSTODY_TPM2,     /* real anchor (deferred) */
   KB_CUSTODY_PKCS11,   /* real anchor (deferred) */
   KB_CUSTODY_KMS,      /* real anchor (deferred) */
   KB_CUSTODY_UNKNOWN,  /* not in the enum — reject */
} kb_custody_kind_t;

static kb_custody_kind_t g_selected = KB_CUSTODY_FILE;

static kb_custody_kind_t custody_parse(const char *custody)
{
   if (!custody || !custody[0] || strcmp(custody, "file") == 0)
      return KB_CUSTODY_FILE;
   if (strcmp(custody, "mock") == 0)
      return KB_CUSTODY_MOCK;
   if (strcmp(custody, "tpm2") == 0)
      return KB_CUSTODY_TPM2;
   if (strcmp(custody, "pkcs11") == 0)
      return KB_CUSTODY_PKCS11;
   if (strcmp(custody, "kms") == 0)
      return KB_CUSTODY_KMS;
   return KB_CUSTODY_UNKNOWN;
}

/* Is this kind a REAL external anchor (the only basis P7 §3 permits for a live
 * key)? file and mock are NOT — mock is test/dev only. */
static int custody_is_real_anchor(kb_custody_kind_t k)
{
   return k == KB_CUSTODY_TPM2 || k == KB_CUSTODY_PKCS11 || k == KB_CUSTODY_KMS;
}

int kb_vault_policy_select(const char *custody, char *errbuf, size_t errlen)
{
   kb_custody_kind_t k = custody_parse(custody);

   switch (k)
   {
   case KB_CUSTODY_FILE:
      /* Restore/keep the built-in file provider (already the default). */
      vault_custody_set_provider(NULL);
      g_selected = KB_CUSTODY_FILE;
      return 0;

   case KB_CUSTODY_MOCK:
      /* Bind the test/dev anchor: boots SEALED, exercises the seal barrier, but is
       * never a live-key basis (see kb_vault_live_keys_allowed). */
      vault_custody_set_provider(vault_custody_mock_provider());
      g_selected = KB_CUSTODY_MOCK;
      return 0;

   case KB_CUSTODY_TPM2:
      /* The tpm2 anchor (P7-tpm2a). Binds the real ESAPI provider on a WITH_TPM2
       * build, else a fail-closed stub — both boot SEALED. live_keys_allowed()
       * flips true only once a real (WITH_TPM2) provider is unsealed; the stub's
       * is_sealed() stays 1, so a libtss2-less build keeps live keys off. */
      vault_custody_set_provider(vault_custody_tpm2_provider());
      g_selected = KB_CUSTODY_TPM2;
      return 0;

   case KB_CUSTODY_PKCS11:
      vault_custody_set_provider(vault_custody_pkcs11_provider());
      g_selected = KB_CUSTODY_PKCS11;
      /* PKCS#11 login is the provider's explicit unseal operation.  Fail closed
       * during startup if the configured token/PIN/object cannot be opened. */
      if (vault_unseal(NULL, 0) != 0)
      {
         vault_custody_set_provider(NULL);
         g_selected = KB_CUSTODY_FILE;
         if (errbuf && errlen)
            snprintf(errbuf, errlen, "pkcs11 token unavailable or login failed");
         return -1;
      }
      return 0;
   case KB_CUSTODY_KMS:
      vault_custody_set_provider(vault_custody_kms_provider());
      g_selected = KB_CUSTODY_KMS;
      if (vault_unseal(NULL, 0) != 0)
      {
         vault_custody_set_provider(NULL);
         g_selected = KB_CUSTODY_FILE;
         if (errbuf && errlen)
            snprintf(errbuf, errlen, "kms helper unavailable or returned invalid root");
         return -1;
      }
      if (vault_custody_kms_hwm_refresh() != 0)
      {
         vault_seal();
         vault_custody_set_provider(NULL);
         g_selected = KB_CUSTODY_FILE;
         if (errbuf && errlen)
            snprintf(errbuf, errlen, "kms high-water mark unavailable or untrusted");
         return -1;
      }
      return 0;

   case KB_CUSTODY_UNKNOWN:
   default:
      if (errbuf && errlen)
         snprintf(errbuf, errlen,
                  "vault.custody '%s' is not a known value {file,mock,tpm2,pkcs11,kms}",
                  custody ? custody : "");
      return -1;
   }
}

int kb_vault_live_keys_allowed(void)
{
   /* Real anchor AND currently unsealed. file/mock -> false unconditionally. */
   return custody_is_real_anchor(g_selected) && vault_is_sealed() == 0 &&
          (g_selected != KB_CUSTODY_KMS || vault_custody_kms_hwm_ready());
}

int kb_vault_management_status_keys_allowed(void)
{
   return g_selected == KB_CUSTODY_KMS && vault_is_sealed() == 0 && vault_custody_kms_hwm_ready();
}

/* The P2b production egress gate, as the conjunction the umbrella (§4) requires.
 * Every term is fail-closed: any query that cannot run, or any term that does not
 * hold, returns 0 (egress stays closed). It returns 1 ONLY on a key-holding kb
 * (real, unsealed anchor) whose witnessing is healthy, so a dev / file-custody /
 * mock kb — where kb_vault_live_keys_allowed() is false — always gets 0.
 *
 * Checked on every egress request. The two DB reads (coverage, freshness) are
 * counts over the small checkpoint table and are cheap relative to the envelope
 * crypto the egress itself performs.
 *
 * Scope: this is a health/liveness gate layered on top of the primary defenses
 * (evidence commits atomically before key use; tampering is caught by external
 * comparison). On a kb with ZERO checkpoints terms 3-5 hold vacuously, so a fresh
 * kb is governed only by term 1 — deliberate, so the first egress is not deadlocked
 * before any evidence exists. Deleting an existing chain to reach that state
 * requires defeating the checkpoint WORM (an already-compromised kb, outside the
 * single-machine threat model) and self-corrects within one checkpoint interval;
 * the gate does not attempt to catch a fully-compromised kb, which is the external
 * comparison's job. */
static int witness_release_gate_open(void)
{
   /* 1. Live keys are allowed under the selected custody anchor (excludes file/mock
    *    and a sealed anchor). */
   if (!kb_vault_live_keys_allowed())
      return 0;

   /* 2. Witnessing is functional: the signing identity this kb would witness with
    *    is derivable. On a key-holding kb witnessing is non-disableable, so this is
    *    the observable "witnessing is active" signal. */
   uint8_t pub[VAULT_WITNESS_ED25519_PUB_LEN], key_id[VAULT_WITNESS_SIGNER_KEY_ID_LEN];
   if (vault_witness_signer_identity(pub, key_id) != 0)
      return 0;

   int open = 0;
   do
   {
      /* 3. The anchor set covers every retained checkpoint's signer_key_id: none is
       *    signed by a key this kb cannot derive (a foreign/revoked key). A coverage
       *    check that cannot run is treated as not-covered. */
      int64_t unknown = -1;
      if (db2_witness_checkpoint_anchor_coverage(key_id, sizeof key_id, &unknown, NULL, 0) != 0 ||
          unknown != 0)
         break;

      /* 4. The latest signed checkpoint is not older than the configured bound. A kb
       *    with zero checkpoints has not stalled (no evidence yet) and is not gated
       *    on this term; once a chain exists, a stale head closes the gate. */
      int64_t count = 0, age = 0;
      if (db2_witness_checkpoint_freshness(&count, &age) != 0)
         break;
      if (count > 0 && age > KB_WITNESS_CHECKPOINT_MAX_AGE_S)
         break;

      /* 5. Continuous verification's last result was clean (not-run and unproven both
       *    read as not-clean → fail-closed). */
      if (kb_witness_verification_last_clean() != 1)
         break;

      open = 1;
   } while (0);

   OPENSSL_cleanse(pub, sizeof pub);
   OPENSSL_cleanse(key_id, sizeof key_id);
   return open;
}

int kb_egress_release_allowed(void)
{
#if defined(AIMEE_P2B_INTEGRATION_TEST_OVERRIDE)
   /* The one conspicuous bypass: an integration-only build that skips the witness
    * health conjunction and gates on live keys alone. Never in a production build. */
   return kb_vault_live_keys_allowed();
#else
   return witness_release_gate_open();
#endif
}
