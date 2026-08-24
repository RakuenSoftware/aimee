#ifndef DEC_DB1_WRITE_H
#define DEC_DB1_WRITE_H 1

struct sqlite3;

/* Increment the write counter; triggers a PASSIVE WAL checkpoint every 50
 * successful commits to prevent the WAL from growing unbounded.  PASSIVE is
 * non-blocking — it skips checkpointing when any reader holds a WAL frame. */
void db1_maybe_checkpoint(struct sqlite3 *db);

#endif /* DEC_DB1_WRITE_H */
