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
 * A spec may NAME two client facts -- `cwd` and `session` -- and the client
 * supplies them. What a spec may not do is read arbitrary files or arbitrary
 * environment variables: the value would be the client's own business, and a
 * vocabulary general enough to ask for one is general enough to ask for a
 * credential. See the note below on the 26 methods this distinction cost.
 *
 * WHY THE REST ARE NOT HERE YET. 156 of the 181 CLI-reachable methods are
 * served. The 25 that are not are listed BY NAME below with what each would
 * take. None is blocked by the spec language being small; each is blocked by
 * what its marshaller does.
 *
 * THREE CLAIMS IN THIS FILE TURNED OUT TO BE MISTAKES, all the same shape: a
 * line drawn on convenience and then defended as principle.
 *
 *   1. "43 methods must never be served, they read the client's own state."
 *      That conflated the client DECIDING a field needs getcwd() -- client-side
 *      knowledge, and what forces a rebuild -- with the client SUPPLYING it
 *      because a served spec asked. Cost: 26 methods. The genuinely client-only
 *      set is SIX.
 *
 *   2. "No branch may decide which fields exist." This vocabulary is FULL of
 *      conditionals -- empty:"drop", omit_if_nonpositive, omit_below,
 *      max_positionals, first_of -- and none makes a spec a program. An ARITY
 *      gate consults the invocation's shape, which max_positionals already does
 *      one level up. count_min/count_max/argc_min are that gate. Cost: 6.
 *
 *   3. "A literal comparison is string surgery." skip_if_dash already tests a
 *      field's own value against a prefix; `equals`/`not_equals` test it
 *      against a literal, which is the same kind of rule. skill.lint reads
 *      argv[0] and sends `all` when it is exactly "--all" and `name` when it is
 *      not. Cost: 2.
 *
 * THE LINE, restated so it can be judged rather than re-argued: a field's rule
 * may depend on ITS OWN value, its own flags, named client facts the SERVER
 * asked for, and the invocation's ARITY. It may not depend on ANOTHER FIELD's
 * value, and it may not compute one.
 *
 * That line still forbids things, and holds where it should: skill.archive gates
 * absorbed_into (read from argv[2]) on argv[1] being "--absorbed-into", and is
 * refused for exactly that reason -- a spec without the gate would send the
 * field for any third argument.
 *
 * WHAT REMAINS, and what each would take:
 *
 *   SIX are genuinely client-only: the file CONTENTS or key material go into the
 *   body, and a spec that could say "read this path" could say "read
 *   ~/.ssh/id_rsa":
 *     delegate, delegate.launch, roundtable.review, skill.create, skill.edit,
 *     vault.unlock
 *
 *   FIVE wear the MCP tool-call envelope -- method "mcp.call" or "help.get", a
 *   constant `tool`, a nested `arguments`, no protocol_version -- and four of
 *   the five read $AIMEE_SESSION_ID or the cwd or parse argv anyway:
 *     get_help, git.cli, git.verify, index.ast_grep, tool.call
 *
 *   FOUR derive rather than source: the memory.user_capture family joins the
 *   positionals from index 1, prefixes the key, and refuses one over 512
 *   characters:
 *     memory.archive, memory.identity, memory.prefer, memory.store
 *
 *   THREE parse argv with a grammar of their own, in a loop, without
 *   cli_args_parse -- so they take `--snapshot X` but NOT `--snapshot=X`, which
 *   cli_args_parse accepts and every served spec therefore does:
 *     memory.supersede, skill.autostub, skill.lifecycle
 *
 *   SEVEN need a CROSS-FIELD rule, the half of the line that still forbids.
 *   Each would be served by a marshaller change, not a vocabulary change, and
 *   that change alters what the CLI sends:
 *     cron.enable, cron.disable  -- job_id OR all:true, and a refusal when
 *                                  NEITHER is given.
 *     trigger.fire               -- --source AND (--task OR --proposal).
 *     primary.set                -- --show / --clear / a positional select
 *                                  three different METHODS, not three fields.
 *     delegate.status            -- job_ids (array) or job_id (scalar).
 *     catalog.show               -- splits "provider:model" on a colon and
 *                                  truncates at 64 bytes.
 *     index.span                 -- the positional INDEX depends on whether an
 *                                  earlier argument parses as a number.
 *     skill.archive              -- gated on a different argv slot than it reads.
 *
 * AND NOTE WHAT THE TEST CANNOT SEE. Its samples are generated FROM the spec, so
 * a rule the spec omits is invisible. Every defect below was found that way, and
 * each is the same shape -- reading PART of a rule:
 *
 *   - memory.delete SHIPPED saying atoi where the marshaller calls atoll().
 *     atoi() keeps the LOW 32 BITS AS A SIGNED INT, measured on a real
 *     appliance: 4294967297 -> 1. `aimee memory delete 4294967297` deleted
 *     memory 1. A JSON number is a double, so ids above 2^53 cannot round-trip
 *     whatever parse is named -- a ceiling shared with the marshaller.
 *   - index.structure sent the file as a PROJECT. Two positionals mean
 *     <project> <file_path>, one means <file_path>. The samples generated two
 *     and three, never ONE -- the way the command is used. Found by running the
 *     real command against a real server.
 *   - kb.search reuses one scratch variable for four flags; the FIRST assignment
 *     gave scope, fusion_mode and embedding_command all --project.
 *   - memory.recall is a four-step cascade; the NEAREST assignment gave the last
 *     step and dropped --task and the positional.
 *   - worktree.gc clamps after reading, and a clamp is itself an assignment.
 *   - provider.list guards available_only with --available; deriving the flag
 *     from the FIELD name produced a spec that never set it.
 *   - skill.pin guards on argc, not pos_count: `skill pin --x y` has argc 2 and
 *     pos_count 0, and the marshaller sends name="--x".
 *   - the session precedence plant PASSED once, undetected, because no spec used
 *     the source and $AIMEE_SESSION_ID was unset.
 *
 * Hence the sample set: adversarial, both-sources, numeric straddling 2^31,
 * cascades supplying two steps at once, ARITY at every count from one to n+1,
 * and the `--flag=value` form -- plus
 * scripts/check_argspec_numeric_parity.py, which compares each spec's numeric
 * type against the parse its marshaller calls rather than hoping an input lands
 * on the boundary. Each was plant-tested: a deliberate violation introduced and
 * the check confirmed to FAIL on it, because a check that has never failed is
 * decoration.
 */

