/* session_credentials.c: see session_credentials.h.
 *
 * Fixed-size in-RAM table of sessions, each holding a small set of
 * {agent_name -> api_key} plus optional Codex OAuth creds. Mutex-guarded.
 * Secrets are zeroed on removal/eviction. Idle sessions past SESS_CRED_TTL_SECS
 * are reaped lazily on access, and a full table evicts the least-recently-used
 * session. Nothing here is ever written to disk.
 */
#include "session_credentials.h"
#include "cJSON.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define SESS_CRED_MAX_SESSIONS 256
#define SESS_CRED_MAX_KEYS     16
#define SESS_CRED_NAME_LEN     64
#define SESS_CRED_KEY_LEN      8192 /* OAuth JWTs run ~1-2 KB */
#define SESS_CRED_ACCT_LEN     128
#define SESS_CRED_SID_LEN      128
#define SESS_CRED_TTL_SECS     (12 * 3600)

typedef struct
{
   char name[SESS_CRED_NAME_LEN];
   char key[SESS_CRED_KEY_LEN];
} cred_entry_t;

typedef struct
{
   char session_id[SESS_CRED_SID_LEN];
   cred_entry_t keys[SESS_CRED_MAX_KEYS];
   int key_count;
   char codex_token[SESS_CRED_KEY_LEN];
   char codex_account[SESS_CRED_ACCT_LEN];
   long last_ts;
   int in_use;
} sess_creds_t;

static sess_creds_t g_sessions[SESS_CRED_MAX_SESSIONS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static long sess_now(void)
{
   return (long)time(NULL);
}

static void sess_zero(sess_creds_t *s)
{
   /* Zero secrets explicitly before reuse/free. */
   memset(s, 0, sizeof(*s));
}

/* Caller holds g_lock. Find an in-use, non-expired session by id. */
static sess_creds_t *sess_find(const char *session_id, long now)
{
   for (int i = 0; i < SESS_CRED_MAX_SESSIONS; i++)
   {
      if (!g_sessions[i].in_use)
         continue;
      if (now - g_sessions[i].last_ts > SESS_CRED_TTL_SECS)
      {
         sess_zero(&g_sessions[i]); /* reap expired */
         continue;
      }
      if (strcmp(g_sessions[i].session_id, session_id) == 0)
         return &g_sessions[i];
   }
   return NULL;
}

/* Caller holds g_lock. Find-or-create a session slot (evicts LRU when full). */
static sess_creds_t *sess_get_or_create(const char *session_id, long now)
{
   sess_creds_t *s = sess_find(session_id, now);
   if (s)
      return s;
   sess_creds_t *lru = NULL;
   for (int i = 0; i < SESS_CRED_MAX_SESSIONS; i++)
   {
      if (!g_sessions[i].in_use)
      {
         s = &g_sessions[i];
         break;
      }
      if (!lru || g_sessions[i].last_ts < lru->last_ts)
         lru = &g_sessions[i];
   }
   if (!s)
   {
      s = lru;
      sess_zero(s); /* evict LRU (zeroes its secrets) */
   }
   snprintf(s->session_id, sizeof(s->session_id), "%s", session_id);
   s->in_use = 1;
   s->key_count = 0;
   return s;
}

void session_creds_set(const char *session_id, const char *agent_name, const char *api_key)
{
   if (!session_id || !session_id[0] || !agent_name || !agent_name[0])
      return;
   pthread_mutex_lock(&g_lock);
   long now = sess_now();
   sess_creds_t *s = sess_get_or_create(session_id, now);
   s->last_ts = now;
   /* Find existing entry for this agent. */
   cred_entry_t *e = NULL;
   for (int i = 0; i < s->key_count; i++)
      if (strcmp(s->keys[i].name, agent_name) == 0)
      {
         e = &s->keys[i];
         break;
      }
   if (!api_key || !api_key[0])
   {
      /* Remove: zero the slot and compact. */
      if (e)
      {
         int idx = (int)(e - s->keys);
         memset(e, 0, sizeof(*e));
         for (int j = idx; j < s->key_count - 1; j++)
            s->keys[j] = s->keys[j + 1];
         memset(&s->keys[s->key_count - 1], 0, sizeof(s->keys[0]));
         s->key_count--;
      }
      pthread_mutex_unlock(&g_lock);
      return;
   }
   if (!e)
   {
      if (s->key_count >= SESS_CRED_MAX_KEYS)
      {
         pthread_mutex_unlock(&g_lock);
         return; /* per-session key cap reached */
      }
      e = &s->keys[s->key_count++];
      snprintf(e->name, sizeof(e->name), "%s", agent_name);
   }
   snprintf(e->key, sizeof(e->key), "%s", api_key);
   pthread_mutex_unlock(&g_lock);
}

void session_creds_set_codex(const char *session_id, const char *token, const char *account_id)
{
   if (!session_id || !session_id[0])
      return;
   pthread_mutex_lock(&g_lock);
   long now = sess_now();
   sess_creds_t *s = sess_get_or_create(session_id, now);
   s->last_ts = now;
   if (token && token[0])
      snprintf(s->codex_token, sizeof(s->codex_token), "%s", token);
   else
      memset(s->codex_token, 0, sizeof(s->codex_token));
   if (account_id && account_id[0])
      snprintf(s->codex_account, sizeof(s->codex_account), "%s", account_id);
   else
      memset(s->codex_account, 0, sizeof(s->codex_account));
   pthread_mutex_unlock(&g_lock);
}

int session_creds_ingest_json(const char *session_id, const char *json)
{
   if (!session_id || !session_id[0] || !json || !json[0])
      return -1;
   cJSON *root = cJSON_Parse(json);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      return -1;
   }
   int n = 0;
   cJSON *agents = cJSON_GetObjectItemCaseSensitive(root, "agents");
   if (cJSON_IsObject(agents))
   {
      cJSON *kv = NULL;
      cJSON_ArrayForEach(kv, agents)
      {
         if (cJSON_IsString(kv) && kv->string && kv->valuestring[0])
         {
            session_creds_set(session_id, kv->string, kv->valuestring);
            n++;
         }
      }
   }
   cJSON *ct = cJSON_GetObjectItemCaseSensitive(root, "codex_oauth_token");
   cJSON *ca = cJSON_GetObjectItemCaseSensitive(root, "codex_account_id");
   if (cJSON_IsString(ct) && ct->valuestring[0])
   {
      session_creds_set_codex(session_id, ct->valuestring,
                              cJSON_IsString(ca) ? ca->valuestring : NULL);
      n++;
   }
   cJSON_Delete(root);
   return n;
}

