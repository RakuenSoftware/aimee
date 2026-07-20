#ifndef DB2_SERVER_REGISTRY_H
#define DB2_SERVER_REGISTRY_H
#include <stdint.h>
typedef struct
{
   char server_id[128], cert_cn[256], mgmt_cert_cn[256], endpoint[512], status[32], health[128],
       version[64];
   int64_t team_id;
} db2_server_row_t;
int db2_server_registry_list(int64_t, db2_server_row_t *, int);
int db2_server_registry_heartbeat(const char *, const char *, const char *, const char *);
#endif