/* No fields at all, and a refusal if anything positional is typed: `aimee
   delegate log 42` means the operator wanted a JOB log, and silently dropping
   the 42 would show them the wrong thing. It cannot go in the no-argument list
   for exactly that reason -- that list accepts whatever it is given.

   bool_flags is deliberately absent even though --json exists: the marshaller
   passes NULL, so --json consumes the following token as its value, and a spec
   that declared it a boolean would leave that token as a positional and refuse
   an invocation the compiled path accepts. */
/* The cwd family. These were listed as NEVER serveable, on the grounds that a
   client reading its own disk is not obeying the server. That was wrong, and it
   cost 26 methods: the client SUPPLYING the working directory because a served
   spec asked for it is the thin-client model working, and only the DECISION had
   to move server-side. Under the old reading a new cwd-taking command could not
   ship without a new client, which is the exact failure this work removes. */
{"index.find",
 "{\"bool_flags\":[\"json\"],\"fields\":[{\"json\":\"identifier\","
 "\"count_min\":1,\"from\":\"positional\",\"index\":0,\"empty\":\"emit\""
 "},{\"json\":\"scope\",\"from\":\"flag\",\"flag\":\"scope\",\"empty\":"
 "\"emit\"},{\"json\":\"cwd\",\"from\":\"cwd\"}]}"},

{"memory.list",
 "{\"fields\":[{\"json\":\"tier\",\"from\":\"flag\",\"flag\":\"tier\","
 "\"empty\":\"emit\"},{\"json\":\"kind\",\"from\":\"flag\",\"flag\":"
 "\"kind\",\"empty\":\"emit\"},{\"json\":\"limit\",\"from\":\"flag\","
 "\"flag\":\"limit\",\"type\":\"number_lenient\",\"default\":20,"
 "\"empty\":\"emit\"},{\"json\":\"project\",\"from\":\"flag\",\"flag\":"
 "\"project\",\"empty\":\"emit\"},{\"json\":\"workspace\",\"from\":"
 "\"flag\",\"flag\":\"workspace\",\"empty\":\"emit\"},{\"json\":"
 "\"scope\",\"from\":\"flag\",\"flag\":\"scope\",\"empty\":\"emit\"},{"
 "\"json\":\"cwd\",\"from\":\"cwd\"}]}"},

/* Generated from the marshallers once cwd and session became describable, and
   judged by the differential test rather than by inspection. */
{"identity.snapshot",
 "{\"fields\":[{\"json\":\"out\",\"from\":\"flag\",\"flag\":\"out\","
 "\"empty\":\"emit\"},{\"json\":\"cwd\",\"from\":\"cwd\"}]}"},

{"index.find_callers",
 "{\"bool_flags\":[\"json\"],\"fields\":[{\"json\":\"symbol\","
 "\"count_min\":1,\"from\":\"positional\",\"index\":0,\"empty\":\"emit\""
 "},{\"json\":\"project\",\"count_min\":2,\"from\":\"positional\","
 "\"index\":1,\"empty\":\"emit\"},{\"json\":\"scope\",\"from\":\"flag\","
 "\"flag\":\"scope\",\"empty\":\"emit\"},{\"json\":\"cwd\",\"from\":"
 "\"cwd\"}]}"},

{"kb.search",
 "{\"fields\":[{\"json\":\"query\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"project\","
 "\"from\":\"flag\",\"flag\":\"project\",\"empty\":\"emit\"},{\"json\":"
 "\"scope\",\"from\":\"flag\",\"flag\":\"scope\",\"empty\":\"emit\"},{"
 "\"json\":\"cwd\",\"from\":\"cwd\"},{\"json\":\"max_results\",\"from\":"
 "\"flag\",\"flag\":\"max\",\"type\":\"number_lenient\",\"default\":10,"
 "\"empty\":\"emit\"},{\"json\":\"fusion_mode\",\"from\":\"flag\","
 "\"flag\":\"fusion-mode\",\"empty\":\"emit\"},{\"json\":"
 "\"embedding_command\",\"from\":\"flag\",\"flag\":\"embed\",\"empty\":"
 "\"emit\"}]}"},

{"wm.get",
 "{\"fields\":[{\"json\":\"key\",\"count_min\":1,\"from\":\"positional\""
 ",\"index\":0,\"empty\":\"emit\"},{\"json\":\"session_id\",\"from\":"
 "\"session\",\"empty\":\"emit\"}]}"},

{"wm.list",
 "{\"fields\":[{\"json\":\"session_id\",\"from\":\"session\",\"empty\":"
 "\"emit\"},{\"json\":\"category\",\"from\":\"flag\",\"flag\":"
 "\"category\",\"empty\":\"emit\"}]}"},

{"wm.set",
 "{\"fields\":[{\"json\":\"key\",\"count_min\":1,\"from\":\"positional\""
 ",\"index\":0,\"empty\":\"emit\"},{\"json\":\"value\",\"count_min\":2,"
 "\"from\":\"positional\",\"index\":1,\"empty\":\"emit\"},{\"json\":"
 "\"session_id\",\"from\":\"session\",\"empty\":\"emit\"},{\"json\":"
 "\"category\",\"from\":\"flag\",\"flag\":\"category\",\"empty\":"
 "\"emit\"},{\"json\":\"ttl\",\"from\":\"flag\",\"flag\":\"ttl\","
 "\"type\":\"number_lenient\",\"default\":0,\"empty\":\"emit\","
 "\"omit_if_nonpositive\":true}]}"},

