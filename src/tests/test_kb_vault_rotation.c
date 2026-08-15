#include "kb/kb_vault_rotation.h"
#include "modules/db2/c/org_vault_rotation.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static db2_vault_rotation_row_t g_row;
static uint64_t g_anchor;
static int g_cas_calls;
static int g_finalize_calls;
static int g_fail_finalize;
static int g_scope;

int kb_identity_key(const kb_principal_t *p, char *out, size_t cap)
{
   if (!p || !p->authenticated || p->kind != KB_PRIN_OWNER || cap < 6)
      return -1;
   snprintf(out, cap, "owner");
   return 0;
}

int db2_tenant_scope_begin(const kb_principal_t *p, int64_t team)
{
   assert(p && p->authenticated && team == 7 && !g_scope);
   g_scope = 1;
   return 0;
}

int db2_tenant_scope_commit(void)
{
   assert(g_scope);
   g_scope = 0;
   return 0;
}

void db2_tenant_scope_rollback(void)
{
   assert(g_scope);
   g_scope = 0;
}

int vault_hwm_read(const char *key_id, uint64_t *version, uint8_t *att, size_t cap, size_t *len)
{
   assert(!g_scope); /* external anchor I/O is never inside a PG transaction */
   if (!key_id || strcmp(key_id, g_row.key_id) || cap < 4)
      return -1;
   *version = g_anchor;
   memset(att, 0x11, 4);
   *len = 4;
   return 0;
}

int vault_hwm_cas(const char *key_id, uint64_t expected, uint64_t next, uint8_t *att, size_t cap,
                  size_t *len)
{
   assert(!g_scope); /* external anchor I/O is never inside a PG transaction */
   g_cas_calls++;
   if (!key_id || strcmp(key_id, g_row.key_id) || expected != g_anchor || next != expected + 1 ||
       cap < 4)
      return -1;
   g_anchor = next;
   memset(att, 0x22, 4);
   *len = 4;
   return 0;
}

int db2_vault_rotation_start(const char *actor, const char *key_id, const char *principal,
                             int has_team, int64_t team_id, const char *agent, const char *cred,
                             int64_t from_version, int compromise, int64_t *out_id)
{
   (void)actor;
   assert(g_scope);
   memset(&g_row, 0, sizeof(g_row));
   g_row.id = 9;
   g_row.has_team = has_team;
   g_row.team_id = team_id;
   g_row.from_version = from_version;
   g_row.to_version = from_version + 1;
   g_row.compromise = compromise;
   snprintf(g_row.key_id, sizeof(g_row.key_id), "%s", key_id);
   snprintf(g_row.principal, sizeof(g_row.principal), "%s", principal);
   snprintf(g_row.agent, sizeof(g_row.agent), "%s", agent);
   snprintf(g_row.cred, sizeof(g_row.cred), "%s", cred);
   snprintf(g_row.state, sizeof(g_row.state), "provision");
   *out_id = g_row.id;
   return 0;
}

int db2_vault_rotation_stage(const char *actor, int64_t id, const uint8_t *wrapped, size_t wn,
                             const uint8_t *nonce, size_t nn, const uint8_t *ct, size_t cn,
                             const uint8_t *tag, size_t tn, int64_t *version)
{
   (void)actor;
   assert(g_scope);
   if (id != g_row.id || !wrapped || !wn || !nonce || !nn || !ct || !cn || !tag || !tn)
      return -1;
   snprintf(g_row.state, sizeof(g_row.state), "staged");
   *version = g_row.to_version;
   return 0;
}

int db2_vault_rotation_transition(const char *actor, int64_t id, const char *expected,
                                  const char *next, const char *error)
{
   (void)actor;
   (void)error;
   assert(g_scope);
   if (id != g_row.id || strcmp(g_row.state, expected))
      return -1;
   snprintf(g_row.state, sizeof(g_row.state), "%s", next);
   return 0;
}

int db2_vault_rotation_finalize(const char *actor, int64_t id, const uint8_t *att, size_t len)
{
   (void)actor;
   assert(g_scope);
   g_finalize_calls++;
   if (g_fail_finalize || id != g_row.id || !att || !len)
      return -1;
   snprintf(g_row.state, sizeof(g_row.state), "activated");
   return 0;
}

int db2_vault_rotation_get(int64_t id, db2_vault_rotation_row_t *out)
{
   assert(g_scope);
   if (id != g_row.id)
      return -1;
   *out = g_row;
   return 0;
}

int main(void)
{
   const kb_principal_t caller = {.kind = KB_PRIN_OWNER, .authenticated = 1};
   assert(kb_vault_rotation_classify(4, 4, 5) == KB_VAULT_ROTATION_RETRY_CAS);
   assert(kb_vault_rotation_classify(5, 4, 5) == KB_VAULT_ROTATION_FINALIZE);
   assert(kb_vault_rotation_classify(3, 4, 5) == -1);
   assert(kb_vault_rotation_classify(5, UINT64_MAX, 0) == -1);

   snprintf(g_row.key_id, sizeof(g_row.key_id), "team:7|bedrock|primary");
   g_anchor = 3;
   int64_t id = 0;
   assert(kb_vault_rotation_start(&caller, 7, g_row.key_id, "team:7", "bedrock", "primary", 4, 0,
                                  &id) == -1);
   g_anchor = 4;
   assert(kb_vault_rotation_start(&caller, 7, g_row.key_id, "team:7", "bedrock", "primary", 4, 0,
                                  &id) == 0 &&
          id == 9);
   const uint8_t b[] = {1, 2, 3};
   assert(kb_vault_rotation_stage(&caller, 7, id, b, sizeof(b), b, sizeof(b), b, sizeof(b), b,
                                  sizeof(b)) == 0);
   assert(kb_vault_rotation_mark_probed(&caller, 7, id) == 0);

   /* Crash window: CAS succeeds, DB finalize fails. Resume reads anchor N+1 and
    * finalizes without issuing a second CAS. */
   g_fail_finalize = 1;
   assert(kb_vault_rotation_activate_or_resume(&caller, 7, id) == -1);
   assert(g_anchor == 5 && g_cas_calls == 1 && g_finalize_calls == 1);
   g_fail_finalize = 0;
   assert(kb_vault_rotation_activate_or_resume(&caller, 7, id) == KB_VAULT_ROTATION_COMPLETE);
   assert(g_cas_calls == 1 && g_finalize_calls == 2);
   assert(kb_vault_rotation_activate_or_resume(&caller, 7, id) == KB_VAULT_ROTATION_COMPLETE);

   snprintf(g_row.state, sizeof(g_row.state), "probed");
   g_anchor = 7;
   assert(kb_vault_rotation_activate_or_resume(&caller, 7, id) == -1);
   puts("PASS: kb vault rotation recovery");
   return 0;
}
