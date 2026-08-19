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

NEVER = (
    ("getcwd(", "local state"), ("fopen(", "local state"), ("getenv(", "local state"),
    ("vault_client_", "local state"), ("cli_read_file", "local state"),
    ("read_stdin", "local state"), ("build_preload", "local state"),
    ("resolve_session_env", "local state"), ("positionals_joined(", "derived field"),
    ("cJSON_AddItemToObject(req", "nested shape"),
)


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


def local_source(body, var):
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
    m = re.search(rf'{re.escape(var)}\s*=\s*cli_args_get_int\(&opts, "([a-z0-9_-]+)", (\d+)\)', body)
    if m:
        return {"from": "flag", "flag": m.group(1), "type": "number_lenient",
                "default": int(m.group(2))}
    m = re.search(rf'{re.escape(var)}\s*=\s*atoi\(cli_args_get\(&opts, "([a-z0-9_-]+)"\)', body)
    if m:
        return {"from": "flag", "flag": m.group(1), "type": "number_lenient"}
    return None


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
    if re.search(r">= sizeof\(\w+\)\)[^}]*return NULL;", expanded, re.S):
        return None, "length-limit refusal"
    if "const char *method" in params and re.search(r"(strcmp|strncmp)\s*\(\s*method", body):
        return None, "multi-method: branches on the method"
    if re.search(r"else if \(cli_args_get", body):
        return None, "conditional: exclusive flags"
    if "argv[0]" in body:
        return None, "raw argv"
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

    fields, seen = [], set()
    for stmt in re.finditer(
            r'cJSON_Add(String|Number|True|Bool)ToObject\(req, "(\w+)"(?:, ([^;]*))?\);', body):
        kind, name, val = stmt.groups()
        if name in ("method", "protocol_version") or name in seen:
            continue
        val = (val or "").strip()
        f = {"json": name}
        var = re.split(r"[,)\s]", val)[0].strip() if val else ""
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
        elif kind == "True":
            f.update({"from": "flag", "flag": name.replace("_", "-"), "type": "true_if_set"})
        elif "cli_args_get_int(&opts," in val:
            # Inline `cli_args_get_int(&opts, "limit", 20)`. The branch below
            # tested for `cli_args_get(&opts,` which does not match the _int
            # spelling, so five plain limit fields were refused on a substring.
            g = re.search(r'cli_args_get_int\(&opts, "([a-z0-9_-]+)", (-?\d+)\)', val)
            if not g:
                return None, f"unreadable cli_args_get_int for {name}"
            f.update({"from": "flag", "flag": g.group(1), "type": "number_lenient",
                      "default": int(g.group(2))})
        elif "cli_args_get(&opts," in val:
            g = re.search(r'cli_args_get\(&opts, "([a-z0-9_-]+)"\)', val)
            f.update({"from": "flag", "flag": g.group(1)})
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
            srcf = local_source(body, var) if var else None
            if not srcf:
                return None, f"cannot resolve source of {name}"
            f.update(srcf)
            if lenient:
                f["type"] = "number_lenient"
                f.pop("empty", None)
        if kind == "Number" and f.get("type") != "number_lenient":
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
