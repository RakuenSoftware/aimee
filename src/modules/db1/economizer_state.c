/* db1/economizer_state.c: per-conversation economizer state (context-paging S2c).
 *
 * Split out of checkpoints.c, which held two unrelated things: session
 * checkpoints, which are rows, and this, which is one opaque document per
 * session. They ride different wires -- checkpoints the fields wire, this the
 * keyed-blob one -- and one family cannot mix them, so they could not stay one
 * source and both be served.
 */
#include "checkpoints.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------- economizer state (context-paging S2c) */

/* Internal label. Not exposed: callers address this state by session id only, so they
 * cannot collide with it or read it back as an ordinary checkpoint. */
#define DB1_ECON_STATE_LABEL "economizer-state"

int db1_economizer_state_save(const char *session_id, const char *json)
{
   sqlite3 *db = db1_conn();
   if (!db || !session_id || !session_id[0] || !json)
      return -1;

   /* Replace rather than append: only the newest row is ever read, so keeping older
    * ones would grow the table for the length of a session and leave stale reducer
    * state behind after a crash. Delete-then-insert keeps exactly one row. */
   sqlite3_stmt *del = NULL;
   if (sqlite3_prepare_v2(db, "DELETE FROM checkpoints WHERE session_id = ? AND label = ?", -1,
                          &del, NULL) == SQLITE_OK)
   {
      sqlite3_bind_text(del, 1, session_id, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(del, 2, DB1_ECON_STATE_LABEL, -1, SQLITE_TRANSIENT);
      sqlite3_step(del);
      sqlite3_finalize(del);
   }

   return db1_checkpoint_insert(DB1_ECON_STATE_LABEL, session_id, 0, json, NULL);
}

int db1_economizer_state_load(const char *session_id, char *out, size_t out_sz)
{
   sqlite3 *db = db1_conn();
   if (!db || !session_id || !session_id[0] || !out || out_sz == 0)
      return -1;
   out[0] = '\0';

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT snapshot FROM checkpoints WHERE session_id = ? AND label = ?"
                            " ORDER BY id DESC LIMIT 1";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, DB1_ECON_STATE_LABEL, -1, SQLITE_TRANSIENT);

   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *txt = sqlite3_column_text(stmt, 0);
      if (txt)
      {
         size_t n = strlen((const char *)txt);
         /* Refuse a row that does not fit rather than returning a truncated prefix:
          * truncated JSON does not parse, and a caller treating that as "no state"
          * would silently lose the conversation's page table instead of learning the
          * row was too big. */
         if (n < out_sz)
         {
            memcpy(out, txt, n + 1);
            rc = 0;
         }
      }
   }
   sqlite3_finalize(stmt);
   return rc;
}
