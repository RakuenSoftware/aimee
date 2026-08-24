/* db1/project_clones.h: local checkout path to shared project identity mapping.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_PROJECT_CLONES_H
#define DEC_DB1_PROJECT_CLONES_H 1

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_PROJECT_CLONE_PATH_LEN 1024
#define DB1_PROJECT_UUID_LEN       64
#define DB1_PROJECT_URL_LEN        1024
#define DB1_PROJECT_TS_LEN         32

   typedef struct
   {
      char clone_path[DB1_PROJECT_CLONE_PATH_LEN];
      char project_uuid[DB1_PROJECT_UUID_LEN];
      char canonical_url[DB1_PROJECT_URL_LEN];
      char origin_url[DB1_PROJECT_URL_LEN];
      char upstream_url[DB1_PROJECT_URL_LEN];
      char last_seen_at[DB1_PROJECT_TS_LEN];
   } db1_project_clone_t;

   int db1_project_clone_upsert(const char *clone_path, const char *project_uuid,
                                const char *canonical_url, const char *origin_url,
                                const char *upstream_url);
   int db1_project_clone_get(const char *clone_path, db1_project_clone_t *out);
   int db1_project_clone_delete(const char *clone_path);
   int db1_project_clone_list(db1_project_clone_t *out, int max);
   int db1_project_clone_list_by_project(const char *project_uuid, db1_project_clone_t *out,
                                         int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_PROJECT_CLONES_H */
