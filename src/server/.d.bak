/* cli_argspec_defs_data.h: the argument specs served to the thin client.
 *
 * Bare initializer rows of {method, spec}, included by BOTH the server emitter
 * (server/cli_argspec_defs.c, which serves them) and the differential test
 * (tests/test_cli_argspec.c, which proves each one builds the same request body
 * the compiled marshaller does). Included by both on purpose: a test that
 * carried its own copy of the specs would prove a spec that is not the one
 * shipped, which is the failure mode this whole exercise exists to avoid.
 *
 * The spec vocabulary is in headers/cli_argspec.h. A method appears here only
 * once its samples in the differential test pass; the test refuses a row it has
 * no samples for, so a spec cannot ship unproven.
 *
 * NOT here, and never: anything reading the client's own disk or environment.
 * That is the thin client's own job, not knowledge of what the server can do.
 *
 * WHY THE REST ARE NOT HERE YET, from reading every remaining marshaller.
 *
 * This list has been WRONG twice, in the same way, and the corrections are the
 * useful part. Both times I called a convention un-describable and treated the
 * marshallers as the thing to change; both times counting showed the spec was
 * the odd one out and the honest fix was to describe what the code does:
 *
 *   - EMPTY POSITIONALS. 81 sites gate on pos_count alone, so
 *     `aimee eval results ""` sends "suite": ""; 2 also require non-empty. I
 *     said a flag for the first "would enshrine a convention that looks like a
 *     bug". A vocabulary expressing 2 of 83 sites is not principled, it is
 *     incomplete -- so `"empty": "emit"`, and six methods followed.
 *   - LENIENT NUMBERS. 53 sites parse with atoi()/cli_args_get_int(), so "12x"
 *     is 12 and "abc" is 0; 3 refuse trailing garbage. Same shape, same
 *     correction: "number_lenient" plus "default", and three more methods.
 *
 * Whether those 81 and 53 sites SHOULD behave that way is a live question and
 * NOT settled here. kb.grant is the argument that the numbers should refuse:
 * its team_id is strict because "770001x" becoming 770001 would administer a
 * team the operator did not type. Changing them is a change to the CLI. This
 * file describes the CLI.
 *
 * What is left is left for reasons that are not counting errors:
 *
 *   - CONDITIONALS. api.enable emits `port` only when > 0; dogfood.tag's
 *     --surprise and --no-surprise are exclusive; trigger.fire takes --task OR
 *     --proposal; cron's --all is valid for two of its five methods. A spec
 *     language that grew these would stop being data and become a program
 *     shipped over the wire, which is the one line this vocabulary will not
 *     cross.
 *   - DERIVED FIELDS. catalog.show splits "provider:model" in two; cron.add and
 *     mcp.call compute fields; memory.user_capture joins the positionals from
 *     index 1 and prefixes the key; workspace.add inverts --no-scan into
 *     "scan": false and nests part of its body under `args`.
 *   - LOCAL STATE. eval.run, identity.snapshot, identity.diff, vault.unlock and
 *     delegate.launch read the client's own disk, environment or key material.
 *     These must NEVER be served, whatever the vocabulary can express.
 *   - RAW ARGV. provider.set reads argv[0] rather than a parsed positional, so
 *     `provider set --json openai` sends name="--json". Describing that would
 *     enshrine semantics that look like a bug, and the counting argument above
 *     does not apply to a sample of one: it is 1 site, not 81.
 *
 * Adding a method: write the spec, add samples INCLUDING the awkward input for
 * whatever convention it uses (empty string, non-numeric), and let the
 * differential test decide. It compares rendered JSON against the real
 * marshaller, so a spec that is merely plausible fails.
 */

{"provider.list",
 "{\"bool_flags\":[\"available\",\"all\",\"json\"],"
 "\"fields\":["
 "{\"json\":\"available_only\",\"from\":\"flag\",\"flag\":\"available\",\"type\":\"true_if_set\"},"
 "{\"json\":\"all\",\"from\":\"flag\",\"flag\":\"all\",\"type\":\"true_if_set\"},"
 "{\"json\":\"json\",\"from\":\"flag\",\"flag\":\"json\",\"type\":\"true_if_set\"}]}"},

{"provider.models",
 "{\"bool_flags\":[\"json\"],"
 "\"fields\":["
 "{\"json\":\"name\",\"from\":\"positional\",\"index\":0},"
 "{\"json\":\"json\",\"from\":\"flag\",\"flag\":\"json\",\"type\":\"true_if_set\"}]}"},

