/* Human terminal startup and native execution-home ownership. Model responses
 * and execution completion are admitted by the canonical host owners. */
#ifndef CLI_SESSION_H
#define CLI_SESSION_H
#include <time.h>
#include <stddef.h>
#define CLI_SESSION_NAME_MAX 128
#define CLI_SESSION_CMD_MAX  256

typedef struct
{
   char session_name[CLI_SESSION_NAME_MAX];
   char cli_cmd[CLI_SESSION_CMD_MAX];
   int reuse;
   time_t last_activity;
   int active;
   char *iso_home; /* Owned runtime home; destroy never follows cache symlinks. */
} cli_session_t;

int cli_session_create(cli_session_t *s, const char *session_name, const char *cli_cmd,
                       const char *work_dir, int reuse);
void cli_session_destroy(cli_session_t *s);
int cli_session_is_alive(const cli_session_t *s);
char *cli_session_make_name(const char *agent_name, const char *role);
void cli_session_set_isolated_home(cli_session_t *s, const char *home);
/* The actual canonical native factory supplies the held source/profile binding
 * and SDK identity. Opaque resume bytes remain partitioned by those owners. */
int cli_session_native_home(const char *vendor, const char *cwd, const char *sid,
                            const char *binding, const char *sdk_identity, char *home,
                            size_t capacity, cli_session_t *cleanup);
#endif
