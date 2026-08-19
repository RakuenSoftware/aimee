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
 * WHY THE REST ARE NOT HERE YET. 126 of the 181 CLI-reachable methods are
 * served (36 no-argument, 90 specs) -- 91% of the set that CAN be served. Every
 * one of the remaining 55 has a reason, and the reasons are of three kinds:
 *
 *   43  NEVER serveable -- the client's own state
 *    7  a field-SET branch: which fields exist depends on the input
 *    5  a transformation of a value, or an argv grammar of its own
 *
 * THE 43 ARE THE POINT, not a shortfall. They read getcwd(),
 * $AIMEE_SESSION_ID, the filesystem, or vault key material. A thin client
 * reading its own disk to build a request is doing its own job, not obeying the
 * server; serving them would be the defect this exercise exists to prevent.
 *
 * COUNT THE DENOMINATOR CAREFULLY. That total said 166 for most of this work,
 * because the extractor matched a verb of [a-z0-9_]+ and so skipped every
 * HYPHENATED verb (`graph sync-code`, `index blast-radius`, `vault set-server`)
 * and every group that is itself a command (`aimee use`, `aimee presence`,
 * `aimee git`). Fifteen methods were invisible, and nine of those had never been
 * classified at all -- a method the extractor never yields is a method no
 * downstream question ever asks about. An undercounted denominator makes
 * coverage look BETTER, which is why nothing complained. scripts/
 * report_cli_argspec_coverage.py now carries the corrected pattern and a note
 * saying which way this error flatters.
 *
 * THE LINE, stated once so the next addition can be judged against it: a
 * field's rule may depend on ITS OWN value and its own flags, and nothing else.
 * No field's presence may depend on another field, and no branch may decide
 * which fields exist. Everything admitted meets it -- empty:"drop",
 * omit_if_nonpositive, bool_inverted, tristate_flag, skip_if_dash,
 * repeated_flag, argv_index, from_end, min/max, alt_flag -- and these do not:
 *
 *   - cron.enable/disable send job_id OR all:true, never both.
 *   - delegate.status sends job_ids (array) or job_id (scalar) by count.
 *   - trigger.fire needs --source AND (--task OR --proposal).
 *   - pipeline.start splits --questions on "||".
 *   - catalog.show splits "provider:model" on a colon, truncating at 64 bytes.
 *   - the memory.user_capture family joins the positionals from index 1,
 *     prefixes the key, and refuses one over 512 characters.
 *   - memory.supersede parses raw argv with a grammar of its own: only the
 *     inline --flag=value form, skipping any word that starts with a dash. That
 *     is a second PARSER, not a field rule, and the spec describes fields.
 *
 * Encoding string surgery would make a spec a program transmitted over the
 * wire, which is the property that makes the served form safe to trust.
 *
 * TWO THAT WERE FILED WRONG, because "the generator could not read it" is not
 * the same reason as "the line forbids it", and this list conflated them:
 *
 *   - delegate.log was listed here as a rule about the invocation. It is one,
 *     and the line does not forbid it: an ARITY constraint makes no field's
 *     presence depend on another field and lets no branch decide which fields
 *     exist. It is now served, with `max_positionals`.
 *
 *   - get_help is still not served, but not on principle either. It is pure
 *     argv with no local state and no branch; it just wears the MCP tool-call
 *     envelope (method "help.get", a constant `tool`, a nested `arguments`,
 *     and no protocol_version). Serving it needs four mechanisms -- method
 *     override, envelope suppression, an unconditional constant, nested
 *     placement -- for ONE method, because the other four MCP-shaped methods
 *     are out for reasons of their own: git.verify, git.cli, index.ast_grep and
 *     index.blast_radius read $AIMEE_SESSION_ID or the cwd, and tool.call
 *     parses key=value pairs and raw JSON out of argv. Of those four
 *     mechanisms, `envelope: none` is the one worth refusing: every served spec
 *     currently produces a properly enveloped request, and an escape hatch used
 *     exactly once is a poor trade for that guarantee. This is a COST decision
 *     and should be revisited if a second method ever wants the same shape.
 *
 * ADDING A METHOD: write the spec, add samples INCLUDING the awkward input for
 * whatever convention it uses, and let test_cli_argspec decide -- it compares
 * the built body against the real marshaller, so a merely plausible spec fails.
 * When it does, the fix usually belongs in the GENERATOR: every mismatch in
 * this work traced back to reading the wrong part of the source -- the value
 * instead of the guard, a field-name list instead of the branch, a direct body
 * instead of its helpers, a verb pattern that could not spell "sync-code".
 *
 * AND NOTE WHAT THE TEST CANNOT SEE. Its samples are generated FROM the spec,
 * so a marshaller rule the spec omits is invisible to it. Two real defects hid
 * exactly there:
 *
 *   - memory.delete SHIPPED saying number_lenient (atoi) where the marshaller
 *     calls atoll(). Every generated id was small, so nothing ever disagreed.
 *
 *     It does not truncate to a large wrong number, which is what "truncated"
 *     first suggested. atoi() keeps the LOW 32 BITS AS A SIGNED INT, measured
 *     on a real appliance against the shipped binary:
 *
 *         2147483647 -> 2147483647      (the last id that survives)
 *         2147483648 -> -2147483648
 *         4294967296 -> 0
 *         4294967297 -> 1               <-- deletes memory 1
 *         8589934592 -> 0
 *
 *     So `aimee memory delete 4294967297` deleted memory 1. Not a different row
 *     in the abstract: a LOW-numbered, early, probably important one, because
 *     the wrap lands near zero.
 *
 *     A JSON number is a double, so ids above 2^53 cannot round-trip whatever
 *     parse the spec names -- 2^53+1 arrives as ...990. That ceiling is shared
 *     by the compiled marshaller and is not something a spec can fix; it is
 *     recorded here so the next person does not read atoll() as a promise of
 *     the full 64-bit range.
 *   - insights.overview showed that the interpreter conflated "flag absent"
 *     with "flag present but empty", silently dropping a default.
 *
 * Hence: adversarial samples (oversized values, flag-shaped words, a bare "--"),
 * both-sources samples (the two precedences differ ONLY when both are given),
 * numeric samples that straddle 2^31 and carry a fraction -- and, because
 * samples can only ever probe what someone thought to sample,
 * scripts/check_argspec_numeric_parity.py compares each spec's numeric type
 * against the parse its marshaller actually calls, rather than hoping an input
 * lands on the boundary. Each of these was plant-tested: a deliberate violation
 * was introduced and the check confirmed to FAIL on it, because a check that
 * has never failed is decoration.
 */

