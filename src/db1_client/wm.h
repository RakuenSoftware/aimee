/* db1/wm.h: session-scoped key-value scratch space with TTL.
 *
 * Pure domain API. No backend types in any signature.
 *
 * The implementation used to be src/modules/db1/wm.c against SQLite. That went
 * with the C module: the family is served from
 * server-go/modules/aimee/families/conversation_wm.go against PostgreSQL, and
 * these declarations are the bus client for it. src/tests/support/wm_mock.c is
 * still the in-memory stand-in for tests.
 */
#ifndef DEC_DB1_WM_H
#define DEC_DB1_WM_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define WM_MAX_KEY_LEN    256
#define WM_MAX_VALUE_LEN  4096
#define WM_MAX_RESULTS    64
#define WM_DEFAULT_TTL    3600 /* 1 hour */
#define WM_SESSION_ID_LEN 128

   typedef struct
   {
      int64_t id;
      char session_id[128];
      char key[WM_MAX_KEY_LEN];
      char value[WM_MAX_VALUE_LEN];
      char category[64];
      char created_at[32];
      char updated_at[32];
      char expires_at[32];
   } wm_entry_t;

   /* Set a key-value pair. Inserts on first use, replaces on subsequent.
    * ttl_seconds: 0 means no expiry, >0 sets expires_at. Returns 0 on success. */
   int db1_wm_set(const char *session_id, const char *key, const char *value, const char *category,
                  int ttl_seconds);

   /* Get a value by key. Returns 0 on success, -1 if not found or expired. */
   int db1_wm_get(const char *session_id, const char *key, wm_entry_t *out);

   /* List entries for a session (optionally filtered by category). Returns count. */
   int db1_wm_list(const char *session_id, const char *category, wm_entry_t *out, int max);

   /* Search working-memory keys/values and return distinct session IDs. */
   int db1_wm_search_session_ids(const char *query, char out[][WM_SESSION_ID_LEN], int max);

   /* Delete a specific key. Returns 0 on success. */
   int db1_wm_delete(const char *session_id, const char *key);

   /* Clear all working memory for a session. Returns 0 on success. */
   int db1_wm_clear(const char *session_id);

   /* Remove expired entries across all sessions. Returns count removed. */
   int db1_wm_gc(void);

   /* Assemble working memory into a context string for the agent.
    * Returns malloc'd string (caller frees), or NULL if session has no entries. */
   char *db1_wm_assemble_context(const char *session_id);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_WM_H */
