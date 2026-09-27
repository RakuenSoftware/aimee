#ifndef AIMEE_POSTGRES_SESSION_INTERNAL_H
#define AIMEE_POSTGRES_SESSION_INTERNAL_H
#include <aimee/postgres/client.h>
#include <sys/types.h>
struct aimee_postgres_session
{
   uint64_t handle;
   char transaction_state;
   uint64_t deadline_ns;
   uint32_t local_policy;
   char *local_credential;
   int channel;
   pid_t child;
};
int aimee_postgres_local_start(aimee_postgres_session_t *session, const char *credential);
void aimee_postgres_local_stop(aimee_postgres_session_t *session);
int aimee_postgres_local_exchange(aimee_postgres_session_t *session, const void *request,
                                  uint32_t length, void *reply, uint32_t capacity,
                                  uint32_t *reply_length);
#endif
