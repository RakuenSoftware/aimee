/* cli_kb_smoke.h: `aimee kb smoke` — exercise a live kb from the client side. */
#ifndef CLI_KB_SMOKE_H
#define CLI_KB_SMOKE_H

typedef struct cJSON cJSON;

/* Returns 0 when every check passed or skipped, 1 when any check failed. */
int cli_kb_smoke(int argc, char **argv, int json_output);

/* Evaluate every check derivable from a kb.health payload. Pure: no I/O and no
 * network, so a test can drive it with canned payloads. Returns a heap cJSON array
 * of {check, outcome, detail} rows; the caller frees. Counts are optional out
 * params. |health| may be NULL, which is itself the unreachable case. */
cJSON *cli_kb_smoke_eval_health(cJSON *health, int *passed, int *failed, int *skipped);

#endif /* CLI_KB_SMOKE_H */