/* No fields at all, and a refusal if anything positional is typed: `aimee
   delegate log 42` means the operator wanted a JOB log, and silently dropping
   the 42 would show them the wrong thing. It cannot go in the no-argument list
   for exactly that reason -- that list accepts whatever it is given.

   bool_flags is deliberately absent even though --json exists: the marshaller
   passes NULL, so --json consumes the following token as its value, and a spec
   that declared it a boolean would leave that token as a positional and refuse
   an invocation the compiled path accepts. */
{"delegate.log",
 "{\"usage\":\"usage: aimee delegate log [--json]; for a background job log, "
 "use `aimee jobs logs <job_id>`\",\"max_positionals\":0}"},

{"session.presence",
 "{\"fields\":[{\"json\":\"owner\",\"from\":\"flag\",\"flag\":\"owner\"}]}"},

{"insights.overview",
 "{\"fields\":[{\"json\":\"days\",\"from\":\"flag\",\"flag\":\"days\","
 "\"type\":\"number_lenient\",\"default\":30,\"min\":1,\"max\":365,\"empty\":\"emit\"}]}"},

{"delegate.backend_exec",
 "{\"bool_flags\":[\"no-hibernate\"],\"fields\":["
 "{\"json\":\"backend\",\"from\":\"flag\",\"flag\":\"backend\",\"empty\":\"emit\"},"
 "{\"json\":\"task_id\",\"from\":\"flag\",\"flag\":\"task-id\",\"empty\":\"emit\"},"
 "{\"json\":\"image\",\"from\":\"flag\",\"flag\":\"image\",\"empty\":\"emit\"},"
 "{\"json\":\"host\",\"from\":\"flag\",\"flag\":\"host\",\"empty\":\"emit\"},"
 "{\"json\":\"no_hibernate\",\"from\":\"flag\",\"flag\":\"no-hibernate\","
 "\"type\":\"true_if_set\"},"
 "{\"json\":\"command\",\"from\":\"positional\",\"index\":0,\"from_end\":true,"
 "\"empty\":\"emit\"}]}"},