/* task_hint is a four-step cascade: --task, then positional[0], then --query,
   then the literal "session start". --query feeds the SAME field rather than a
   second one, because it used to be sent as its own `query` key that nothing
   read -- the recall handler takes task_hint, so `memory recall --query "..."`
   sent the text, had it ignored, and returned the recency bundle while looking
   like it had searched.

   Hand-written, and the generator is taught to REFUSE this shape rather than
   read it. Resolving the variable to its nearest assignment picks the LAST step
   and drops --task and the positional silently, which is worse than refusing
   because it looks like an answer. The differential test certifies this one. */
{"memory.recall",
 "{\"fields\":["
 "{\"json\":\"task_hint\",\"from\":\"first_of\",\"default\":\"session start\","
 "\"default_on_empty\":true,"
 "\"sources\":[{\"from\":\"flag\",\"flag\":\"task\"},"
 "{\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"},"
 "{\"from\":\"flag\",\"flag\":\"query\"}]},"
 "{\"json\":\"session_start\",\"from\":\"flag\",\"flag\":\"session-start\","
 "\"type\":\"true_if_set\"},"
 "{\"json\":\"limit_tokens\",\"from\":\"flag\",\"flag\":\"limit-tokens\","
 "\"type\":\"number_lenient\",\"omit_if_nonpositive\":true},"
 "{\"json\":\"project\",\"from\":\"flag\",\"flag\":\"project\",\"empty\":\"emit\"},"
 "{\"json\":\"workspace\",\"from\":\"flag\",\"flag\":\"workspace\",\"empty\":\"emit\"},"
 "{\"json\":\"scope\",\"from\":\"flag\",\"flag\":\"scope\",\"empty\":\"emit\"},"
 "{\"json\":\"cwd\",\"from\":\"cwd\"}]}"},

/* session_id is `pos_count > 0 ? positional[0] : resolve_session_env()`: a
   two-step cascade ending in a fact only the client holds. An EMPTY positional
   stops the cascade and is sent as "", which is why the first_of source keeps an
   empty value rather than falling through when no default is named. */
{"session.attach",
 "{\"bool_flags\":[\"persistent\"],\"fields\":["
 "{\"json\":\"session_id\",\"from\":\"first_of\",\"empty\":\"emit\","
 "\"sources\":[{\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"},"
 "{\"from\":\"session\"}]},"
 "{\"json\":\"surface\",\"from\":\"first_of\",\"default\":\"cli\","
 "\"default_on_empty\":true,"
 "\"sources\":[{\"from\":\"flag\",\"flag\":\"surface\"}]},"
 "{\"json\":\"target\",\"from\":\"flag\",\"flag\":\"target\"},"
 "{\"json\":\"owner\",\"from\":\"flag\",\"flag\":\"owner\"},"
 "{\"json\":\"subscribe_mask\",\"from\":\"flag\",\"flag\":\"subscribe\","
 "\"type\":\"number_lenient\",\"default\":-1,\"omit_below\":0},"
 "{\"json\":\"persistent\",\"from\":\"flag\",\"flag\":\"persistent\","
 "\"type\":\"true_if_set\"}]}"},

{"session.detach",
 "{\"fields\":["
 "{\"json\":\"session_id\",\"from\":\"first_of\",\"empty\":\"emit\","
 "\"sources\":[{\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"},"
 "{\"from\":\"session\"}]},"
 "{\"json\":\"attach_id\",\"from\":\"first_of\",\"empty\":\"emit\","
 "\"default\":\"\",\"sources\":[{\"from\":\"flag\",\"flag\":\"attach-id\"}]}]}"},

{"eval.run",
 "{\"fields\":[{\"json\":\"suite_dir\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"ablation\","
 "\"from\":\"flag\",\"flag\":\"ablation\",\"empty\":\"emit\"},{\"json\":"
 "\"runs\",\"from\":\"flag\",\"flag\":\"runs\",\"empty\":\"emit\","
 "\"type\":\"number_lenient\"},{\"json\":\"seed\",\"type\":"
 "\"number_lenient_ulong\",\"from\":\"flag\",\"flag\":\"seed\",\"empty\""
 ":\"emit\"},{\"json\":\"cwd\",\"from\":\"cwd\"}]}"},

{"identity.diff",
 "{\"fields\":[{\"json\":\"a\",\"count_min\":1,\"from\":\"positional\","
 "\"index\":0,\"empty\":\"emit\"},{\"json\":\"b\",\"count_min\":2,"
 "\"from\":\"positional\",\"index\":1,\"empty\":\"emit\"},{\"json\":"
 "\"flip_threshold\",\"type\":\"number_lenient_real\",\"from\":\"flag\","
 "\"flag\":\"flip-threshold\",\"empty\":\"emit\"},{\"json\":\"cwd\","
 "\"from\":\"cwd\"}]}"},

/* suite is `pos_count >= 1 ? positional[0] : "code-graph-fusion"`. The guard is
   on the COUNT, so an empty positional is the value and does NOT fall back --
   which is why this one does not set default_on_empty and memory.recall does. */
{"memory.benchmark",
 "{\"fields\":[{\"json\":\"suite\",\"from\":\"first_of\","
 "\"default\":\"code-graph-fusion\",\"empty\":\"emit\","
 "\"sources\":[{\"from\":\"positional\",\"index\":0,"
 "\"empty\":\"emit\"}]}]}"},

{"memory.search",
 "{\"fields\":[{\"json\":\"keywords\",\"from\":\"positional_array\"},{"
 "\"json\":\"limit\",\"from\":\"flag\",\"flag\":\"limit\",\"type\":"
 "\"number_lenient\",\"default\":10,\"empty\":\"emit\"},{\"json\":"
 "\"project\",\"from\":\"flag\",\"flag\":\"project\",\"empty\":\"emit\"}"
 ",{\"json\":\"workspace\",\"from\":\"flag\",\"flag\":\"workspace\","
 "\"empty\":\"emit\"},{\"json\":\"scope\",\"from\":\"flag\",\"flag\":"
 "\"scope\",\"empty\":\"emit\"},{\"json\":\"cwd\",\"from\":\"cwd\"}]}"},

{"worktree.gc",
 "{\"bool_flags\":[\"force\",\"dry-run\"],\"fields\":[{\"json\":"
 "\"client_cwd\",\"from\":\"cwd\"},{\"json\":\"max_age_days\",\"from\":"
 "\"flag\",\"flag\":\"days\",\"type\":\"number_lenient\",\"default\":14,"
 "\"empty\":\"emit\",\"min\":1,\"max\":365},{\"json\":\"force\",\"from\""
 ":\"flag\",\"type\":\"true_if_set\",\"flag\":\"force\"},{\"json\":"
 "\"dry_run\",\"from\":\"flag\",\"type\":\"true_if_set\",\"flag\":"
 "\"dry-run\"}]}"},

