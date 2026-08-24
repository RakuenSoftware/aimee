/* db1/local_operator.h: local machine credential-to-operator mapping.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_LOCAL_OPERATOR_H
#define DEC_DB1_LOCAL_OPERATOR_H 1

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_LOCAL_OPERATOR_SECRET_REF_LEN   256
#define DB1_LOCAL_OPERATOR_UUID_LEN         64
#define DB1_LOCAL_OPERATOR_DISPLAY_HINT_LEN 256
#define DB1_LOCAL_OPERATOR_TS_LEN           32

   typedef struct
   {
      char secret_ref[DB1_LOCAL_OPERATOR_SECRET_REF_LEN];
      char operator_uuid[DB1_LOCAL_OPERATOR_UUID_LEN];
      int active;
      char display_hint[DB1_LOCAL_OPERATOR_DISPLAY_HINT_LEN];
      char created_at[DB1_LOCAL_OPERATOR_TS_LEN];
   } db1_local_operator_t;

   int db1_local_operator_upsert(const char *secret_ref, const char *operator_uuid, int active,
                                 const char *display_hint);
   int db1_local_operator_get(const char *secret_ref, db1_local_operator_t *out);
   int db1_local_operator_get_active(db1_local_operator_t *out);
   int db1_local_operator_set_active(const char *secret_ref);
   int db1_local_operator_delete(const char *secret_ref);
   int db1_local_operator_list(db1_local_operator_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_LOCAL_OPERATOR_H */