{"memory.get",
 "{\"fields\":[{\"json\":\"id\",\"from\":\"positional\",\"index\":0,"
 "\"type\":\"number_lenient_int64\",\"empty\":\"emit\"},"
 "{\"json\":\"as_of\",\"from\":\"flag\",\"flag\":\"as-of\",\"alt_flag\":\"as_of\"}]}"},

{"memory.embed",
 "{\"bool_flags\":[\"all\"],\"fields\":["
 "{\"json\":\"all\",\"from\":\"flag\",\"flag\":\"all\",\"type\":\"true_if_set\"},"
 "{\"json\":\"memory_id\",\"from\":\"positional\",\"index\":0,"
 "\"type\":\"number_lenient_real\",\"empty\":\"emit\"},"
 "{\"json\":\"version\",\"from\":\"flag\",\"flag\":\"version\",\"empty\":\"emit\"}]}"},

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

/* Generated from each marshaller and proven against it; see the test. */

{"config.get",
 "{\"fields\":[{\"json\":\"key\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"}]}"},
{"config.set",
 "{\"fields\":[{\"json\":\"key\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"value\",\"from\":\"positional\",\"index\":1,\"empty\":\"emit\"}]}"},
{"delegate.aggregate",
 "{\"bool_flags\":[\"json\"],\"fields\":[{\"json\":\"prompt\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"}]}"},
{"evidence.fidelity_retrieval_event",
 "{\"fields\":[{\"json\":\"turn_id\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"}]}"},
{"evidence.provenance_retrieval_event",
 "{\"fields\":[{\"json\":\"turn_id\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"}]}"},
{"evidence.trace_retrieval_event",
 "{\"fields\":[{\"json\":\"turn_id\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"}]}"},
{"index.scan",
 "{\"bool_flags\":[\"force\"],\"fields\":[{\"json\":\"name\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"root\",\"from\":\"positional\",\"index\":1,\"empty\":\"emit\"},{\"json\":\"force\",\"from\":\"flag\",\"flag\":\"force\",\"type\":\"true_if_set\"}]}"},
{"kb.build",
 "{\"bool_flags\":[\"force\"],\"fields\":[{\"json\":\"path\",\"from\":\"flag_or_positional\",\"flag\":\"path\",\"index\":0},{\"json\":\"project\",\"from\":\"flag_or_positional\",\"flag\":\"project\",\"index\":1},{\"json\":\"force\",\"from\":\"flag\",\"flag\":\"force\",\"type\":\"true_if_set\"},{\"json\":\"embedding_command\",\"from\":\"flag\",\"flag\":\"embed\",\"empty\":\"emit\"}]}"},
{"kb.ingest",
 "{\"bool_flags\":[\"force\"],\"fields\":[{\"json\":\"workspace\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"force\",\"from\":\"flag\",\"flag\":\"force\",\"type\":\"true_if_set\"},{\"json\":\"embedding_command\",\"from\":\"flag\",\"flag\":\"embed\"}]}"},
{"kb.status",
 "{\"fields\":[{\"json\":\"project\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"}]}"},
{"kb.update",
 "{\"fields\":[{\"json\":\"path\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"project\",\"from\":\"positional\",\"index\":1,\"empty\":\"emit\"},{\"json\":\"embedding_command\",\"from\":\"flag\",\"flag\":\"embed\"}]}"},

