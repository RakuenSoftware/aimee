/* primary_session_release.c: giving back a primary-session row.
 *
 * db1_primary_session_row_t carries one member the store allocates -- the
 * messages document -- and these give it back. Neither reads a database nor
 * answers a query; they only free memory, which is why they live here rather
 * than in the module. The row crosses the module boundary now, so the
 * allocation a caller frees was made by the client on this side, and a copy of
 * these inside the module would be freeing an allocation it never made.
 *
 * The module needs neither: its stage releases what the domain allocated,
 * having written the values out first.
 */
#include <stdlib.h>
#include <string.h>

#include "primary_sessions.h"

void db1_primary_session_row_clear(db1_primary_session_row_t *row)
{
   if (!row)
      return;
   free(row->messages_json);
   memset(row, 0, sizeof(*row));
}

void db1_primary_session_rows_free(db1_primary_session_row_t *rows, int count)
{
   if (!rows)
      return;
   for (int i = 0; i < count; i++)
      free(rows[i].messages_json);
   free(rows);
}
