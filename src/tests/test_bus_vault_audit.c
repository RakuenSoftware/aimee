/* test_bus_vault_audit.c: the vault credential-access audit trail, END TO END
 * through the REAL server-side bridge (vault_audit_bridge.c) onto the audit event
 * bus and into the ledger.
 *
 * The unit test in test_vault_service.c pins vault_service -> hook (a capturing
 * stub). This pins the OTHER half the stub cannot: the actual server bridge's
 * field mapping. It installs vault_audit_bridge_install(), drives real vault ops,
 * drains the async bus, then reads the real ledger back and asserts each row's
 * actor/tool/command/mode/verdict — and, crucially, that NO secret plaintext
 * appears anywhere in the ledger. The (agent,cred) identity is correlated via the
 * human-readable `command` field (the correlation key for vault-access rows), not
 * via args_hash (which is tool-name-only for these non-allowlisted ops).
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audit_action.h" /* audit_ensure_key */
#include "audit_bus.h"
#include "audit_ledger.h"
#include "cJSON.h"
#include "log.h" /* audit_log_open */
#include "server/vault_audit_bridge.h"
#include "vault_crypto.h" /* VAULT_ROOT_KEY_LEN */
#include "vault_kek_cache.h"
#include "vault_service.h"

static const long T0 = 100000;

static void root_key(uint8_t rk[VAULT_ROOT_KEY_LEN], unsigned seed)
{
   for (int i = 0; i < VAULT_ROOT_KEY_LEN; i++)
      rk[i] = (uint8_t)(seed * 13 + i);
}

static const char *sval(cJSON *row, const char *key)
{
   const char *v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, key));
   return v ? v : "";
}

/* First ledger row whose (tool, command) match; NULL if none. */
static cJSON *find_row(cJSON *rows, const char *tool, const char *command)
{
   cJSON *r = NULL;
   cJSON_ArrayForEach(r, rows)
   {
      if (strcmp(sval(r, "tool"), tool) == 0 && strcmp(sval(r, "command"), command) == 0)
         return r;
   }
   return NULL;
}

/* First ledger row whose (tool, mode) match; NULL if none. */
static cJSON *find_row_mode(cJSON *rows, const char *tool, const char *mode)
{
   cJSON *r = NULL;
   cJSON_ArrayForEach(r, rows)
   {
      if (strcmp(sval(r, "tool"), tool) == 0 && strcmp(sval(r, "mode"), mode) == 0)
         return r;
   }
   return NULL;
}

int main(void)
{
   printf("test_bus_vault_audit:\n");

   char home[] = "/tmp/aimee-busvault-XXXXXX";
   if (!mkdtemp(home))
   {
      fprintf(stderr, "FAIL: tmp home\n");
      return 1;
   }
   setenv("AIMEE_HOME", home, 1);
   audit_log_open();   /* the consumer appends rows here */
   audit_ensure_key(); /* so args_hash is a real keyed digest, not the sentinel */
   vault_kek_cache_clear();

   /* Install the REAL bridge: vault access -> hook -> audit_bus -> ledger. */
   vault_audit_bridge_install();

   const char *p = "uid:7000";
   const char *sc = "sk-claude-DO-NOT-LOG-a1b2c3";
   const char *so = "sk-openai-DO-NOT-LOG-d4e5f6";
   uint8_t rk[VAULT_ROOT_KEY_LEN];
   root_key(rk, 7);
   char out[128];

   assert(vault_service_unlock(p, ATTEST_UDS_PEERCRED, rk, sizeof rk, T0) == VAULT_OK);
   assert(vault_service_set(p, "claude", "api_key", sc, T0) == VAULT_OK);
   assert(vault_service_set(p, "openai", "api_key", so, T0) == VAULT_OK);
   assert(vault_service_get(p, "claude", "api_key", out, sizeof out, T0) == VAULT_OK);
   assert(strcmp(out, sc) == 0); /* caller really receives the plaintext... */
   assert(vault_service_delete(p, "claude", "api_key") == VAULT_OK);
   assert(vault_service_unlock(p, ATTEST_TCP_BEARER, rk, sizeof rk, T0) == VAULT_ERR_TRANSPORT);

   audit_bus_stop(); /* drains every in-flight row into the ledger */

   cJSON *rows = audit_ledger_read(NULL, NULL);
   assert(rows);

   /* unlock (allow): actor=principal, command empty (whole-vault op), mode=uds. */
   cJSON *unlock_ok = find_row(rows, "vault.unlock", "");
   assert(unlock_ok);
   assert(strcmp(sval(unlock_ok, "actor"), p) == 0);
   assert(strcmp(sval(unlock_ok, "verdict"), "allow") == 0);
   assert(strcmp(sval(unlock_ok, "mode"), "uds") == 0);

   /* Two distinct credentials each get their OWN row, correlated by the
    * human-readable command identity (claude/api_key vs openai/api_key). */
   cJSON *set_c = find_row(rows, "vault.set", "claude/api_key");
   cJSON *set_o = find_row(rows, "vault.set", "openai/api_key");
   assert(set_c && set_o);
   assert(strcmp(sval(set_c, "actor"), p) == 0);
   assert(strcmp(sval(set_c, "verdict"), "allow") == 0);
   assert(strcmp(sval(set_c, "mode"), "n/a") == 0); /* get/set/delete carry no transport */

   /* get + delete of the claude cred: both allowed, both attributed to it. */
   cJSON *get_c = find_row(rows, "vault.get", "claude/api_key");
   assert(get_c && strcmp(sval(get_c, "verdict"), "allow") == 0);
   cJSON *del_c = find_row(rows, "vault.delete", "claude/api_key");
   assert(del_c && strcmp(sval(del_c, "verdict"), "allow") == 0);

   /* The wrong-transport unlock is recorded as a DENIAL over tcp — the security
    * signal that a root-key unlock was attempted on a disallowed channel. */
   cJSON *unlock_deny = find_row_mode(rows, "vault.unlock", "tcp");
   assert(unlock_deny);
   assert(strcmp(sval(unlock_deny, "verdict"), "deny") == 0);
   assert(strcmp(sval(unlock_deny, "reason_code"), "transport_not_allowed") == 0);

   /* THE invariant: no secret plaintext leaked into ANY field of ANY row. */
   char *dump = cJSON_PrintUnformatted(rows);
   assert(dump);
   assert(!strstr(dump, sc));
   assert(!strstr(dump, so));
   free(dump);

   cJSON_Delete(rows);
   vault_kek_cache_clear();
   printf("test_bus_vault_audit: OK (vault access -> bridge -> bus -> ledger; fields mapped, "
          "identity correlated by command, no secret leak)\n");
   return 0;
}