{"curator.implements",
 "{\"fields\":[{\"json\":\"topic\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"}]}"},

{"curator.synthesize",
 "{\"fields\":[{\"json\":\"topic\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"}]}"},

{"provider.quota",
 "{\"fields\":[{\"json\":\"name\",\"from\":\"positional\",\"index\":0}]}"},

{"provider.show",
 "{\"fields\":[{\"json\":\"name\",\"from\":\"positional\",\"index\":0}]}"},

{"provider.test",
 "{\"fields\":[{\"json\":\"name\",\"from\":\"positional\",\"index\":0}]}"},

/* The model/agent family: every argument passed through verbatim as an array,
   which is one shape shared by all of them. */

{"model.add",
 "{\"fields\":[{\"json\":\"args\",\"from\":\"argv_array\"}]}"},

{"model.disable",
 "{\"fields\":[{\"json\":\"args\",\"from\":\"argv_array\"}]}"},

{"model.enable",
 "{\"fields\":[{\"json\":\"args\",\"from\":\"argv_array\"}]}"},

{"model.episodes",
 "{\"fields\":[{\"json\":\"args\",\"from\":\"argv_array\"}]}"},

{"model.list",
 "{\"fields\":[{\"json\":\"args\",\"from\":\"argv_array\"}]}"},

{"model.local",
 "{\"fields\":[{\"json\":\"args\",\"from\":\"argv_array\"}]}"},

{"model.personas",
 "{\"fields\":[{\"json\":\"args\",\"from\":\"argv_array\"}]}"},

{"model.probe",
 "{\"fields\":[{\"json\":\"args\",\"from\":\"argv_array\"}]}"},

{"model.remove",
 "{\"fields\":[{\"json\":\"args\",\"from\":\"argv_array\"}]}"},

{"model.roles",
 "{\"fields\":[{\"json\":\"args\",\"from\":\"argv_array\"}]}"},

{"cert.issue",
 "{\"fields\":[{\"json\":\"cn\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"days\",\"from\":\"flag\",\"flag\":\"days\",\"type\":\"number_lenient\"}]}"},

{"job.cancel",
 "{\"fields\":[{\"json\":\"job_id\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"job-id\",\"type\":\"number_lenient\"},{\"json\":\"reason\",\"from\":\"flag\",\"flag\":\"reason\"}]}"},

{"job.status",
 "{\"fields\":[{\"json\":\"job_id\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"job-id\",\"type\":\"number_lenient\"},{\"json\":\"reason\",\"from\":\"flag\",\"flag\":\"reason\"}]}"},

{"jobs.cancel",
 "{\"fields\":[{\"json\":\"job_id\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"job-id\",\"type\":\"number_lenient\"},{\"json\":\"reason\",\"from\":\"flag\",\"flag\":\"reason\"}]}"},

{"jobs.logs",
 "{\"fields\":[{\"json\":\"job_id\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"job-id\",\"type\":\"number_lenient\"},{\"json\":\"reason\",\"from\":\"flag\",\"flag\":\"reason\"}]}"},

{"jobs.status",
 "{\"fields\":[{\"json\":\"job_id\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"job-id\",\"type\":\"number_lenient\"},{\"json\":\"reason\",\"from\":\"flag\",\"flag\":\"reason\"}]}"},

{"kb.reembed",
 "{\"bool_flags\":[\"confirm\",\"force\",\"dry-run\",\"clear-maintenance\"],\"fields\":[{\"json\":\"confirm\",\"from\":\"flag\",\"flag\":\"confirm\",\"type\":\"true_if_set\"},{\"json\":\"force\",\"from\":\"flag\",\"flag\":\"force\",\"type\":\"true_if_set\"},{\"json\":\"dry_run\",\"from\":\"flag\",\"flag\":\"dry-run\",\"type\":\"true_if_set\"},{\"json\":\"clear_maintenance\",\"from\":\"flag\",\"flag\":\"clear-maintenance\",\"type\":\"true_if_set\"},{\"json\":\"target_dim\",\"from\":\"flag\",\"flag\":\"target-dim\",\"type\":\"number_lenient\"}]}"},

