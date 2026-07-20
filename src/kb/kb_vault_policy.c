/* kb_vault_policy.c: kb-only custody selection + P7 §3 live-key gate. See
 * kb_vault_policy.h. Owns the vault.custody enum and the fail-closed policy for
 * unimplemented anchors — kept out of the shared config layer so the enum + the
 * "real anchor only" rule live in one kb-owned place. */
#include "kb_vault_policy.h"
#include "vault_custody_mock.h" /* vault_custody_mock_provider */
#include "vault_custody_tpm2.h" /* vault_custody_tpm2_provider (real or stub) */
#include "vault_custody_pkcs11.h"
#include "vault_internal.h"     /* vault_custody_set_provider */
#include "vault_server_key.h"   /* vault_is_sealed */
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
      return 0;
   case KB_CUSTODY_KMS:
      /* Declared but unimplemented: fail closed — never silently fall back to a
       * plaintext/file root under a config that asked for an anchor. */
      if (errbuf && errlen)
         snprintf(errbuf, errlen,
                  "vault.custody '%s' not yet implemented; use file (dev) or mock (test)",
                  custody ? custody : "");
      return -1;

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
   return custody_is_real_anchor(g_selected) && vault_is_sealed() == 0;
}