{"pipeline.start",
 "{\"fields\":[{\"json\":\"idea\",\"from\":\"positional\",\"index\":0,"
 "\"empty\":\"emit\"},{\"json\":\"done_bar\",\"from\":\"flag\",\"flag\":"
 "\"done-bar\"},{\"json\":\"base_branch\",\"from\":\"flag\",\"flag\":"
 "\"base-branch\"},{\"json\":\"repo_root\",\"from\":\"flag\",\"flag\":"
 "\"repo-root\"},{\"json\":\"brief\",\"from\":\"flag\",\"flag\":"
 "\"brief\"},{\"json\":\"head_branch\",\"from\":\"flag\",\"flag\":"
 "\"head-branch\"},{\"json\":\"remote\",\"from\":\"flag\",\"flag\":"
 "\"remote\"},{\"json\":\"worktree_path\",\"from\":\"flag\",\"flag\":"
 "\"worktree-path\"}]}"},

{"skill.list",
 "{\"fields\":[{\"json\":\"cwd\",\"from\":\"cwd\"}]}"},

{"skill.archive",
 "{\"fields\":[{\"json\":\"cwd\",\"from\":\"cwd\"},{\"json\":\"name\","
 "\"argc_min\":1,\"from\":\"argv_index\",\"index\":0,\"empty\":\"emit\"}"
 ",{\"json\":\"absorbed_into\",\"argc_min\":1,\"from\":\"argv_index\","
 "\"index\":2,\"empty\":\"emit\"}]}"},

{"skill.eval",
 "{\"fields\":[{\"json\":\"cwd\",\"from\":\"cwd\"},{\"json\":\"name\","
 "\"from\":\"argv_index\",\"index\":0,\"empty\":\"emit\"}]}"},

{"skill.show",
 "{\"fields\":[{\"json\":\"cwd\",\"from\":\"cwd\"},{\"json\":\"name\","
 "\"argc_min\":1,\"from\":\"argv_index\",\"index\":0,\"empty\":\"emit\"}"
 ",{\"json\":\"file_path\",\"argc_min\":1,\"from\":\"argv_index\","
 "\"index\":2,\"empty\":\"emit\"}]}"},

{"index.blast_radius",
 "{\"bool_flags\":[\"json\"],\"fields\":[{\"json\":\"project\","
 "\"count_min\":2,\"from\":\"positional\",\"index\":0,\"empty\":\"emit\""
 "},{\"json\":\"file_path\",\"count_min\":2,\"from\":\"positional\","
 "\"index\":1,\"empty\":\"emit\"},{\"json\":\"file_path\",\"count_min\":"
 "1,\"count_max\":1,\"from\":\"positional\",\"index\":0,\"empty\":"
 "\"emit\"},{\"json\":\"cwd\",\"from\":\"cwd\"}]}"},

{"index.hybrid",
 "{\"bool_flags\":[\"json\"],\"fields\":[{\"json\":\"queries\",\"from\":"
 "\"positional_array\",\"count_min\":2},{\"json\":\"query\","
 "\"count_min\":1,\"count_max\":1,\"from\":\"positional\",\"index\":0,"
 "\"empty\":\"emit\"},{\"json\":\"scope\",\"from\":\"flag\",\"flag\":"
 "\"scope\"},{\"json\":\"cwd\",\"from\":\"cwd\"}]}"},

{"index.investigate",
 "{\"bool_flags\":[\"json\"],\"fields\":[{\"json\":\"queries\",\"from\":"
 "\"positional_array\",\"count_min\":2},{\"json\":\"query\","
 "\"count_min\":1,\"count_max\":1,\"from\":\"positional\",\"index\":0,"
 "\"empty\":\"emit\"},{\"json\":\"cwd\",\"from\":\"cwd\"}]}"},

{"index.structure",
 "{\"bool_flags\":[\"json\"],\"fields\":[{\"json\":\"project\","
 "\"count_min\":2,\"from\":\"positional\",\"index\":0,\"empty\":\"emit\""
 "},{\"json\":\"file_path\",\"count_min\":2,\"from\":\"positional\","
 "\"index\":1,\"empty\":\"emit\"},{\"json\":\"file_path\",\"count_min\":"
 "1,\"count_max\":1,\"from\":\"positional\",\"index\":0,\"empty\":"
 "\"emit\"},{\"json\":\"cwd\",\"from\":\"cwd\"}]}"},

{"skill.pin",
 "{\"fields\":[{\"json\":\"cwd\",\"from\":\"cwd\"},{\"json\":\"name\","
 "\"argc_min\":1,\"from\":\"argv_index\",\"index\":0,\"empty\":\"emit\"}"
 ",{\"json\":\"pinned\",\"argc_min\":1,\"from\":\"const\",\"type\":"
 "\"const_bool\",\"value\":true}]}"},

{"skill.unpin",
 "{\"fields\":[{\"json\":\"cwd\",\"from\":\"cwd\"},{\"json\":\"name\","
 "\"argc_min\":1,\"from\":\"argv_index\",\"index\":0,\"empty\":\"emit\"}"
 ",{\"json\":\"pinned\",\"argc_min\":1,\"from\":\"const\",\"type\":"
 "\"const_bool\",\"value\":false}]}"},

{"skill.lint",
 "{\"fields\":[{\"json\":\"cwd\",\"from\":\"cwd\"},{\"json\":\"all\","
 "\"from\":\"argv_index\",\"index\":0,\"type\":\"const_bool\",\"value\":"
 "true,\"equals\":\"--all\",\"argc_min\":1},{\"json\":\"name\",\"from\":"
 "\"argv_index\",\"index\":0,\"not_equals\":\"--all\",\"argc_min\":1,"
 "\"empty\":\"emit\"}]}"},

{"skill.patch",
 "{\"fields\":[{\"json\":\"cwd\",\"from\":\"cwd\"},{\"json\":\"name\","
 "\"argc_min\":1,\"from\":\"argv_index\",\"index\":0,\"empty\":\"emit\"}"
 ",{\"json\":\"old_string\",\"argc_min\":3,\"from\":\"argv_index\","
 "\"index\":1,\"empty\":\"emit\"},{\"json\":\"new_string\",\"argc_min\":"
 "3,\"from\":\"argv_index\",\"index\":2,\"empty\":\"emit\"},{\"json\":"
 "\"replace_all\",\"argc_min\":4,\"from\":\"argv_index\",\"index\":3,"
 "\"type\":\"const_bool\",\"value\":true,\"equals\":\"--all\"}]}"},

