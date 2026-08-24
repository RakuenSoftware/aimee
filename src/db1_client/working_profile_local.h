/* db1/working_profile_local.h: per-machine learned operator preferences.
 *
 * This DB1 subsystem stores the local working-profile signal stream plus
 * the currently committed value for each field. */
#ifndef DEC_DB1_WORKING_PROFILE_LOCAL_H
#define DEC_DB1_WORKING_PROFILE_LOCAL_H 1

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_WORKING_PROFILE_FIELD_LEN 64
#define DB1_WORKING_PROFILE_VALUE_LEN 256

   typedef struct
   {
      char field[DB1_WORKING_PROFILE_FIELD_LEN];
      char value[DB1_WORKING_PROFILE_VALUE_LEN];
      int observation_count;
      double score;
      char updated_at[32];
   } db1_working_profile_local_state_t;

   int db1_working_profile_local_observe(const char *field, const char *value, double confidence,
                                         const char *session_id, int threshold);
   int db1_working_profile_local_list(db1_working_profile_local_state_t *out, int max);
   int db1_working_profile_local_get(const char *field, db1_working_profile_local_state_t *out);
   int db1_working_profile_local_reset_field(const char *field);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_WORKING_PROFILE_LOCAL_H */