{"rules.delete",
 "{\"fields\":[{\"json\":\"id\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"id\",\"type\":\"number_lenient\"}]}"},

{"job.start",
 "{\"fields\":[{\"json\":\"plan_id\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"plan-id\",\"type\":\"number_lenient\"},{\"json\":\"parallel\",\"from\":\"flag\",\"flag\":\"parallel\",\"type\":\"number_lenient\",\"default\":0,\"omit_if_nonpositive\":true}]}"},

{"session.list",
 "{\"fields\":[{\"json\":\"limit\",\"from\":\"flag\",\"flag\":\"limit\",\"type\":\"number_lenient\",\"default\":0,\"omit_if_nonpositive\":true}]}"},

{"curator.contradictions",
 "{\"fields\":[{\"json\":\"limit\",\"from\":\"flag\",\"flag\":\"limit\",\"type\":\"number_lenient\",\"default\":20}]}"},

{"job.list",
 "{\"fields\":[{\"json\":\"limit\",\"from\":\"flag\",\"flag\":\"limit\",\"type\":\"number_lenient\",\"default\":20}]}"},

{"jobs.list",
 "{\"fields\":[{\"json\":\"limit\",\"from\":\"flag\",\"flag\":\"limit\",\"type\":\"number_lenient\",\"default\":20}]}"},

{"notes.search",
 "{\"fields\":[{\"json\":\"query\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"query\",\"empty\":\"emit\"},{\"json\":\"limit\",\"from\":\"flag\",\"flag\":\"limit\",\"type\":\"number_lenient\",\"default\":20}]}"},

/* Four of the six methods marshal_cron_id serves. It branches on `method` in
   exactly one place -- `--all` is honoured only for cron.enable/cron.disable --
   which makes those two indescribable, not all six. No marshaller change. */

{"cron.show",
 "{\"usage\":\"usage: aimee cron show <id>\",\"fields\":[{\"json\":\"job_id\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"id\",\"required\":true}]}"},

{"cron.run",
 "{\"usage\":\"usage: aimee cron run <id>\",\"fields\":[{\"json\":\"job_id\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"id\",\"required\":true}]}"},

{"cron.remove",
 "{\"usage\":\"usage: aimee cron show <id>\",\"fields\":[{\"json\":\"job_id\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"id\",\"required\":true}]}"},

{"cron.history",
 "{\"usage\":\"usage: aimee cron history <id>\",\"fields\":[{\"json\":\"job_id\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"id\",\"required\":true},{\"json\":\"limit\",\"from\":\"flag\",\"flag\":\"limit\",\"type\":\"number_lenient\",\"default\":20}]}"},

/* marshal_pipeline_request, per method. It branches three times and every other
   method falls through to a shared tail, so each has a fixed shape of its own. */

{"pipeline.advance",
 "{\"fields\":[{\"json\":\"pipeline_id\",\"from\":\"positional\",\"index\":0,\"type\":\"number_lenient\"},{\"json\":\"artifact\",\"from\":\"flag\",\"flag\":\"artifact\"},{\"json\":\"artifact_hash\",\"from\":\"flag\",\"flag\":\"artifact-hash\"},{\"json\":\"repo_root\",\"from\":\"flag\",\"flag\":\"repo-root\"},{\"json\":\"remote\",\"from\":\"flag\",\"flag\":\"remote\"},{\"json\":\"head_branch\",\"from\":\"flag\",\"flag\":\"head-branch\"},{\"json\":\"worktree_path\",\"from\":\"flag\",\"flag\":\"worktree-path\"}]}"},

