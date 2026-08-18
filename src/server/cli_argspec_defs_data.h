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
 * WHY THE REST ARE NOT HERE YET, from reading every remaining marshaller. The
 * blocker is not the spec language -- it is that the marshallers disagree with
 * each other, and a spec that reproduced each disagreement would stop being
 * data and start being a program:
 *
 *   - LENIENT NUMBERS. aux.test, dogfood.tag, api.enable and others parse with
 *     atoi()/cli_args_get_int(), so "12x" becomes 12 and "abc" becomes 0 or a
 *     default. This file's "number" refuses rather than coerces, deliberately
 *     (see kb.grant's team_id, where rounding an id would administer the wrong
 *     team). Serving these means fixing the coercion, not describing it.
 *   - MISSING ENVELOPE. eval.results, dogfood.tag and others build their body
 *     by hand and omit protocol_version, which every other request carries. The
 *     differential test catches it immediately, which is how it was found.
 *   - DERIVED FIELDS. catalog.show splits "provider:model" into two fields;
 *     workspace.add inverts --no-scan into "scan": false and nests part of its
 *     body under `args`.
 *   - EMPTY POSITIONALS. 45 positional sites gate on pos_count alone, so
 *     `aimee eval results ""` sends "suite": "" and filters on the empty
 *     string; a handful of others require non-empty and drop it. The spec's
 *     `positional` source follows the second, so it cannot describe the first
 *     without a flag that would enshrine a convention that looks like a bug.
 *     Found by the differential test on the first attempt to serve
 *     eval.results, which is exactly what it is for.
 *   - CROSS-FIELD RULES. cron.enable's --all is valid only for two of the five
 *     cron methods; trigger.fire accepts --task OR --proposal; dogfood.tag's
 *     --surprise and --no-surprise are exclusive. These are judgements about
 *     what the operator meant.
 *   - LOCAL STATE. eval.run, identity.snapshot and six others read getcwd() or
 *     the filesystem. These must NEVER be served, whatever the spec can express.
 *
 * So the remaining work is normalising the marshallers, which now has a safety
 * net: convert one, add its samples, and the differential test proves the
 * request body did not change.
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