/* A custom body inside marshal_request rather than a named marshaller, which is
   why the generator never saw it: one cwd field, and argc/argv explicitly
   discarded. It was filed under client-local state for most of this work, on the
   reading that a client touching getcwd() could not be served at all. */
{"init.run",
 "{\"fields\":[{\"json\":\"cwd\",\"from\":\"cwd\"}]}"},

{"delegate.log",
 "{\"usage\":\"usage: aimee delegate log [--json]; for a background job log, "
 "use `aimee jobs logs <job_id>`\",\"max_positionals\":0}"},

{"session.presence",
 "{\"fields\":[{\"json\":\"owner\",\"from\":\"flag\",\"flag\":\"owner\"}"
 "]}"},

{"insights.overview",
 "{\"fields\":[{\"json\":\"days\",\"from\":\"flag\",\"flag\":\"days\","
 "\"type\":\"number_lenient\",\"default\":30,\"empty\":\"emit\",\"min\":"
 "1,\"max\":365}]}"},

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
 "{\"bool_flags\":[\"available\",\"all\",\"json\"],\"fields\":[{\"json\""
 ":\"available_only\",\"from\":\"flag\",\"type\":\"true_if_set\","
 "\"flag\":\"available\"},{\"json\":\"all\",\"from\":\"flag\",\"type\":"
 "\"true_if_set\",\"flag\":\"all\"},{\"json\":\"json\",\"from\":\"flag\""
 ",\"type\":\"true_if_set\",\"flag\":\"json\"}]}"},

{"provider.models",
 "{\"bool_flags\":[\"json\"],\"fields\":[{\"json\":\"name\",\"from\":"
 "\"positional\",\"index\":0},{\"json\":\"json\",\"from\":\"flag\","
 "\"type\":\"true_if_set\",\"flag\":\"json\"}]}"},

{"catalog.list",
 "{\"bool_flags\":[\"json\",\"open-weights\"],\"fields\":[{\"json\":"
 "\"capability\",\"from\":\"flag\",\"flag\":\"capability\"},{\"json\":"
 "\"json\",\"from\":\"flag\",\"type\":\"true_if_set\",\"flag\":\"json\"}"
 ",{\"json\":\"open_weights_only\",\"from\":\"flag\",\"flag\":"
 "\"open-weights\",\"type\":\"true_if_set\"}]}"},

{"trigger.list",
 "{\"fields\":[{\"json\":\"status\",\"from\":\"flag\",\"flag\":"
 "\"status\"}]}"},

{"trigger.status",
 "{\"usage\":\"usage: aimee trigger status <id>\","
 "\"fields\":[{\"json\":\"id\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"id\","
 "\"required\":true}]}"},

{"trigger.cancel",
 "{\"usage\":\"usage: aimee trigger cancel <id>\","
 "\"fields\":[{\"json\":\"id\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"id\","
 "\"required\":true}]}"},

{"model.episodes",
 "{\"fields\":[{\"json\":\"agent\",\"from\":\"positional_or_flag\","
 "\"index\":0,\"flag\":\"agent\"}]}"},

{"graph.sync_code",
 "{\"fields\":[{\"json\":\"project\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"empty\":\"emit\"}]}"},

{"dogfood.report",
 "{\"bool_flags\":[\"json\"],\"fields\":[{\"json\":\"month\",\"from\":"
 "\"flag\",\"flag\":\"month\",\"empty\":\"emit\"},{\"json\":\"dir\","
 "\"from\":\"flag\",\"flag\":\"dir\",\"empty\":\"emit\"}]}"},

/* These gate on pos_count alone, so an empty argument is SENT -- the
   "empty":"emit" convention. Servable only once the spec could say that; before
   it could, each of these disagreed with its marshaller on exactly one sample. */

{"eval.results",
 "{\"fields\":[{\"json\":\"suite\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"empty\":\"emit\"}]}"},

{"cert.revoke",
 "{\"fields\":[{\"json\":\"serial\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"empty\":\"emit\"}]}"},

{"vault.capability",
 "{\"fields\":[{\"json\":\"action\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"principal\""
 ",\"count_min\":2,\"from\":\"positional\",\"index\":1,\"empty\":"
 "\"emit\"}]}"},

{"vault.delete",
 "{\"fields\":[{\"json\":\"agent\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"cred\","
 "\"count_min\":2,\"from\":\"positional\",\"index\":1,\"empty\":\"emit\""
 "}]}"},

{"vault.set",
 "{\"fields\":[{\"json\":\"agent\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"cred\","
 "\"count_min\":2,\"from\":\"positional\",\"index\":1,\"empty\":\"emit\""
 "},{\"json\":\"secret\",\"count_min\":3,\"from\":\"positional\","
 "\"index\":2,\"empty\":\"emit\"}]}"},

{"vault.set_server",
 "{\"fields\":[{\"json\":\"agent\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"cred\","
 "\"count_min\":2,\"from\":\"positional\",\"index\":1,\"empty\":\"emit\""
 "},{\"json\":\"secret\",\"count_min\":3,\"from\":\"positional\","
 "\"index\":2,\"empty\":\"emit\"}]}"},

/* The lenient-number family. Each parses with atoi()/cli_args_get_int(), so a
   non-numeric argument becomes 0 rather than a refusal -- described with
   "number_lenient", and sampled with a non-numeric value so the description is
   proven rather than assumed. */

{"graph.explain",
 "{\"fields\":[{\"json\":\"entity\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"limit\","
 "\"from\":\"flag\",\"flag\":\"limit\",\"type\":\"number_lenient\","
 "\"default\":40,\"empty\":\"emit\"}]}"},

{"aux.test",
 "{\"fields\":[{\"json\":\"task\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"prompt\","
 "\"count_min\":2,\"from\":\"positional\",\"index\":1,\"empty\":\"emit\""
 "},{\"json\":\"max_tokens\",\"count_min\":3,\"from\":\"positional\","
 "\"index\":2,\"type\":\"number_lenient\"}]}"},

{"dogfood.review",
 "{\"bool_flags\":[\"json\"],\"fields\":[{\"json\":\"month\",\"from\":"
 "\"flag\",\"flag\":\"month\",\"empty\":\"emit\"},{\"json\":\"dir\","
 "\"from\":\"flag\",\"flag\":\"dir\",\"empty\":\"emit\"},{\"json\":"
 "\"limit\",\"from\":\"flag\",\"flag\":\"limit\",\"empty\":\"emit\","
 "\"type\":\"number_lenient\"}]}"},