{"pipeline.cancel",
 "{\"fields\":[{\"json\":\"pipeline_id\",\"from\":\"positional\",\"index\":0,\"type\":\"number_lenient\"},{\"json\":\"artifact\",\"from\":\"flag\",\"flag\":\"artifact\"},{\"json\":\"artifact_hash\",\"from\":\"flag\",\"flag\":\"artifact-hash\"},{\"json\":\"repo_root\",\"from\":\"flag\",\"flag\":\"repo-root\"},{\"json\":\"remote\",\"from\":\"flag\",\"flag\":\"remote\"},{\"json\":\"head_branch\",\"from\":\"flag\",\"flag\":\"head-branch\"},{\"json\":\"worktree_path\",\"from\":\"flag\",\"flag\":\"worktree-path\"}]}"},

{"pipeline.gate",
 "{\"fields\":[{\"json\":\"pipeline_id\",\"from\":\"positional\",\"index\":0,\"type\":\"number_lenient\"},{\"json\":\"verdict\",\"from\":\"positional\",\"index\":1,\"empty\":\"emit\"},{\"json\":\"reason\",\"from\":\"flag\",\"flag\":\"reason\"},{\"json\":\"operator_principal\",\"from\":\"flag\",\"flag\":\"operator-principal\"}]}"},

{"pipeline.list",
 "{\"fields\":[{\"json\":\"state\",\"from\":\"flag\",\"flag\":\"state\"}]}"},

{"pipeline.resume",
 "{\"fields\":[{\"json\":\"pipeline_id\",\"from\":\"positional\",\"index\":0,\"type\":\"number_lenient\"},{\"json\":\"artifact\",\"from\":\"flag\",\"flag\":\"artifact\"},{\"json\":\"artifact_hash\",\"from\":\"flag\",\"flag\":\"artifact-hash\"},{\"json\":\"repo_root\",\"from\":\"flag\",\"flag\":\"repo-root\"},{\"json\":\"remote\",\"from\":\"flag\",\"flag\":\"remote\"},{\"json\":\"head_branch\",\"from\":\"flag\",\"flag\":\"head-branch\"},{\"json\":\"worktree_path\",\"from\":\"flag\",\"flag\":\"worktree-path\"}]}"},

{"pipeline.show",
 "{\"fields\":[{\"json\":\"pipeline_id\",\"from\":\"positional\",\"index\":0,\"type\":\"number_lenient\"},{\"json\":\"artifact\",\"from\":\"flag\",\"flag\":\"artifact\"},{\"json\":\"artifact_hash\",\"from\":\"flag\",\"flag\":\"artifact-hash\"},{\"json\":\"repo_root\",\"from\":\"flag\",\"flag\":\"repo-root\"},{\"json\":\"remote\",\"from\":\"flag\",\"flag\":\"remote\"},{\"json\":\"head_branch\",\"from\":\"flag\",\"flag\":\"head-branch\"},{\"json\":\"worktree_path\",\"from\":\"flag\",\"flag\":\"worktree-path\"}]}"},

{"pipeline.status",
 "{\"fields\":[{\"json\":\"pipeline_id\",\"from\":\"positional\",\"index\":0,\"type\":\"number_lenient\"},{\"json\":\"artifact\",\"from\":\"flag\",\"flag\":\"artifact\"},{\"json\":\"artifact_hash\",\"from\":\"flag\",\"flag\":\"artifact-hash\"},{\"json\":\"repo_root\",\"from\":\"flag\",\"flag\":\"repo-root\"},{\"json\":\"remote\",\"from\":\"flag\",\"flag\":\"remote\"},{\"json\":\"head_branch\",\"from\":\"flag\",\"flag\":\"head-branch\"},{\"json\":\"worktree_path\",\"from\":\"flag\",\"flag\":\"worktree-path\"}]}"},

{"api.enable",
 "{\"bool_flags\":[\"vscode\"],\"fields\":[{\"json\":\"vscode\",\"from\":\"flag\",\"flag\":\"vscode\",\"type\":\"true_if_set\"},{\"json\":\"port\",\"from\":\"flag\",\"flag\":\"port\",\"type\":\"number_lenient\",\"default\":0,\"omit_if_nonpositive\":true},{\"json\":\"rate_limit\",\"from\":\"flag\",\"flag\":\"rate-limit\",\"type\":\"number_lenient\",\"default\":0,\"omit_if_nonpositive\":true}]}"},

