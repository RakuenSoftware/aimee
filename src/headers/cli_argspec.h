/* cli_argspec.h: build a request body from a SERVED argument spec.
 *
 * The manifest already tells the thin client which commands exist, how they
 * route, and which take no arguments. What it could not say is how the words
 * after the command become fields, so a new command with arguments still
 * needed a new client — the last thing in the chain the server could not
 * describe.
 *
 * This interprets a spec that says it. The vocabulary is deliberately small,
 * because it was read off the marshallers that exist rather than designed for
 * ones that might: a bool-flag list for the parser, then an ordered list of
 * fields, each naming where its value comes from and what JSON it becomes.
 *
 *   {"method": "catalog.list",
 *    "args": {"bool_flags": ["json", "open-weights"],
 *             "fields": [
 *               {"json": "capability",        "from": "flag",       "flag": "capability"},
 *               {"json": "json",              "from": "flag",       "flag": "json",
 *                "type": "true_if_set"},
 *               {"json": "open_weights_only", "from": "flag",       "flag": "open-weights",
 *                "type": "bool"},
 *               {"json": "name",              "from": "positional", "index": 0,
 *                "empty": "emit"}]}}
 *
 * `empty` says whether a present-but-empty value is sent ("emit") or dropped
 * ("drop", the default). Both are real: 81 positional sites send it, 2 drop it.
 *
 * What it deliberately CANNOT express: reading the client's filesystem or
 * environment, composing prompts, or cross-field rules like "either --task or
 * --proposal". The first two are the thin client's own job and must not move
 * server-side at all; the third is a judgement about what the user meant, and a
 * spec language that grew conditionals would become a program transmitted over
 * the wire. Those methods keep their compiled marshaller, and this returns NULL
 * for anything it cannot build faithfully rather than sending a body it guessed.
 */
#ifndef DEC_CLI_ARGSPEC_H
#define DEC_CLI_ARGSPEC_H 1

struct cJSON;

/* Build the request body for `method` from `spec` (the "args" object of one
 * manifest marshal row) and the command's argv.
 *
 * Returns a new cJSON object carrying method + protocol_version + the spec's
 * fields, or NULL when the spec is unusable or a required field is missing. On
 * a missing required field it prints the spec's `usage` line to stderr, so a
 * served command misuses exactly as loudly as a compiled one. Caller owns the
 * result. */
struct cJSON *cli_argspec_build(const char *method, const struct cJSON *spec, int argc,
                                char **argv);

/* True when `spec` is an object this interpreter can build from. A spec naming
 * a source or type it does not know is refused WHOLE rather than partially
 * honoured: half a request body is a wrong request, not a degraded one. */
int cli_argspec_supported(const struct cJSON *spec);

/* Build `method`'s body from the SERVED spec, if the server sent one.
 *
 * Returns 1 when the served spec is authoritative — *out holds the request, or
 * is NULL because a required argument was missing and the usage line has
 * already been printed. Returns 0 to fall through to the compiled marshaller:
 * no spec was served, or it named something this build cannot interpret. */
int cli_argspec_try_served(const char *method, int argc, char **argv, struct cJSON **out);

#endif /* DEC_CLI_ARGSPEC_H */