int session_creds_get(const char *session_id, const char *agent_name, char *out, size_t out_len)
{
   if (!session_id || !session_id[0] || !agent_name || !agent_name[0] || !out || out_len == 0)
      return 0;
   int found = 0;
   pthread_mutex_lock(&g_lock);
   sess_creds_t *s = sess_find(session_id, sess_now());
   if (s)
   {
      s->last_ts = sess_now();
      for (int i = 0; i < s->key_count; i++)
         if (strcmp(s->keys[i].name, agent_name) == 0 && s->keys[i].key[0])
         {
            snprintf(out, out_len, "%s", s->keys[i].key);
            found = 1;
            break;
         }
   }
   pthread_mutex_unlock(&g_lock);
   return found;
}

int session_creds_get_codex(const char *session_id, char *token, size_t token_len, char *account_id,
                            size_t account_id_len)
{
   if (token && token_len)
      token[0] = '\0';
   if (account_id && account_id_len)
      account_id[0] = '\0';
   if (!session_id || !session_id[0])
      return 0;
   int found = 0;
   pthread_mutex_lock(&g_lock);
   sess_creds_t *s = sess_find(session_id, sess_now());
   if (s && s->codex_token[0])
   {
      s->last_ts = sess_now();
      if (token && token_len)
         snprintf(token, token_len, "%s", s->codex_token);
      if (account_id && account_id_len)
         snprintf(account_id, account_id_len, "%s", s->codex_account);
      found = 1;
   }
   pthread_mutex_unlock(&g_lock);
   return found;
}

void session_creds_clear(const char *session_id)
{
   if (!session_id || !session_id[0])
      return;
   pthread_mutex_lock(&g_lock);
   sess_creds_t *s = sess_find(session_id, sess_now());
   if (s)
      sess_zero(s);
   pthread_mutex_unlock(&g_lock);
}