{"catalog.list",
 "{\"bool_flags\":[\"json\",\"open-weights\"],"
 "\"fields\":["
 "{\"json\":\"capability\",\"from\":\"flag\",\"flag\":\"capability\"},"
 "{\"json\":\"json\",\"from\":\"flag\",\"flag\":\"json\",\"type\":\"true_if_set\"},"
 "{\"json\":\"open_weights_only\",\"from\":\"flag\",\"flag\":\"open-weights\","
 "\"type\":\"bool\"}]}"},

{"trigger.list",
 "{\"fields\":[{\"json\":\"status\",\"from\":\"flag\",\"flag\":\"status\"}]}"},

{"trigger.status",
 "{\"usage\":\"usage: aimee trigger status <id>\","
 "\"fields\":[{\"json\":\"id\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"id\","
 "\"required\":true}]}"},

{"trigger.cancel",
 "{\"usage\":\"usage: aimee trigger cancel <id>\","
 "\"fields\":[{\"json\":\"id\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"id\","
 "\"required\":true}]}"},

{"model.episodes",
 "{\"fields\":[{\"json\":\"agent\",\"from\":\"positional_or_flag\",\"index\":0,"
 "\"flag\":\"agent\"}]}"},

{"graph.sync_code",
 "{\"fields\":[{\"json\":\"project\",\"from\":\"positional\",\"index\":0}]}"},

{"dogfood.report",
 "{\"fields\":[{\"json\":\"month\",\"from\":\"flag\",\"flag\":\"month\"},"
 "{\"json\":\"dir\",\"from\":\"flag\",\"flag\":\"dir\"}]}"},

/* These gate on pos_count alone, so an empty argument is SENT -- the
   "empty":"emit" convention. Servable only once the spec could say that; before
   it could, each of these disagreed with its marshaller on exactly one sample. */

{"eval.results",
 "{\"fields\":[{\"json\":\"suite\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"}]}"},

{"cert.revoke",
 "{\"fields\":[{\"json\":\"serial\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"}]}"},

{"vault.capability",
 "{\"fields\":[{\"json\":\"action\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"principal\",\"from\":\"positional\",\"index\":1,\"empty\":\"emit\"}]}"},

{"vault.delete",
 "{\"fields\":[{\"json\":\"agent\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"cred\",\"from\":\"positional\",\"index\":1,\"empty\":\"emit\"}]}"},

{"vault.set",
 "{\"fields\":[{\"json\":\"agent\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"cred\",\"from\":\"positional\",\"index\":1,\"empty\":\"emit\"},{\"json\":\"secret\",\"from\":\"positional\",\"index\":2,\"empty\":\"emit\"}]}"},

{"vault.set_server",
 "{\"fields\":[{\"json\":\"agent\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"cred\",\"from\":\"positional\",\"index\":1,\"empty\":\"emit\"},{\"json\":\"secret\",\"from\":\"positional\",\"index\":2,\"empty\":\"emit\"}]}"},

/* The lenient-number family. Each parses with atoi()/cli_args_get_int(), so a
   non-numeric argument becomes 0 rather than a refusal -- described with
   "number_lenient", and sampled with a non-numeric value so the description is
   proven rather than assumed. */

{"graph.explain",
 "{\"fields\":[{\"json\":\"entity\",\"from\":\"positional\",\"index\":0,"
 "\"empty\":\"emit\"},"
 "{\"json\":\"limit\",\"from\":\"flag\",\"flag\":\"limit\","
 "\"type\":\"number_lenient\",\"default\":40}]}"},

{"aux.test",
 "{\"fields\":[{\"json\":\"task\",\"from\":\"positional\",\"index\":0,"
 "\"empty\":\"emit\"},"
 "{\"json\":\"prompt\",\"from\":\"positional\",\"index\":1,\"empty\":\"emit\"},"
 "{\"json\":\"max_tokens\",\"from\":\"positional\",\"index\":2,"
 "\"type\":\"number_lenient\",\"empty\":\"emit\"}]}"},

{"dogfood.review",
 "{\"bool_flags\":[\"json\"],"
 "\"fields\":[{\"json\":\"month\",\"from\":\"flag\",\"flag\":\"month\"},"
 "{\"json\":\"dir\",\"from\":\"flag\",\"flag\":\"dir\"},"
 "{\"json\":\"limit\",\"from\":\"flag\",\"flag\":\"limit\","
 "\"type\":\"number_lenient\"}]}"},