/* Generated from each marshaller and proven against it; see the test. */

{"config.get",
 "{\"fields\":[{\"json\":\"key\",\"count_min\":1,\"from\":\"positional\""
 ",\"index\":0,\"empty\":\"emit\"}]}"},
{"config.set",
 "{\"fields\":[{\"json\":\"key\",\"count_min\":1,\"from\":\"positional\""
 ",\"index\":0,\"empty\":\"emit\"},{\"json\":\"value\",\"count_min\":2,"
 "\"from\":\"positional\",\"index\":1,\"empty\":\"emit\"}]}"},
{"delegate.aggregate",
 "{\"bool_flags\":[\"json\"],\"fields\":[{\"json\":\"prompt\","
 "\"count_min\":1,\"from\":\"positional\",\"index\":0,\"empty\":\"emit\""
 "}]}"},
{"evidence.fidelity_retrieval_event",
 "{\"fields\":[{\"json\":\"turn_id\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"empty\":\"emit\"}]}"},
{"evidence.provenance_retrieval_event",
 "{\"fields\":[{\"json\":\"turn_id\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"empty\":\"emit\"}]}"},
{"evidence.trace_retrieval_event",
 "{\"fields\":[{\"json\":\"turn_id\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"empty\":\"emit\"}]}"},
{"index.scan",
 "{\"bool_flags\":[\"force\"],\"fields\":[{\"json\":\"name\","
 "\"count_min\":1,\"from\":\"positional\",\"index\":0,\"empty\":\"emit\""
 "},{\"json\":\"root\",\"count_min\":2,\"from\":\"positional\",\"index\""
 ":1,\"empty\":\"emit\"},{\"json\":\"force\",\"from\":\"flag\",\"type\":"
 "\"true_if_set\",\"flag\":\"force\"}]}"},
{"kb.build",
 "{\"bool_flags\":[\"force\"],\"fields\":[{\"json\":\"path\",\"from\":\"flag_or_positional\",\"flag\":\"path\",\"index\":0},{\"json\":\"project\",\"from\":\"flag_or_positional\",\"flag\":\"project\",\"index\":1},{\"json\":\"force\",\"from\":\"flag\",\"flag\":\"force\",\"type\":\"true_if_set\"},{\"json\":\"embedding_command\",\"from\":\"flag\",\"flag\":\"embed\",\"empty\":\"emit\"}]}"},
{"kb.ingest",
 "{\"bool_flags\":[\"force\"],\"fields\":[{\"json\":\"workspace\","
 "\"count_min\":1,\"from\":\"positional\",\"index\":0,\"empty\":\"emit\""
 "},{\"json\":\"force\",\"from\":\"flag\",\"type\":\"true_if_set\","
 "\"flag\":\"force\"},{\"json\":\"embedding_command\",\"from\":\"flag\","
 "\"flag\":\"embed\",\"empty\":\"emit\"}]}"},
{"kb.status",
 "{\"fields\":[{\"json\":\"project\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"empty\":\"emit\"}]}"},
{"kb.update",
 "{\"fields\":[{\"json\":\"path\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"project\","
 "\"count_min\":2,\"from\":\"positional\",\"index\":1,\"empty\":\"emit\""
 "},{\"json\":\"embedding_command\",\"from\":\"flag\",\"flag\":\"embed\""
 ",\"empty\":\"emit\"}]}"},

{"curator.implements",
 "{\"fields\":[{\"json\":\"topic\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"empty\":\"emit\"}]}"},

{"curator.synthesize",
 "{\"fields\":[{\"json\":\"topic\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"empty\":\"emit\"}]}"},

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
 "{\"fields\":[{\"json\":\"cn\",\"count_min\":1,\"from\":\"positional\","
 "\"index\":0,\"empty\":\"emit\"},{\"json\":\"days\",\"from\":\"flag\","
 "\"flag\":\"days\",\"type\":\"number_lenient\"}]}"},

{"job.cancel",
 "{\"fields\":[{\"json\":\"job_id\",\"from\":\"positional_or_flag\","
 "\"index\":0,\"flag\":\"job-id\",\"type\":\"number_lenient\"},{\"json\""
 ":\"reason\",\"from\":\"flag\",\"flag\":\"reason\"}]}"},

{"job.status",
 "{\"fields\":[{\"json\":\"job_id\",\"from\":\"positional_or_flag\","
 "\"index\":0,\"flag\":\"job-id\",\"type\":\"number_lenient\"},{\"json\""
 ":\"reason\",\"from\":\"flag\",\"flag\":\"reason\"}]}"},

{"jobs.cancel",
 "{\"fields\":[{\"json\":\"job_id\",\"from\":\"positional_or_flag\","
 "\"index\":0,\"flag\":\"job-id\",\"type\":\"number_lenient\"},{\"json\""
 ":\"reason\",\"from\":\"flag\",\"flag\":\"reason\"}]}"},

{"jobs.logs",
 "{\"fields\":[{\"json\":\"job_id\",\"from\":\"positional_or_flag\","
 "\"index\":0,\"flag\":\"job-id\",\"type\":\"number_lenient\"},{\"json\""
 ":\"reason\",\"from\":\"flag\",\"flag\":\"reason\"}]}"},

{"jobs.status",
 "{\"fields\":[{\"json\":\"job_id\",\"from\":\"positional_or_flag\","
 "\"index\":0,\"flag\":\"job-id\",\"type\":\"number_lenient\"},{\"json\""
 ":\"reason\",\"from\":\"flag\",\"flag\":\"reason\"}]}"},

{"kb.reembed",
 "{\"bool_flags\":[\"confirm\",\"force\",\"dry-run\","
 "\"clear-maintenance\"],\"fields\":[{\"json\":\"confirm\",\"from\":"
 "\"flag\",\"type\":\"true_if_set\",\"flag\":\"confirm\"},{\"json\":"
 "\"force\",\"from\":\"flag\",\"type\":\"true_if_set\",\"flag\":"
 "\"force\"},{\"json\":\"dry_run\",\"from\":\"flag\",\"type\":"
 "\"true_if_set\",\"flag\":\"dry-run\"},{\"json\":\"clear_maintenance\","
 "\"from\":\"flag\",\"type\":\"true_if_set\",\"flag\":"
 "\"clear-maintenance\"},{\"json\":\"target_dim\",\"from\":\"flag\","
 "\"flag\":\"target-dim\",\"empty\":\"emit\",\"type\":\"number_lenient\""
 "}]}"},

