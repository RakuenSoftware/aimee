/* cli_argspec_defs.h: the argument specs the server publishes in the CLI
 * manifest, alongside the "this method takes no arguments" rows.
 *
 * See headers/cli_argspec.h for the spec vocabulary and what it deliberately
 * cannot express.
 */
#ifndef DEC_CLI_ARGSPEC_DEFS_H
#define DEC_CLI_ARGSPEC_DEFS_H 1

#include <stddef.h>

struct cJSON;

/* JSON array of {method, args}, where `args` is a spec OBJECT. Merged into the
 * manifest's `marshal` list beside the {method, args:"none"} rows. Caller owns
 * the result. */
struct cJSON *cli_argspec_defs_to_json(void);

size_t cli_argspec_defs_count(void);

#endif /* DEC_CLI_ARGSPEC_DEFS_H */
