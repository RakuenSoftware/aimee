"""Derive served argument specs by reading the compiled marshallers.

Run from the repo root. Emits a candidate spec per method, or the reason it
refuses. A candidate is NOT trustworthy until test_cli_argspec compares it
against the real marshaller over generated argv -- that test is the authority,
this is the draft.

It reads required-field refusals, which the spec can already express.

Many marshallers open with

    const char *id = ...;
    if (!id || !id[0])
    {
        fprintf(stderr, "aimee: usage: ...");
        return NULL;
    }

The previous passes saw fprintf/return NULL and refused. But `required: true`
plus `usage` is exactly this, and the interpreter already implements it -- so
the shape was describable all along and I was refusing to look.

Still refuses genuine conditionals: a branch on `method`, a positivity filter,
mutually exclusive flags. Those change WHICH fields exist, not whether a value
was supplied.
"""
import pathlib
import re

SRC = "".join(p.read_text() for p in sorted(pathlib.Path("src").glob("cli_v1_routes*.c")))
FUNCS = {m.group(1): (m.group(2), m.group(3)) for m in re.finditer(
    r"^(?:static )?cJSON \*(marshal_\w+)\(([^)]*)\)\s*\n\{(.*?)^\}", SRC, re.S | re.M)}
# VOID helpers too. marshal_add_memory_scope() adds cwd and $AIMEE_SESSION_ID
# and returns nothing, so indexing only cJSON*-returning functions left it
# invisible -- and memory.list generated a spec that dropped both fields. A
# helper's return type says nothing about whether it touches the request.
FUNCS.update({m.group(1): (m.group(2), m.group(3)) for m in re.finditer(
    r"^(?:static )?void (marshal_\w+)\(([^)]*)\)\s*\n\{(.*?)^\}", SRC, re.S | re.M)})
TABLE = dict(re.findall(r'\{"([a-z0-9_.]+)",\s*(marshal_\w+)\}', SRC))

# getcwd() and resolve_session_env() used to be here. They are not refusals any
# more: the spec can NAME them, and the client supplies the value. What the
# client must not do is DECIDE which commands need them -- that is the knowledge
# that forces a rebuild, and it is the part that moved server-side.
#
# getenv( stays, but only for variables other than the session id: an arbitrary
# environment read is not describable safely, because a vocabulary general
# enough to say "read $X" is general enough to say "read $AWS_SECRET_ACCESS_KEY".
NEVER = (
    ("fopen(", "local state"), ("vault_client_", "local state"),
    ("cli_read_file", "local state"), ("read_stdin", "local state"),
    ("build_preload", "local state"), ("positionals_joined(", "derived field"),

)

# An env read that is NOT the session id. The session resolver is expressible;
# a general getenv is not.
import re as _re
OTHER_ENV = _re.compile(r'getenv\(\s*"(?!AIMEE_SESSION_ID|CLAUDE_SESSION_ID)')


def usage_text(body):
    m = re.search(r'fprintf\(stderr,\s*((?:"(?:[^"\\]|\\.)*"\s*)+)', body)
    if not m:
        return None
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
    txt = "".join(parts).replace("\\n", " ").strip()
    txt = re.sub(r"\s+", " ", txt)
    return txt.replace("aimee: ", "", 1) if txt else None


def required_vars(body):
    """Locals a refusal insists on: if (!x || !x[0]) { ...; return NULL; }"""
    req = set()
    for m in re.finditer(r"if \(([^)]*)\)\s*\n\s*\{[^}]*return NULL;", body, re.S):
        cond = m.group(1)
        if "strcmp" in cond or "pos_count" in cond:
            continue
        for v in re.findall(r"!([a-z_][a-z0-9_]*)\b", cond):
            req.add(v)
    return req