{"rules.delete",
 "{\"fields\":[{\"json\":\"id\",\"from\":\"positional_or_flag\","
 "\"index\":0,\"flag\":\"id\",\"type\":\"number_lenient\"}]}"},

{"job.start",
 "{\"fields\":[{\"json\":\"plan_id\",\"from\":\"positional_or_flag\","
 "\"index\":0,\"flag\":\"plan-id\",\"type\":\"number_lenient\"},{"
 "\"json\":\"parallel\",\"from\":\"flag\",\"flag\":\"parallel\",\"type\""
 ":\"number_lenient\",\"default\":0,\"empty\":\"emit\","
 "\"omit_if_nonpositive\":true}]}"},

{"session.list",
 "{\"fields\":[{\"json\":\"limit\",\"from\":\"flag\",\"flag\":\"limit\","
 "\"type\":\"number_lenient\",\"default\":0,\"empty\":\"emit\","
 "\"omit_if_nonpositive\":true}]}"},

{"curator.contradictions",
 "{\"fields\":[{\"json\":\"limit\",\"from\":\"flag\",\"flag\":\"limit\","
 "\"type\":\"number_lenient\",\"default\":20,\"empty\":\"emit\"}]}"},

{"job.list",
 "{\"fields\":[{\"json\":\"limit\",\"from\":\"flag\",\"flag\":\"limit\","
 "\"type\":\"number_lenient\",\"default\":20,\"empty\":\"emit\"}]}"},

{"jobs.list",
 "{\"fields\":[{\"json\":\"limit\",\"from\":\"flag\",\"flag\":\"limit\","
 "\"type\":\"number_lenient\",\"default\":20,\"empty\":\"emit\"}]}"},

{"notes.search",
 "{\"fields\":[{\"json\":\"query\",\"from\":\"positional_or_flag\","
 "\"index\":0,\"flag\":\"query\",\"empty\":\"emit\"},{\"json\":\"limit\""
 ",\"from\":\"flag\",\"flag\":\"limit\",\"type\":\"number_lenient\","
 "\"default\":20,\"empty\":\"emit\"}]}"},

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
 "{\"fields\":[{\"json\":\"pipeline_id\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"type\":\"number_lenient\"},{\"json\":"
 "\"artifact\",\"from\":\"flag\",\"flag\":\"artifact\"},{\"json\":"
 "\"artifact_hash\",\"from\":\"flag\",\"flag\":\"artifact-hash\"},{"
 "\"json\":\"repo_root\",\"from\":\"flag\",\"flag\":\"repo-root\"},{"
 "\"json\":\"remote\",\"from\":\"flag\",\"flag\":\"remote\"},{\"json\":"
 "\"head_branch\",\"from\":\"flag\",\"flag\":\"head-branch\"},{\"json\":"
 "\"worktree_path\",\"from\":\"flag\",\"flag\":\"worktree-path\"}]}"},

{"pipeline.cancel",
 "{\"fields\":[{\"json\":\"pipeline_id\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"type\":\"number_lenient\"},{\"json\":"
 "\"artifact\",\"from\":\"flag\",\"flag\":\"artifact\"},{\"json\":"
 "\"artifact_hash\",\"from\":\"flag\",\"flag\":\"artifact-hash\"},{"
 "\"json\":\"repo_root\",\"from\":\"flag\",\"flag\":\"repo-root\"},{"
 "\"json\":\"remote\",\"from\":\"flag\",\"flag\":\"remote\"},{\"json\":"
 "\"head_branch\",\"from\":\"flag\",\"flag\":\"head-branch\"},{\"json\":"
 "\"worktree_path\",\"from\":\"flag\",\"flag\":\"worktree-path\"}]}"},

{"pipeline.gate",
 "{\"fields\":[{\"json\":\"pipeline_id\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"type\":\"number_lenient\"},{\"json\":"
 "\"verdict\",\"count_min\":2,\"from\":\"positional\",\"index\":1,"
 "\"empty\":\"emit\"},{\"json\":\"reason\",\"from\":\"flag\",\"flag\":"
 "\"reason\"},{\"json\":\"operator_principal\",\"from\":\"flag\","
 "\"flag\":\"operator-principal\"}]}"},

{"pipeline.list",
 "{\"fields\":[{\"json\":\"state\",\"from\":\"flag\",\"flag\":\"state\"}"
 "]}"},

{"pipeline.resume",
 "{\"fields\":[{\"json\":\"pipeline_id\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"type\":\"number_lenient\"},{\"json\":"
 "\"artifact\",\"from\":\"flag\",\"flag\":\"artifact\"},{\"json\":"
 "\"artifact_hash\",\"from\":\"flag\",\"flag\":\"artifact-hash\"},{"
 "\"json\":\"repo_root\",\"from\":\"flag\",\"flag\":\"repo-root\"},{"
 "\"json\":\"remote\",\"from\":\"flag\",\"flag\":\"remote\"},{\"json\":"
 "\"head_branch\",\"from\":\"flag\",\"flag\":\"head-branch\"},{\"json\":"
 "\"worktree_path\",\"from\":\"flag\",\"flag\":\"worktree-path\"}]}"},

{"pipeline.show",
 "{\"fields\":[{\"json\":\"pipeline_id\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"type\":\"number_lenient\"},{\"json\":"
 "\"artifact\",\"from\":\"flag\",\"flag\":\"artifact\"},{\"json\":"
 "\"artifact_hash\",\"from\":\"flag\",\"flag\":\"artifact-hash\"},{"
 "\"json\":\"repo_root\",\"from\":\"flag\",\"flag\":\"repo-root\"},{"
 "\"json\":\"remote\",\"from\":\"flag\",\"flag\":\"remote\"},{\"json\":"
 "\"head_branch\",\"from\":\"flag\",\"flag\":\"head-branch\"},{\"json\":"
 "\"worktree_path\",\"from\":\"flag\",\"flag\":\"worktree-path\"}]}"},

