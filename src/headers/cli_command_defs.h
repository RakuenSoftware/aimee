/* cli_command_defs.h: the command catalogue served to the thin client.
 *
 * The client renders `aimee help` from what the server sends rather than from a
 * table compiled into it, so a command added server-side is discoverable from an
 * existing client. See server/cli_command_defs.c for why the data sits here.
 */
#ifndef DEC_CLI_COMMAND_DEFS_H
#define DEC_CLI_COMMAND_DEFS_H 1

#include <stddef.h>

struct cJSON;

/* Which listing a command appears in. CORE is what `aimee` with no arguments
 * shows; ADVANCED and ADMIN need `aimee help --all`. This is a presentation
 * hint, NOT an authorization boundary -- capability decides what may run. */
typedef enum
{
   AIMEE_CMD_TIER_CORE = 0,
   AIMEE_CMD_TIER_ADVANCED = 1,
   AIMEE_CMD_TIER_ADMIN = 2,
} aimee_cmd_tier_t;

typedef struct
{
   const char *name;
   const char *help;
   aimee_cmd_tier_t tier;
   /* Listed only under `aimee help --all`. Not secrecy: these are commands whose
    * audience is narrow enough that showing them by default costs every other
    * reader attention. */
   int hidden_default;
   const char *subcommands;
} cli_command_def_t;

/* The catalogue as a JSON array of
 * {name, summary, tier, hidden_default?, subcommands?}. Caller owns it.
 * NULL on allocation failure. */
struct cJSON *cli_command_defs_to_json(void);

size_t cli_command_defs_count(void);

#endif /* DEC_CLI_COMMAND_DEFS_H */
