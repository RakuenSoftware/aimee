/* cli_marshal_defs.h: argument shapes served to the thin client.
 *
 * Today this carries only "this method takes no arguments", which is the part
 * of argument handling that is pure data. See server/cli_marshal_defs.c for
 * what is deliberately NOT served, and why some of it never should be.
 */
#ifndef DEC_CLI_MARSHAL_DEFS_H
#define DEC_CLI_MARSHAL_DEFS_H 1

#include <stddef.h>

struct cJSON;

/* JSON array of {method, args}. `args` is "none" today; the field exists so a
 * richer shape can be added without a wire break. Caller owns the result. */
struct cJSON *cli_marshal_defs_to_json(void);

size_t cli_marshal_defs_count(void);

#endif /* DEC_CLI_MARSHAL_DEFS_H */