{"session.brief",
 "{\"bool_flags\":[\"list\"],\"fields\":[{\"json\":\"session_id\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"session\"},{\"json\":\"list\",\"from\":\"flag\",\"flag\":\"list\",\"type\":\"true_if_set\"}]}"},

/* Same verbatim-argv shape as the model family: these dispatch to
   marshal_agent_args too. */

{"workspace.get",
 "{\"fields\":[{\"json\":\"args\",\"from\":\"argv_array\"}]}"},

{"workspace.remove",
 "{\"fields\":[{\"json\":\"args\",\"from\":\"argv_array\"}]}"},

{"index.deps",
 "{\"bool_flags\":[\"review\",\"reverse\",\"dry-run\"],\"fields\":[{\"json\":\"project\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"min_tier\",\"from\":\"flag\",\"flag\":\"tier\"},{\"json\":\"status\",\"from\":\"flag\",\"flag\":\"review\",\"type\":\"const_if_set\",\"value\":\"ambiguous\"},{\"json\":\"direction\",\"from\":\"flag\",\"flag\":\"reverse\",\"type\":\"const_if_set\",\"value\":\"in\"},{\"json\":\"dry_run\",\"from\":\"flag\",\"flag\":\"dry-run\",\"type\":\"true_if_set\"}]}"},

/* The inverted-flag pair: `compress` is true unless --no-compress was given. */

{"trajectory.export",
 "{\"bool_flags\":[\"no-compress\"],\"usage\":\"usage: aimee trajectory export <session_id> [--no-compress] [--max-result-bytes N]\",\"fields\":[{\"json\":\"session_id\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"session\",\"required\":true},{\"json\":\"compress\",\"from\":\"flag\",\"flag\":\"no-compress\",\"type\":\"bool_inverted\"},{\"json\":\"max_result_bytes\",\"from\":\"flag\",\"flag\":\"max-result-bytes\",\"type\":\"number_lenient\",\"default\":512,\"omit_if_nonpositive\":true}]}"},

{"trajectory.batch",
 "{\"bool_flags\":[\"no-compress\"],\"usage\":\"usage: aimee trajectory batch --tasks corpus.jsonl|suite_dir [--toolset-dist research] [--out dir]\",\"fields\":[{\"json\":\"tasks_path\",\"from\":\"flag\",\"flag\":\"tasks\",\"required\":true},{\"json\":\"toolset_dist\",\"from\":\"flag\",\"flag\":\"toolset-dist\"},{\"json\":\"out_dir\",\"from\":\"flag\",\"flag\":\"out\"},{\"json\":\"compress\",\"from\":\"flag\",\"flag\":\"no-compress\",\"type\":\"bool_inverted\"},{\"json\":\"max_result_bytes\",\"from\":\"flag\",\"flag\":\"max-result-bytes\",\"type\":\"number_lenient\",\"default\":512,\"omit_if_nonpositive\":true}]}"},

{"session.close",
 "{\"fields\":[{\"json\":\"session_id\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"session\"}]}"},

{"session.get",
 "{\"fields\":[{\"json\":\"session_id\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"session\"}]}"},

/* Raw argv, read before flag parsing. Described, not endorsed -- see
   headers/cli_argspec.h on argv_index. */

{"memory.delete",
 "{\"fields\":[{\"json\":\"id\",\"from\":\"argv_index\",\"index\":0,\"type\":\"number_lenient_int64\",\"empty\":\"emit\"}]}"},

{"provider.set",
 "{\"fields\":[{\"json\":\"name\",\"from\":\"argv_index\",\"index\":0}]}"},

/* argv_array PLUS top-level fields: the route reads both. */

