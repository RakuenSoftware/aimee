/* cli_dispatch_defs.h: the CLI dispatch rows served to the thin client.
 *
 * `aimee <group> <verb>` -> method. Served (GET /v1/cli/manifest) so a command
 * added server-side is invokable from an existing client; see
 * server/cli_dispatch_defs.c for why the rows sit on this side of the wire.
 */
#ifndef DEC_CLI_DISPATCH_DEFS_H
#define DEC_CLI_DISPATCH_DEFS_H 1

#include <stddef.h>

struct cJSON;

/* The rows as a JSON array of
 * {cmd, sub?, method, server_method?, extract?, timeout_ms?}. An ABSENT `sub`
 * is the NULL wildcard (command takes no subcommand keyword); a present, empty
 * `sub` matches only when no subcommand was given. Caller owns the result.  */
struct cJSON *cli_dispatch_defs_to_json(void);

size_t cli_dispatch_defs_count(void);

#endif /* DEC_CLI_DISPATCH_DEFS_H */