{"pipeline.status",
 "{\"fields\":[{\"json\":\"pipeline_id\",\"count_min\":1,\"from\":"
 "\"positional\",\"index\":0,\"type\":\"number_lenient\"},{\"json\":"
 "\"artifact\",\"from\":\"flag\",\"flag\":\"artifact\"},{\"json\":"
 "\"artifact_hash\",\"from\":\"flag\",\"flag\":\"artifact-hash\"},{"
 "\"json\":\"repo_root\",\"from\":\"flag\",\"flag\":\"repo-root\"},{"
 "\"json\":\"remote\",\"from\":\"flag\",\"flag\":\"remote\"},{\"json\":"
 "\"head_branch\",\"from\":\"flag\",\"flag\":\"head-branch\"},{\"json\":"
 "\"worktree_path\",\"from\":\"flag\",\"flag\":\"worktree-path\"}]}"},

{"api.enable",
 "{\"bool_flags\":[\"vscode\"],\"fields\":[{\"json\":\"vscode\",\"from\""
 ":\"flag\",\"flag\":\"vscode\",\"type\":\"true_if_set\"},{\"json\":"
 "\"port\",\"from\":\"flag\",\"flag\":\"port\",\"type\":"
 "\"number_lenient\",\"default\":0,\"empty\":\"emit\","
 "\"omit_if_nonpositive\":true},{\"json\":\"rate_limit\",\"from\":"
 "\"flag\",\"flag\":\"rate-limit\",\"type\":\"number_lenient\","
 "\"default\":0,\"empty\":\"emit\",\"omit_if_nonpositive\":true}]}"},

{"session.brief",
 "{\"bool_flags\":[\"list\"],\"fields\":[{\"json\":\"session_id\","
 "\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"session\"},{"
 "\"json\":\"list\",\"from\":\"flag\",\"flag\":\"list\",\"type\":"
 "\"true_if_set\"}]}"},

/* Same verbatim-argv shape as the model family: these dispatch to
   marshal_agent_args too. */

{"workspace.get",
 "{\"fields\":[{\"json\":\"args\",\"from\":\"argv_array\"}]}"},

{"workspace.remove",
 "{\"fields\":[{\"json\":\"args\",\"from\":\"argv_array\"}]}"},

{"index.deps",
 "{\"bool_flags\":[\"review\",\"reverse\",\"dry-run\"],\"fields\":[{"
 "\"json\":\"project\",\"count_min\":1,\"from\":\"positional\",\"index\""
 ":0,\"empty\":\"emit\"},{\"json\":\"min_tier\",\"from\":\"flag\","
 "\"flag\":\"tier\",\"empty\":\"emit\"},{\"json\":\"status\",\"from\":"
 "\"flag\",\"flag\":\"review\",\"type\":\"const_if_set\",\"value\":"
 "\"ambiguous\"},{\"json\":\"direction\",\"from\":\"flag\",\"flag\":"
 "\"reverse\",\"type\":\"const_if_set\",\"value\":\"in\"},{\"json\":"
 "\"dry_run\",\"from\":\"flag\",\"flag\":\"dry-run\",\"type\":"
 "\"true_if_set\"}]}"},

/* The inverted-flag pair: `compress` is true unless --no-compress was given. */

{"trajectory.export",
 "{\"bool_flags\":[\"no-compress\"],\"usage\":\"usage: aimee trajectory export <session_id> [--no-compress] [--max-result-bytes N]\",\"fields\":[{\"json\":\"session_id\",\"from\":\"positional_or_flag\",\"index\":0,\"flag\":\"session\",\"required\":true},{\"json\":\"compress\",\"from\":\"flag\",\"flag\":\"no-compress\",\"type\":\"bool_inverted\"},{\"json\":\"max_result_bytes\",\"from\":\"flag\",\"flag\":\"max-result-bytes\",\"type\":\"number_lenient\",\"default\":512,\"omit_if_nonpositive\":true}]}"},

{"trajectory.batch",
 "{\"bool_flags\":[\"no-compress\"],\"usage\":\"usage: aimee trajectory batch --tasks corpus.jsonl|suite_dir [--toolset-dist research] [--out dir]\",\"fields\":[{\"json\":\"tasks_path\",\"from\":\"flag\",\"flag\":\"tasks\",\"required\":true},{\"json\":\"toolset_dist\",\"from\":\"flag\",\"flag\":\"toolset-dist\"},{\"json\":\"out_dir\",\"from\":\"flag\",\"flag\":\"out\"},{\"json\":\"compress\",\"from\":\"flag\",\"flag\":\"no-compress\",\"type\":\"bool_inverted\"},{\"json\":\"max_result_bytes\",\"from\":\"flag\",\"flag\":\"max-result-bytes\",\"type\":\"number_lenient\",\"default\":512,\"omit_if_nonpositive\":true}]}"},

{"session.close",
 "{\"fields\":[{\"json\":\"session_id\",\"from\":\"positional_or_flag\","
 "\"index\":0,\"flag\":\"session\"}]}"},

{"session.get",
 "{\"fields\":[{\"json\":\"session_id\",\"from\":\"positional_or_flag\","
 "\"index\":0,\"flag\":\"session\"}]}"},

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
 "{\"fields\":[{\"json\":\"name\",\"from\":\"argv_index\",\"index\":0,"
 "\"skip_if_dash\":true}]}"},

/* Reachable only since the served consult moved above marshal_request's
   custom-body block; before that a spec for these could never be reached. */

{"toolset.show",
 "{\"fields\":[{\"json\":\"name\",\"from\":\"argv_index\",\"index\":0,\"required\":true,\"empty\":\"emit\"}]}"},

{"toolset.resolve",
 "{\"fields\":[{\"json\":\"name\",\"from\":\"argv_index\",\"index\":0,\"required\":true,\"empty\":\"emit\"}]}"},

/* One field, two flags, three states. */

{"dogfood.tag",
 "{\"bool_flags\":[\"surprise\",\"no-surprise\"],\"fields\":[{\"json\":\"record_id\",\"from\":\"positional\",\"index\":0,\"empty\":\"emit\"},{\"json\":\"outcome\",\"from\":\"flag\",\"flag\":\"outcome\",\"empty\":\"emit\"},{\"json\":\"notes\",\"from\":\"flag\",\"flag\":\"notes\",\"empty\":\"emit\"},{\"json\":\"richness\",\"from\":\"flag\",\"flag\":\"richness\",\"type\":\"number_lenient\"},{\"json\":\"surprise\",\"from\":\"flag\",\"flag\":\"surprise\",\"false_flag\":\"no-surprise\",\"type\":\"tristate_flag\"}]}"},
