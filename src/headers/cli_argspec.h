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
 * `"from": "argv_array"` emits every argv word as a JSON array of strings --
 * the shape the model and agent command families use to pass their arguments
 * through verbatim.
 *
 * `type` distinguishes the numeric conventions the same way, and there are
 * FOUR, not two: "number" refuses trailing garbage (3 sites), "number_lenient"
 * is atoi (53 sites), "number_lenient_int64" is atoll, "number_lenient_real" is
 * atof. Naming the wrong one is not cosmetic -- memory.delete shipped saying
 * atoi where its marshaller calls atoll, so an id above 2^31 would have been
 * truncated and addressed a different row. The differential test could not see
 * it, because its samples come from the spec and every id it generated was
 * small; scripts/check_argspec_numeric_parity.py compares the two directly.
 *
 * `default` carries the value a field takes when its flag is ABSENT, which is
 * what cli_args_get_int(opts, name, def) does. Absent is not the same as
 * present-but-empty: `--days ""` reaches atoi("") and yields 0, while omitting
 * --days yields the default.
 *
 * `min`/`max` clamp a number into a range (insights.overview pins --days into
 * [1, 365]). `from_end` counts a positional back from the last one, which is
 * how delegate.backend_exec takes its command. `alt_flag` is a second spelling
 * of the same flag: memory.get accepts --as-of and --as_of.
 *
 * `max_positionals` sits on the SPEC, not on a field: it refuses an invocation
 * carrying more positionals than the method accepts. delegate.log takes none,
 * and says so rather than dropping the id an operator typed. An arity rule is
 * not a field rule, but it crosses neither half of the line either -- no
 * field's presence depends on another field, and no branch decides which fields
 * exist.
 *
 * `count_min`/`count_max` gate a field on how many POSITIONALS were typed, and
 * `argc_min` on the raw argv count -- which is a different number: `skill pin
 * --x y` has argc 2 and pos_count 0, and the marshaller reads argv[0]. These
 * describe index.hybrid, which sends "query" for exactly one positional and
 * "queries" (an array) for more, and index.structure, which reads <project>
 * <file_path> for two and <file_path> alone for one.
 *
 * I refused that shape for most of this work as "a branch decides which fields
 * exist". That was the wrong line: this vocabulary is full of conditionals --
 * empty:"drop", omit_if_nonpositive, omit_below, max_positionals, first_of --
 * and none of them makes a spec a program. What would is a rule consulting
 * ANOTHER FIELD's value, or computing one.
 *
 * `equals`/`not_equals` test the field's own source value against a literal.
 * skill.lint never calls cli_args_parse: it compares raw argv[0] against
 * "--all" and sends `all` or `name` accordingly. That is the same kind of rule
 * as skip_if_dash, which tests the same value against a prefix.
 *
 * So: a field's rule may depend on its own value, its own flags, named client
 * facts the SERVER asked for, and the invocation's ARITY. It may not depend on
 * another field's value, and it may not compute one. That still forbids things:
 * skill.archive gates a field read from argv[2] on argv[1] matching a literal,
 * and is refused for exactly that reason.
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
