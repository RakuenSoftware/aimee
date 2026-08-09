/* test_db1_delegate_reservation.c: execution-key -> delegate job reservations.
 *
 * The reservation is what makes a re-run replay the delegate it already paid
 * for instead of launching a second one, so the cases that matter are the ones
 * where a wrong answer costs money or loses a paid-for job:
 *   1. A saved reservation is resolvable by its key, with its participant.
 *   2. An unknown key misses rather than resolving to job 0, which would
 *      replay a launch that never happened.
 *   3. Re-saving a key repoints it and resets cancel_attempts: the retry
 *      counter belongs to the current job, not to the key across launches.
 *   4. forget_if_matches will not erase a reservation that now names a newer
 *      job -- otherwise an older job's cleanup lets a third launch happen.
 *   5. A row with an unusable job id reads as a miss, not as a replayable job.
 *   6. Every operation tolerates the table being absent, because the Go control
 *      plane's migrations create it and a server may run without that plane.
 *   7. Key-algorithm upgrades adopt only a sole matching legacy reservation;
 *      grouped/parallel matches remain untouched rather than being guessed. */

#include <assert.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "delegate_reservation.h"

int db1_init(const char *path);
void db1_shutdown(void);
sqlite3 *db1_conn(void);

static char tmp_db_path[256];

static void unlink_siblings(void)
{
   char path[300];
   unlink(tmp_db_path);
   snprintf(path, sizeof(path), "%s-wal", tmp_db_path);
   unlink(path);
   snprintf(path, sizeof(path), "%s-shm", tmp_db_path);
   unlink(path);
}

static void setup_db(void)
{
   snprintf(tmp_db_path, sizeof(tmp_db_path), "/tmp/test_db1_dres_%d.sqlite", (int)getpid());
   unlink_siblings();
   assert(db1_init(tmp_db_path) == 0);
}

static void teardown_db(void)
{
   db1_shutdown();
   unlink_siblings();
}

/* The Go control plane owns this schema; mirror the columns C depends on. */
static void create_reservation_table(void)
{
   char *err = NULL;
   assert(sqlite3_exec(db1_conn(),
                       "CREATE TABLE lifecycle_delegate_job ("
                       " execution_key TEXT PRIMARY KEY,"
                       " job_id INTEGER NOT NULL,"
                       " work_item_id TEXT NOT NULL DEFAULT '',"
                       " participant_token TEXT NOT NULL DEFAULT '',"
                       " cancel_attempts INTEGER NOT NULL DEFAULT 0,"
                       " updated_at TEXT NOT NULL DEFAULT '')",
                       NULL, NULL, &err) == SQLITE_OK);
}

static int cancel_attempts_for(const char *key)
{
   sqlite3_stmt *stmt = NULL;
   assert(sqlite3_prepare_v2(db1_conn(),
                             "SELECT cancel_attempts FROM lifecycle_delegate_job"
                             " WHERE execution_key = ?",
                             -1, &stmt, NULL) == SQLITE_OK);
   sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int attempts = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return attempts;
}

static void test_saved_reservation_round_trips(void)
{
   int job_id = 0;
   char participant[128] = "";
   assert(db1_delegate_reservation_save("step-a", "item-1", 41, "token-a") == 0);
   assert(db1_delegate_reservation_get("step-a", &job_id, participant, sizeof(participant)) == 0);
   assert(job_id == 41);
   assert(strcmp(participant, "token-a") == 0);
}

static void test_unknown_key_misses(void)
{
   int job_id = -1;
   char participant[128] = "seeded";
   assert(db1_delegate_reservation_get("never-saved", &job_id, participant, sizeof(participant)) ==
          -1);
   assert(job_id == 0);
   /* A miss must not leave a stale participant readable from a prior call. */
   assert(participant[0] == '\0');
}

static void test_resave_repoints_and_resets_attempts(void)
{
   int job_id = 0;
   assert(db1_delegate_reservation_save("step-b", "item-1", 50, "token-old") == 0);
   assert(sqlite3_exec(db1_conn(),
                       "UPDATE lifecycle_delegate_job SET cancel_attempts = 3"
                       " WHERE execution_key = 'step-b'",
                       NULL, NULL, NULL) == SQLITE_OK);
   assert(cancel_attempts_for("step-b") == 3);

   assert(db1_delegate_reservation_save("step-b", "item-1", 51, "token-new") == 0);
   char participant[128] = "";
   assert(db1_delegate_reservation_get("step-b", &job_id, participant, sizeof(participant)) == 0);
   assert(job_id == 51);
   assert(strcmp(participant, "token-new") == 0);
   assert(cancel_attempts_for("step-b") == 0);
}

