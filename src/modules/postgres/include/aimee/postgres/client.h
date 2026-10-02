#ifndef AIMEE_POSTGRES_CLIENT_H
#define AIMEE_POSTGRES_CLIENT_H
#include <stddef.h>
#include <stdint.h>

typedef struct aimee_postgres_session aimee_postgres_session_t;
typedef struct aimee_postgres_result aimee_postgres_result_t;
enum aimee_postgres_value_kind
{
   AIMEE_POSTGRES_NULL = 0,
   AIMEE_POSTGRES_TEXT = 1,
   AIMEE_POSTGRES_INT = 2,
   AIMEE_POSTGRES_FLOAT = 3,
   AIMEE_POSTGRES_BOOL = 4,
   AIMEE_POSTGRES_BYTES = 6
};
typedef struct
{
   enum aimee_postgres_value_kind kind;
   const void *data;
   size_t length;
   int64_t integer;
   double real;
} aimee_postgres_value_t;

/* No address or credential is accepted. The supervised PostgreSQL provider
 * owns the configured runtime connection and returns a scoped capability. */
aimee_postgres_session_t *aimee_postgres_session_open(char *error, size_t capacity);
/* Explicit offline authority: launches the PostgreSQL provider on a private
 * inherited pipe. Never falls back to host runtime credentials or vice versa. */
aimee_postgres_session_t *aimee_postgres_session_open_local(const char *credential, char *error,
                                                            size_t capacity);
/* Offline runtime authority, resolved by the provider from its fixed runtime
 * credential. Used by CLI tools before a supervised module bus exists. */
aimee_postgres_session_t *aimee_postgres_session_open_configured(char *error, size_t capacity);
/* Offline schema owner authority, resolved privately from the PostgreSQL
 * module's fixed migration credential. No caller-selected credential. */
aimee_postgres_session_t *aimee_postgres_session_open_migration(char *error, size_t capacity);
/* Restricted operator profile: verified TCP or a local Unix socket; no DNS
 * resolution without a numeric address pinned to the certificate identity. */
aimee_postgres_session_t *aimee_postgres_session_open_operator(const char *credential,
                                                               int64_t deadline_milliseconds,
                                                               char *error, size_t capacity);
/* Absolute CLOCK_MONOTONIC deadline. Zero restores the ordinary 60s cap. */
int aimee_postgres_session_deadline(aimee_postgres_session_t *session, int64_t milliseconds);
/* Reopen the same explicit authority; never substitute another profile. */
int aimee_postgres_session_reconnect(aimee_postgres_session_t *session, char *error,
                                     size_t capacity);
int aimee_postgres_session_close(aimee_postgres_session_t *session);
int aimee_postgres_session_discard(aimee_postgres_session_t *session);
typedef struct
{
   uint32_t capacity, in_use, waiters;
   uint64_t refused, expired, discarded;
} aimee_postgres_session_stats_t;
int aimee_postgres_session_stats(aimee_postgres_session_stats_t *stats);
char aimee_postgres_session_cached_state(const aimee_postgres_session_t *session);
int aimee_postgres_session_state(aimee_postgres_session_t *session, char *state);
int aimee_postgres_session_exec(aimee_postgres_session_t *session, const char *sql,
                                const aimee_postgres_value_t *args, size_t count, uint64_t *changed,
                                char sqlstate[6], char *error, size_t capacity);
aimee_postgres_result_t *aimee_postgres_session_query(aimee_postgres_session_t *session,
                                                      const char *sql,
                                                      const aimee_postgres_value_t *args,
                                                      size_t count, char sqlstate[6], char *error,
                                                      size_t capacity);
/* 1 notification, 0 timeout, -1 refusal. Waits on this session's LISTEN set. */
int aimee_postgres_session_wait(aimee_postgres_session_t *session, unsigned milliseconds);
void aimee_postgres_result_free(aimee_postgres_result_t *result);
size_t aimee_postgres_result_rows(const aimee_postgres_result_t *result);
size_t aimee_postgres_result_columns(const aimee_postgres_result_t *result);
uint64_t aimee_postgres_result_changes(const aimee_postgres_result_t *result);
const char *aimee_postgres_result_name(const aimee_postgres_result_t *result, size_t column);
uint32_t aimee_postgres_result_oid(const aimee_postgres_result_t *result, size_t column);
/* NULL represents SQL NULL; a present empty string has length zero. Results own
 * cell storage, including a trailing NUL for text callers. */
const void *aimee_postgres_result_cell(const aimee_postgres_result_t *result, size_t row,
                                       size_t column, size_t *length);
/* PostgreSQL binary representation for primitive operator protocol columns.
 * The result owns this storage; SQL NULL or a malformed value returns NULL. */
const void *aimee_postgres_result_binary(aimee_postgres_result_t *result, size_t row, size_t column,
                                         size_t *length);
#endif