def local_source(body, var, before=None):
    """Where `var` last got its value BEFORE offset `before`.

    Marshallers reuse one scratch variable: kb.search writes
    `if ((v = cli_args_get(&opts, "project")))` and then does the same for
    scope, fusion-mode and embed. Resolving `v` to the first assignment in the
    body gave all four fields the --project flag, so a served kb.search sent
    scope, fusion_mode and embedding_command all carrying whatever --project
    was. Take the assignment nearest above the use, which is what the C means.
    """
    if before is not None:
        head = body[:before]
        # A CASCADE, not a reassignment: `task = cli_args_get(...); if (!task &&
        # pos_count) task = positional[0]; if (!task) task = cli_args_get(...)`.
        # Taking the nearest assignment picks the LAST step and silently drops
        # the earlier ones, which for memory.recall meant a spec that read
        # --query and ignored --task and the positional entirely. Reading part
        # of a rule is worse than refusing it, because it looks like an answer.
        if re.search(rf"if \(!\s*{re.escape(var)}\b", head):
            return None
        # Trim to just after the last assignment to this variable, so the
        # patterns below match that one rather than the earliest.
        # Nearest assignment FIRST, then further back. A clamp is itself an
        # assignment -- worktree.gc writes `days = cli_args_get_int(...)` and
        # then `days = 1;` / `days = 365;` -- so "nearest" alone lands on the
        # clamp and finds no source at all. Walk outwards until one resolves.
        starts = [m.start() for m in re.finditer(rf"\b{re.escape(var)}\s*=", head)]
        for s in reversed(starts):
            got = _local_source_at(body[s:], var)
            if got:
                return got
        return None
    return _local_source_at(body, var)


def _local_source_at(body, var):
    m = re.search(rf'{re.escape(var)}\s*=\s*opts\.pos_count > (\d+) \? opts\.positional\[(\d+)\]\s*'
                  rf':\s*cli_args_get\(&opts, "([a-z0-9_-]+)"\)', body)
    if m:
        return {"from": "positional_or_flag", "index": int(m.group(2)), "flag": m.group(3)}
    m = re.search(rf'{re.escape(var)}\s*=\s*cli_args_get\(&opts, "([a-z0-9_-]+)"\)', body)
    if m:
        return {"from": "flag", "flag": m.group(1)}
    m = re.search(rf'{re.escape(var)}\s*=\s*opts\.positional\[(\d+)\]', body)
    if m:
        return {"from": "positional", "index": int(m.group(1)), "empty": "emit"}
    m = re.search(rf'{re.escape(var)}\s*=\s*cli_args_get_int\(&opts, "([a-z0-9_-]+)", (-?\d+)\)', body)
    if m:
        f = {"from": "flag", "flag": m.group(1), "type": "number_lenient",
             "default": int(m.group(2)), "empty": "emit"}
        # A clamp right after the read: `if (d < 1) d = 1; if (d > 365) d = 365;`
        # worktree.gc and insights.overview both do this, and reading only the
        # default produced a spec that sent --days 9999 through unclamped.
        lo = re.search(rf"if \({re.escape(var)} < (-?\d+)\)\s*\n\s*"
                       rf"{re.escape(var)} = (-?\d+);", body)
        hi = re.search(rf"if \({re.escape(var)} > (-?\d+)\)\s*\n\s*"
                       rf"{re.escape(var)} = (-?\d+);", body)
        if lo:
            f["min"] = int(lo.group(2))
        if hi:
            f["max"] = int(hi.group(2))
        return f
    m = re.search(rf'{re.escape(var)}\s*=\s*atoi\(cli_args_get\(&opts, "([a-z0-9_-]+)"\)', body)
    if m:
        return {"from": "flag", "flag": m.group(1), "type": "number_lenient"}
    return None


def numeric_wrapper(body, name):
    """`AddNumber(req, "x", atof(v))` -- the parse wraps the variable.

    The field loop reads the VALUE, so a field whose value is a parse call over
    a flag variable resolved to nothing at all. Which parse it is matters: atof
    keeps a fraction, strtoul wraps a negative, atoll does neither.
    """
    m = re.search(rf'cJSON_AddNumberToObject\(req, "{re.escape(name)}",\s*'
                  rf'(atof|atoll|strtoul|atoi)\(\s*(\w+)', body)
    if not m:
        return None, None
    kind = {"atof": "number_lenient_real", "atoll": "number_lenient_int64",
            "strtoul": "number_lenient_ulong", "atoi": "number_lenient"}[m.group(1)]
    return kind, m.group(2)