{"workspace.add",
 "{\"bool_flags\":[\"no-scan\"],\"fields\":[{\"json\":\"args\",\"from\":\"argv_array\"},{\"json\":\"root\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"provider\",\"from\":\"flag\",\"flag\":\"provider\",\"empty\":\"emit\"},{\"json\":\"remote\",\"from\":\"flag\",\"flag\":\"remote\",\"empty\":\"emit\"},{\"json\":\"head\",\"from\":\"flag\",\"flag\":\"head\",\"empty\":\"emit\"},{\"json\":\"scan\",\"from\":\"flag\",\"flag\":\"no-scan\",\"type\":\"const_if_set\",\"value\":false}]}"},

/* Repeated --skill flags collect into an array. */

{"cron.add",
 "{\"bool_flags\":[\"only-if-changed\",\"first-run-silent\",\"pre-wake-gate\",\"disabled\"],\"usage\":\"usage: aimee cron add <id> --schedule S [--mode llm|script|hybrid] [--script CMD] [--prompt TEXT]\",\"fields\":[{\"json\":\"job_id\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"id\",\"required\":true},{\"json\":\"schedule\",\"from\":\"flag\",\"flag\":\"schedule\",\"required\":true},{\"json\":\"mode\",\"from\":\"flag\",\"flag\":\"mode\"},{\"json\":\"script\",\"from\":\"flag\",\"flag\":\"script\"},{\"json\":\"prompt\",\"from\":\"flag\",\"flag\":\"prompt\"},{\"json\":\"workdir\",\"from\":\"flag\",\"flag\":\"workdir\"},{\"json\":\"deliver_target\",\"from\":\"flag\",\"flag\":\"target\"},{\"json\":\"context_from\",\"from\":\"flag\",\"flag\":\"context-from\"},{\"json\":\"when_context_contains\",\"from\":\"flag\",\"flag\":\"when-context-contains\"},{\"json\":\"deliver_only_if_changed\",\"from\":\"flag\",\"flag\":\"only-if-changed\",\"type\":\"const_if_set\",\"value\":true},{\"json\":\"deliver_first_run_silent\",\"from\":\"flag\",\"flag\":\"first-run-silent\",\"type\":\"const_if_set\",\"value\":true},{\"json\":\"pre_wake_gate\",\"from\":\"flag\",\"flag\":\"pre-wake-gate\",\"type\":\"const_if_set\",\"value\":true},{\"json\":\"enabled\",\"from\":\"flag\",\"flag\":\"disabled\",\"type\":\"const_if_set\",\"value\":false},{\"json\":\"skills\",\"from\":\"repeated_flag\",\"flag\":\"skill\"}]}"},

/* Raw argv that refuses a flag-looking word. */

{"mcp.recheck",
 "{\"fields\":[{\"json\":\"name\",\"from\":\"argv_index\",\"index\":0,\"skip_if_dash\":true}]}"},

/* Reachable only since the served consult moved above marshal_request's
   custom-body block; before that a spec for these could never be reached. */

{"toolset.show",
 "{\"fields\":[{\"json\":\"name\",\"from\":\"argv_index\",\"index\":0,\"required\":true,\"empty\":\"emit\"}]}"},

{"toolset.resolve",
 "{\"fields\":[{\"json\":\"name\",\"from\":\"argv_index\",\"index\":0,\"required\":true,\"empty\":\"emit\"}]}"},

/* One field, two flags, three states. */

{"dogfood.tag",
 "{\"bool_flags\":[\"surprise\",\"no-surprise\"],\"fields\":[{\"json\":\"record_id\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"outcome\",\"from\":\"flag\",\"flag\":\"outcome\",\"empty\":\"emit\"},{\"json\":\"notes\",\"from\":\"flag\",\"flag\":\"notes\",\"empty\":\"emit\"},{\"json\":\"richness\",\"from\":\"flag\",\"flag\":\"richness\",\"type\":\"number_lenient\"},{\"json\":\"surprise\",\"from\":\"flag\",\"flag\":\"surprise\",\"false_flag\":\"no-surprise\",\"type\":\"tristate_flag\"}]}"},