static void test_compare_delete_protects_a_newer_reservation(void)
{
   assert(db1_delegate_reservation_save("step-c", "item-1", 60, "token-c") == 0);
   /* The retry reserved a newer job under the same key. */
   assert(db1_delegate_reservation_save("step-c", "item-1", 61, "token-c2") == 0);

   /* The older job's cleanup must not erase the newer reservation. */
   assert(db1_delegate_reservation_forget_if_matches("step-c", 60) == 0);
   int job_id = 0;
   assert(db1_delegate_reservation_get("step-c", &job_id, NULL, 0) == 0);
   assert(job_id == 61);

   assert(db1_delegate_reservation_forget_if_matches("step-c", 61) == 1);
   assert(db1_delegate_reservation_get("step-c", &job_id, NULL, 0) == -1);
   /* Releasing an already-released reservation is terminal, not an error. */
   assert(db1_delegate_reservation_forget_if_matches("step-c", 61) == 0);
}

static void test_unusable_job_id_reads_as_a_miss(void)
{
   assert(sqlite3_exec(db1_conn(),
                       "INSERT INTO lifecycle_delegate_job"
                       " (execution_key, job_id, work_item_id, participant_token)"
                       " VALUES ('step-d', 0, 'item-1', 'token-d')",
                       NULL, NULL, NULL) == SQLITE_OK);
   int job_id = -1;
   assert(db1_delegate_reservation_get("step-d", &job_id, NULL, 0) == -1);
   assert(job_id == 0);
}

static void test_forget_is_idempotent(void)
{
   assert(db1_delegate_reservation_save("step-e", "", 70, "") == 0);
   assert(db1_delegate_reservation_forget("step-e") == 0);
   assert(db1_delegate_reservation_get("step-e", &(int){0}, NULL, 0) == -1);
   assert(db1_delegate_reservation_forget("step-e") == 0);
}

static void test_adopts_only_a_sole_legacy_key(void)
{
   const char *old_key = "wi-1:impl:2026-08-05 15:14:57:old-hash";
   const char *new_key = "wi-1:impl:2026-08-05 15:14:57:new-hash";
   assert(db1_delegate_reservation_save(old_key, "wi-1", 71, "token-71") == 0);
   int job_id = 0;
   char participant[128] = "";
   assert(db1_delegate_reservation_adopt_sole_legacy(new_key, "wi-1", &job_id, participant,
                                                     sizeof(participant)) == 0);
   assert(job_id == 71);
   assert(strcmp(participant, "token-71") == 0);
   assert(db1_delegate_reservation_get(old_key, &job_id, NULL, 0) == -1);
   assert(db1_delegate_reservation_get(new_key, &job_id, NULL, 0) == 0);
   assert(job_id == 71);

   const char *group_a = "wi-2:gate:v1:legacy-a";
   const char *group_b = "wi-2:gate:v1:legacy-b";
   const char *group_new = "wi-2:gate:v1:new-hash";
   assert(db1_delegate_reservation_save(group_a, "wi-2", 72, "seat-a") == 0);
   assert(db1_delegate_reservation_save(group_b, "wi-2", 73, "seat-b") == 0);
   assert(db1_delegate_reservation_adopt_sole_legacy(group_new, "wi-2", &job_id, participant,
                                                     sizeof(participant)) == -1);
   assert(db1_delegate_reservation_get(group_a, &job_id, NULL, 0) == 0);
   assert(job_id == 72);
   assert(db1_delegate_reservation_get(group_b, &job_id, NULL, 0) == 0);
   assert(job_id == 73);
   assert(db1_delegate_reservation_get(group_new, &job_id, NULL, 0) == -1);
}

static void test_rejects_unusable_arguments(void)
{
   int job_id = 0;
   assert(db1_delegate_reservation_get("", &job_id, NULL, 0) == -1);
   assert(db1_delegate_reservation_get("step-a", NULL, NULL, 0) == -1);
   assert(db1_delegate_reservation_save("step-f", "", 0, "") == -1);
   assert(db1_delegate_reservation_save("", "", 5, "") == -1);
   assert(db1_delegate_reservation_forget_if_matches("step-f", 0) == -1);
}

/* Without the Go control plane the table does not exist. Launching each time is
 * the correct degradation; failing every launch is not. */
static void test_absent_table_degrades_to_no_reservation(void)
{
   assert(sqlite3_exec(db1_conn(), "DROP TABLE lifecycle_delegate_job", NULL, NULL, NULL) ==
          SQLITE_OK);
   int job_id = -1;
   assert(db1_delegate_reservation_get("step-a", &job_id, NULL, 0) == -1);
   assert(job_id == 0);
   assert(db1_delegate_reservation_save("step-a", "item-1", 80, "token") == -1);
   /* Nothing is reserved, which is what the caller asked for. */
   assert(db1_delegate_reservation_forget("step-a") == 0);
   assert(db1_delegate_reservation_forget_if_matches("step-a", 80) == 0);
}

int main(void)
{
   printf("db1_delegate_reservation: ");
   setup_db();
   create_reservation_table();

   test_saved_reservation_round_trips();
   test_unknown_key_misses();
   test_resave_repoints_and_resets_attempts();
   test_compare_delete_protects_a_newer_reservation();
   test_unusable_job_id_reads_as_a_miss();
   test_forget_is_idempotent();
   test_adopts_only_a_sole_legacy_key();
   test_rejects_unusable_arguments();
   test_absent_table_degrades_to_no_reservation();

   teardown_db();
   printf("ok\n");
   return 0;
}