def method_branch(body, method):
    """The statements that actually run for `method` in a shared marshaller.

    marshal_skill_request handles thirteen methods in one function, each in its
    own `if (strcmp(method, "x") == 0) { ... return req; }` block. Judging the
    WHOLE body refused all thirteen because two of them (skill.create,
    skill.edit) read a file -- so eleven methods that never touch a file were
    excluded by their neighbours.

    Every branch returns, so the statements before a branch run for that method
    and for every method whose branch comes later. Drop the other branches and
    keep the rest, in order.
    """
    out, i, n = [], 0, len(body)
    # A branch may name SEVERAL methods: `if (strcmp(method, "skill.create") ==
    # 0 || strcmp(method, "skill.edit") == 0)`. Matching only the single-method
    # form left those two blocks in every other method's slice, so the file read
    # inside them kept refusing eleven methods that never reach it.
    pat = re.compile(r'if \((strcmp\(method, "[a-z0-9_.]+"\) == 0'
                     r'(?:\s*\|\|\s*strcmp\(method, "[a-z0-9_.]+"\) == 0)*)\)\s*')
    while i < n:
        m = pat.search(body, i)
        if not m:
            out.append(body[i:])
            break
        names = re.findall(r'strcmp\(method, "([a-z0-9_.]+)"\)', m.group(1))
        out.append(body[i:m.start()])
        j = m.end()
        if j < n and body[j] == "{":
            depth, k = 0, j
            while k < n:
                if body[k] == "{":
                    depth += 1
                elif body[k] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                k += 1
            block, i = body[j + 1:k], k + 1
        else:
            k = body.find(";", j)
            block, i = body[j:k + 1], k + 1
        if method in names:
            out.append(block)
            # This branch returns; nothing after it runs for this method.
            break
    return "".join(out)


def spec_for(method):
    fn = TABLE.get(method)
    if not fn:
        for pre, h in (("skill.", "marshal_skill_request"), ("model.", "marshal_agent_args"),
                       ("agent.", "marshal_agent_args"), ("pipeline.", "marshal_pipeline_request")):
            if method.startswith(pre):
                fn = h
                break
    if not fn or fn not in FUNCS:
        # A method handled by a custom body inside marshal_request rather than a
        # named marshaller. init.run is the only one left and it reads getcwd(),
        # so it belongs with the client-local-state group rather than in a
        # bucket that reads like "not looked at".
        src = SRC[SRC.index(f'strcmp(method, "{method}")'):][:600] if \
            f'strcmp(method, "{method}")' in SRC else ""
        for needle, why in NEVER:
            if needle in src:
                return None, why
        return None, "custom body in marshal_request"
    params, body = FUNCS[fn]

    # A shared marshaller is judged on the branch that actually runs, not on
    # the union of every method it serves.
    if "const char *method" in params and re.search(r'strcmp\(method, "', body):
        sliced = method_branch(body, method)
        if sliced.strip():
            body = sliced

    # Follow helper calls before judging. The generator used to read only the
    # DIRECT body, so a marshaller calling a helper that adds getcwd() looked
    # clean -- memory.list generated a spec that silently DROPPED the cwd field
    # its marshaller sends. The differential test caught it, but a checker that
    # can only see one level deep will keep producing that class of near-miss,
    # so inline what the body calls and judge the whole thing.
    seen_fns, expanded, queue = {fn}, body, [body]
    while queue:
        cur = queue.pop()
        for callee in re.findall(r"\b(marshal_\w+)\s*\(", cur):
            if callee in seen_fns or callee not in FUNCS:
                continue
            seen_fns.add(callee)
            cbody = FUNCS[callee][1]
            expanded += "\n" + cbody
            queue.append(cbody)

    for needle, why in NEVER:
        if needle in expanded:
            return None, why
    # A nested object under req is still out of reach, EXCEPT the
    # array-of-positionals shape handled in the field loop below.
    if "cJSON_AddItemToObject(req" in expanded and not re.search(
            r"for \(int \w+ = 0; \w+ < opts\.pos_count", expanded):
        return None, "nested shape"
    if OTHER_ENV.search(expanded):
        return None, "reads an arbitrary environment variable"
    if re.search(r">= sizeof\(\w+\)\)[^}]*return NULL;", expanded, re.S):
        return None, "length-limit refusal"
    # A strcmp on the method that only supplies a CONSTANT is not a branch on
    # the method once specs are per-method: skill.pin's `AddBool(req, "pinned",
    # strcmp(method, "skill.pin") == 0)` is the literal true for pin and false
    # for unpin. Judge what is left after those are accounted for.
    # A constant that sits AFTER an early return is not unconditional. skill.pin
    # reaches `AddBool(req, "pinned", ...)` only when `if (argc < 1) return req;`
    # did not fire, so with no arguments the field is absent -- its presence
    # depends on another argument, which is the half of the line that forbids.

    judged = re.sub(r'cJSON_AddBoolToObject\(req, "\w+",\s*'
                    r'strcmp\(method, "[a-z0-9_.]+"\) == 0\);', "", body)
    if re.search(r"(strcmp|strncmp)\s*\(\s*method", judged):
        return None, "multi-method: branches on the method"
    # A branch on the COUNT that changes which field is sent. index.hybrid sends
    # "query" for one positional and "queries" (an array) for several, and
    # delegate.status does the same with job_id/job_ids. The generator happily
    # produced a spec for the scalar half and dropped the array half entirely,
    # so `aimee index hybrid a b` would have searched for "a" alone. Which
    # fields EXIST must not depend on the input; that is the half of the line
    # this crosses.
    # A branch on the COUNT that changes WHICH fields are sent.
    #
    # The first version of this only caught `pos_count == N` together with an
    # array (index.hybrid). marshal_index_file_request uses `pos_count > 1` and
    # no array: two positionals mean <project> <file_path>, one means
    # <file_path> alone. The generated spec put positional[0] in `project`
    # unconditionally, so `aimee index structure <file>` sent the file as a
    # PROJECT and the server answered "missing file_path".
    #
    # The differential test did not catch it, because samples are built from the
    # spec: with two fields it generated two positionals and three, never ONE.
    # Found by driving the real command against a real server.
    # The count branch is DESCRIBED now, via count_min/count_max, not refused.
    # See the note in cli_argspec.c: an arity gate consults the invocation's
    # shape, which max_positionals already does, and not another field's value.
    if re.search(r"else if \(cli_args_get", body):
        return None, "conditional: exclusive flags"
    if re.search(r"for \(int \w+ = 0; \w+ < argc", body) or "argv[i]" in body:
        return None, "raw argv scan"
    # An inverted flag: `compress` defaults true and --no-compress
    # clears it. The vocabulary has no negation, deliberately.
    if re.search(r'cli_args_(has_flag|get)\(&opts, "no-', body):
        return None, "inverted flag"
    # A refusal whose condition combines two DIFFERENT fields is a
    # cross-field rule: trigger.fire needs --source plus (--task OR
    # --proposal). `required` is per-field and cannot say that.
    if re.search(r"is_\w+ =|\|\| *\w+ *&&|&& *\(!\w+ *\|\|", body):
        return None, "cross-field rule"

    req_vars = required_vars(body)
    usage = usage_text(body) if req_vars else None

    bools = []
    m = re.search(r"bool\w*\[\]\s*=\s*\{([^}]*)\}", body)
    if m:
        bools = re.findall(r'"([a-z0-9_-]+)"', m.group(1))

    # A pure delegation (`return marshal_other(...);` as the only statement)
    # carries its fields in the callee. session.close and session.get are two
    # lines each; the field loop saw an empty body and reported "no fields".
    delegated = re.match(r"\s*return (marshal_\w+)\([^;]*\);\s*$", body, re.S)
    if delegated and delegated.group(1) in FUNCS:
        body = FUNCS[delegated.group(1)][1]
        req_vars = required_vars(body)
        usage = usage_text(body) if req_vars else None
        m = re.search(r"bool\w*\[\]\s*=\s*\{([^}]*)\}", body)
        if m:
            bools = re.findall(r'"([a-z0-9_-]+)"', m.group(1))

    # INLINE helper calls at the call SITE, so their fields land in the order
    # the request is actually built. Expanding them at the end (which is what
    # the refusal sweep above does) is enough to JUDGE a body but not to
    # describe one: memory.search calls marshal_add_memory_scope() in the
    # middle, and reading only the direct body dropped project, workspace,
    # scope and cwd from the spec entirely.
    def _inline(src, depth=0):
        if depth > 3:
            return src
        def sub(m):
            callee = m.group(1)
            if callee not in FUNCS:
                return m.group(0)
            inner = _inline(FUNCS[callee][-1], depth + 1)
            # A helper takes `const cli_args_t *opts`, so it writes
            # cli_args_get(opts, ...) and opts->pos_count. Normalise to the
            # caller's spelling or every pattern below misses by one character.
            inner = inner.replace("cli_args_get(opts,", "cli_args_get(&opts,")
            inner = inner.replace("cli_args_get_int(opts,", "cli_args_get_int(&opts,")
            inner = inner.replace("cli_args_has_flag(opts,", "cli_args_has_flag(&opts,")
            inner = inner.replace("opts->", "opts.")
            return "\n" + inner + "\n"
        return re.sub(r"^\s*(marshal_\w+)\(req, &opts\);\s*$", sub, src, flags=re.M)

    body = _inline(body)

    def count_gate(pos_in_body):
        """The positional-count condition ENCLOSING the statement at this offset.

        Brace-aware on purpose. Taking "the last `if (opts.pos_count ...)` seen
        before this point" gated index.hybrid's scope and cwd on the branch they
        come AFTER, because a branch that has closed is not a branch you are in.
        """
        head = body[:pos_in_body]
        gate = {}
        # An early `argc < N` return gates on RAW argv, not on positionals.
        for m in re.finditer(r"if \(argc < (\d+)\)\s*\n\s*return req;", head):
            gate["argc_min"] = max(gate.get("argc_min", 0), int(m.group(1)))

        # Walk back to the innermost block still open at pos_in_body.
        depth, i = 0, len(head) - 1
        while i >= 0:
            c = head[i]
            if c == "}":
                depth += 1
            elif c == "{":
                if depth == 0:
                    m = re.search(
                        r"(else\s+)?if \(opts\.pos_count\s*(==|>=|>)\s*(\d+)\)\s*$",
                        head[:i].rstrip())
                    if m:
                        op, n = m.group(2), int(m.group(3))
                        if op == "==":
                            gate["count_min"] = max(gate.get("count_min", 0), n)
                            gate["count_max"] = n
                        elif op == ">":
                            gate["count_min"] = max(gate.get("count_min", 0), n + 1)
                        else:
                            gate["count_min"] = max(gate.get("count_min", 0), n)
                        if m.group(1):  # else if: exclude the earlier siblings
                            prev = re.findall(
                                r"if \(opts\.pos_count\s*(==|>=|>)\s*(\d+)\)",
                                head[:m.start()])
                            lows = []
                            for pop, pn in prev:
                                pn = int(pn)
                                lows.append(pn + 1 if pop == ">" else pn)
                            # Only a sibling whose threshold is HIGHER than
                            # this branch's constrains it. `if (== 1) ... else
                            # if (> 1)` is already disjoint and needs no upper
                            # bound; `if (> 1) ... else if (> 0)` covers exactly
                            # one and does.
                            mine = gate.get("count_min", 0)
                            higher = [l for l in lows if l > mine]
                            if higher:
                                gate["count_max"] = min(
                                    gate.get("count_max", 1 << 30), min(higher) - 1)
                    break
                depth -= 1
            i -= 1

        # A braceless branch: `if (pos_count == 1)\n   Add(...);` with no block.
        if not any(k in gate for k in ("count_max", "count_min")):
            m = re.search(r"(else\s+)?if \(opts\.pos_count\s*(==|>=|>)\s*(\d+)\)\s*\n\s*$",
                          head)
            if m:
                op, n = m.group(2), int(m.group(3))
                if op == "==":
                    gate["count_min"], gate["count_max"] = n, n
                elif op == ">":
                    gate["count_min"] = n + 1
                else:
                    gate["count_min"] = n
                # A BRACELESS else-if needs the same sibling exclusion as the
                # braced one. index.structure writes `else if (pos_count > 0)`
                # with a single statement and no block, so without this the
                # one-positional file_path had no upper bound and fired
                # alongside the two-positional one, sending the key twice.
                if m.group(1):
                    prev = re.findall(
                        r"if \(opts\.pos_count\s*(==|>=|>)\s*(\d+)\)", head[:m.start()])
                    mine = gate.get("count_min", 0)
                    higher = [(int(pn) + 1 if pop == ">" else int(pn)) for pop, pn in prev]
                    higher = [h for h in higher if h > mine]
                    if higher:
                        gate["count_max"] = min(gate.get("count_max", 1 << 30),
                                                min(higher) - 1)
        return gate

    fields, seen = [], set()
    # An array field is added with AddItemToObject, which the value-shaped
    # pattern below does not match at all -- so memory.search's keywords were
    # not merely mis-sourced, they were invisible.
    array_stmts = [(m.group(1), m.group(2), m.start()) for m in re.finditer(
        r'cJSON_AddItemToObject\(req, "(\w+)", (\w+)\);', body)]
    array_stmts += [(m.group(2), m.group(1), m.start()) for m in re.finditer(
        r'cJSON \*(\w+) = cJSON_AddArrayToObject\(req, "(\w+)"\);', body)]
    for name, arrvar, at in array_stmts:
        if name in seen:
            continue
        built = re.search(
            rf'for \(int \w+ = 0; \w+ < opts\.pos_count; \w+\+\+\)\s*\n\s*'
            rf'cJSON_AddItemToArray\({re.escape(arrvar)}, cJSON_CreateString\(opts\.positional',
            body)
        if not built:
            return None, f"nested shape for {name}"
        arrf = {"json": name, "from": "positional_array"}
        arrf.update(count_gate(at))
        fields.append(arrf)
        seen.add(name)

    for stmt in re.finditer(
            r'cJSON_Add(String|Number|True|Bool)ToObject\(req, "(\w+)"(?:, ([^;]*))?\);', body):
        kind, name, val = stmt.groups()
        if name in ("method", "protocol_version"):
            continue
        val = (val or "").strip()
        f = {"json": name}
        f.update(count_gate(stmt.start()))
        var = re.split(r"[,)\s]", val)[0].strip() if val else ""

        # The two client facts a spec may NAME. Both are recognised by the value
        # the marshaller passes, not by the field's json name, so a field called
        # something else that happens to carry the cwd is still described right
        # -- and a field called "cwd" carrying something else is not silently
        # mislabelled.
        cwd_var = re.search(r"getcwd\((\w+), sizeof", body)
        if cwd_var and var == cwd_var.group(1):
            f["from"] = "cwd"
            fields.append(f)
            seen.add(name)
            continue
        if "resolve_session_env" in val:
            # resolve_session_env never returns NULL -- worst case the literal
            # "default" -- so the field is ALWAYS sent, including as "" when
            # --session was given empty. Dropping it there is a different
            # request from the one the marshaller makes.
            f["from"] = "session"
            f["empty"] = "emit"
            fields.append(f)
            seen.add(name)
            continue

        if re.fullmatch(r"argv\[(\d+)\]", val or ""):
            idx = int(re.fullmatch(r"argv\[(\d+)\]", val).group(1))
            f.update({"from": "argv_index", "index": idx})
            # Read the guard, exactly as the positional branch does. provider.set
            # tests argv[0][0] and DROPS an empty word; mcp.recheck also tests
            # argv[0][0] != '-' and skips a flag-shaped one. Assuming emit here
            # sent name:"" and name:"--json" where the marshaller sends nothing.
            if not re.search(rf"argv\[{idx}\]\s*&&\s*argv\[{idx}\]\[0\]|"
                             rf"argv\[{idx}\]\[0\]\s*(?:&&|\))", body):
                f["empty"] = "emit"
            if re.search(rf"argv\[{idx}\]\[0\]\s*!=\s*'-'", body):
                f["skip_if_dash"] = True
            fields.append(f)
            seen.add(name)
            continue

        if "opts.positional[" in val:
            pm = re.search(r"positional\[(\d+)\]", val)
            if not pm:
                return None, f"non-literal positional index for {name}"
            i = int(pm.group(1))
            # WHICH convention: does the guard require a non-empty value?
            # `pos_count > i && positional[i] && positional[i][0]` DROPS an
            # empty argument; `pos_count > i` alone EMITS it. Assuming either
            # is how provider.show generated a spec that sent "name": "" where
            # the marshaller sent nothing -- caught by the differential test
            # twice before I read the guard instead of guessing it.
            drops_empty = re.search(
                rf"pos_count > {i}[^\n]*positional\[{i}\]\[0\]", body) is not None
            f.update({"from": "positional", "index": i})
            if not drops_empty:
                f["empty"] = "emit"
        elif "cli_args_has_flag" in val:
            g = re.search(r'has_flag\(&opts, "([a-z0-9_-]+)"\)', val)
            f.update({"from": "flag", "flag": g.group(1) if g else name, "type": "bool"})
        elif kind == "Bool" and re.search(r'strcmp\(method, "[a-z0-9_.]+"\) == 0', val):
            want = re.search(r'strcmp\(method, "([a-z0-9_.]+)"\) == 0', val).group(1)
            f.update({"from": "const", "type": "const_bool",
                      "value": (want == method)})
        elif kind == "True":
            # The GUARD names the flag, not the field. provider.list writes
            # `if (cli_args_get(&opts, "available")) AddTrue(req,
            # "available_only")`, and deriving "available-only" from the field
            # name produced a spec that never set it. Derive only as a
            # fallback, and only when no guard is there to read.
            gt = re.search(
                rf'if \(cli_args_(?:has_flag|get)\(&opts, "([a-z0-9_-]+)"\)\)\s*\n\s*'
                rf'cJSON_AddTrueToObject\(req, "{re.escape(name)}"', body)
            f.update({"from": "flag", "type": "true_if_set",
                      "flag": gt.group(1) if gt else name.replace("_", "-")})
        elif "cli_args_get_int(&opts," in val:
            # Inline `cli_args_get_int(&opts, "limit", 20)`. The branch below
            # tested for `cli_args_get(&opts,` which does not match the _int
            # spelling, so five plain limit fields were refused on a substring.
            g = re.search(r'cli_args_get_int\(&opts, "([a-z0-9_-]+)", (-?\d+)\)', val)
            if not g:
                return None, f"unreadable cli_args_get_int for {name}"
            # cli_args_get_int parses whatever is PRESENT, so an empty --limit
            # is atoi("") == 0 and not the default. Without empty:emit the spec
            # substitutes the default and sends a different number.
            f.update({"from": "flag", "flag": g.group(1), "type": "number_lenient",
                      "default": int(g.group(2)), "empty": "emit"})
        elif "cli_args_get(&opts," in val:
            g = re.search(r'cli_args_get\(&opts, "([a-z0-9_-]+)"\)', val)
            f.update({"from": "flag", "flag": g.group(1)})
            # Same empty-vs-drop question the positional branch already asks,
            # and it was only asked there. A flag guarded by `if (v)` SENDS an
            # empty value; one guarded by `if (v && v[0])` drops it. Defaulting
            # to drop made identity.snapshot and index.find_callers omit a field
            # their marshallers send as "" -- caught by the differential test,
            # which is the third time a guard read as a value has produced this
            # exact class of near-miss.
            var_for_flag = var or name
            drops_empty = re.search(
                rf"\b{re.escape(var_for_flag)}\s*&&\s*{re.escape(var_for_flag)}\[0\]|"
                rf"&&\s*{re.escape(var_for_flag)}\[0\]", body) is not None
            if not drops_empty:
                f["empty"] = "emit"
        elif kind == "String" and re.search(
                rf'if \(cli_args_get\(&opts, "[a-z0-9_-]+"\)\)\s*\n\s*'
                rf'cJSON_AddStringToObject\(req, "{re.escape(name)}", "', body):
            # A constant emitted on flag presence: --review sends
            # "status": "ambiguous". true_if_set with a literal other than true.
            g = re.search(
                rf'if \(cli_args_get\(&opts, "([a-z0-9_-]+)"\)\)\s*\n\s*'
                rf'cJSON_AddStringToObject\(req, "{re.escape(name)}", "([^"]*)"', body)
            f.update({"from": "flag", "flag": g.group(1), "type": "const_if_set",
                      "value": g.group(2)})
        elif re.search(
                rf'if \(cli_args_(?:has_flag|get)\(&opts, "([a-z0-9_-]+)"\)\)\s*\n\s*'
                rf'cJSON_Add(?:Bool|True)ToObject\(req, "{re.escape(name)}"', body):
            # `if (has_flag("vscode")) AddBool(req, "vscode", 1)` -- emitted only
            # when the flag is present, which is true_if_set. The generator read
            # the VALUE (a literal 1) and could not resolve it; the guard is
            # where the meaning is.
            g = re.search(
                rf'if \(cli_args_(?:has_flag|get)\(&opts, "([a-z0-9_-]+)"\)\)\s*\n\s*'
                rf'cJSON_Add(?:Bool|True)ToObject\(req, "{re.escape(name)}"', body)
            f.update({"from": "flag", "flag": g.group(1), "type": "true_if_set"})
        else:
            # Unwrap atoi(VAR) / atoi(cli_args_get(...)) before resolving: the
            # naive split yielded "atoi(job_id" and refused six describable
            # methods on a parsing artefact rather than a real shape.
            inner = re.match(r"atoi\(\s*([a-z_][a-z0-9_]*)\s*\)", val)
            lenient = False
            if inner:
                var = inner.group(1)
                lenient = True
            srcf = local_source(body, var, stmt.start()) if var else None
            if not srcf:
                wkind, wvar = numeric_wrapper(body, name)
                if wvar:
                    srcf = local_source(body, wvar, stmt.start())
                    if srcf:
                        f["type"] = wkind
                        lenient = True
            if not srcf:
                if re.search(rf"if \(!\s*{re.escape(var)}\b", body):
                    return None, f"multi-step cascade for {name}"
                return None, f"cannot resolve source of {name}"
            f.update(srcf)
            # The empty-vs-drop guard again, for a field whose value arrives via
            # a VARIABLE rather than an inline call. `const char *out =
            # cli_args_get(...); if (out) Add(...)` sends an empty --out; the
            # variable form is how most marshallers are written, so reading the
            # guard only at the inline call site missed nearly all of them.
            if srcf.get("from") == "flag" and "empty" not in f:
                drops = re.search(
                    rf"\b{re.escape(var)}\s*&&\s*{re.escape(var)}\[0\]|"
                    rf"&&\s*{re.escape(var)}\[0\]", body)
                if not drops:
                    f["empty"] = "emit"
            if lenient:
                # A numeric_wrapper already set the exact parse; only the plain
                # `atoi(...)` path needs the generic one.
                f.setdefault("type", "number_lenient")
                # The empty rule STAYS. identity.diff guards --flip-threshold
                # with `if (ft)`, so an empty value is parsed (atof("") == 0)
                # and sent; dropping the rule because the field is numeric made
                # the spec omit a field the marshaller emits as 0.
        if kind == "Number" and f.get("type") not in (
                "number_lenient", "number_lenient_int64", "number_lenient_real",
                "number_lenient_ulong"):
            if "atoi(" in val or "cli_args_get_int" in val:
                f["type"] = "number_lenient"
                d = re.search(r'cli_args_get_int\(&opts, "[a-z0-9_-]+", (\d+)\)', val)
                if d:
                    f["default"] = int(d.group(1))
            elif re.search(rf'{re.escape(var)}\s*=\s*atoi\(', body):
                f["type"] = "number_lenient"
            else:
                return None, f"unrecognised number source for {name}"
            f.pop("empty", None)
        # `int n = ...; if (n > 0) Add(n);` -- a rule about this field's own
        # value, the numeric parallel of empty:"drop".
        if kind == "Number" and var and re.search(
                rf"if \({re.escape(var)} > 0\)\s*\n\s*cJSON_AddNumberToObject\(req, \"{re.escape(name)}\"",
                body):
            f["omit_if_nonpositive"] = True
        # The same empty-guard question for a value held in a LOCAL: `if (q)`
        # emits an empty string, `if (q && q[0])` drops it. Read it rather than
        # inherit whatever local_source guessed -- notes.search emits an empty
        # query and the spec dropped it, which the differential test caught.
        if var and f.get("from") in ("positional", "positional_or_flag"):
            strict = re.search(rf"if \({re.escape(var)} && {re.escape(var)}\[0\]\)", body)
            loose = re.search(rf"if \({re.escape(var)}\)\s*\n\s*cJSON_Add", body)
            if strict:
                f.pop("empty", None)
            elif loose:
                f["empty"] = "emit"
        if var in req_vars:
            f["required"] = True
            f.pop("empty", None)
        seen.add(name)
        fields.append(f)

    if not fields:
        return None, "no fields"
    # A refusal we could not attribute to a field would be lost.
    if req_vars and not any(f.get("required") for f in fields):
        return None, "refusal not attributable to a field"
    spec = {}
    if bools:
        spec["bool_flags"] = bools
    if usage:
        spec["usage"] = usage
    spec["fields"] = fields
    return spec, fn
