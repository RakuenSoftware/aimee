#!/usr/bin/env python3
"""Serve exactly ONE pluggy plugin as an MCP server over stdio.

Pluggy is not a protocol. It is a Python hook-dispatch library with no wire
format at all, so "support pluggy" could mean building a second transport, a
second dispatcher, a second audit path and a second security review. This shim
is the alternative: a pluggy plugin becomes an MCP server, and everything
upstream -- the Go plugin module, the command registry, the CLI/RPC/MCP/ACP
surfaces -- is reused unchanged. There is no pluggy-specific code above this
file.

WHAT IT DOES

  * builds a pluggy.PluginManager for --project
  * registers the hookspecs from --spec-module (the host application defines
    hookspecs; a plugin only implements them, so this is required)
  * loads exactly one plugin, either by --dist (a setuptools entry point) or by
    --plugin-module (an importable module)
  * reflects each hookspec into one MCP tool
  * speaks MCP JSON-RPC 2.0 on stdin/stdout: initialize, tools/list, tools/call

HOOK SEMANTICS, stated because they do not map one-to-one:

  * one hookspec -> one tool, named after the hook
  * a `firstresult` hookspec returns its single result
  * a non-`firstresult` hookspec returns the LIST of implementation results, in
    pluggy's own call order
  * hookwrappers are NOT exposed. A wrapper is not a callable surface, and
    pretending otherwise would advertise a tool that cannot be invoked. A plugin
    whose only registrations are wrappers advertises no tools and says so on
    stderr rather than appearing as an empty, broken server.

PINNING. --version and --sha256 are verified BEFORE the plugin is imported.
Importing Python executes it, so a check performed afterwards checks nothing.
"""

import argparse
import hashlib
import importlib
import inspect
import json
import sys


def log(message):
    """Diagnostics go to stderr; stdout is the JSON-RPC frame stream."""
    print(f"aimee-pluggy-host: {message}", file=sys.stderr, flush=True)


def fail(message, code=2):
    log(message)
    sys.exit(code)


def verify_pin(dist_name, want_version, want_sha256):
    """Check the pinned distribution BEFORE importing anything from it."""
    if not dist_name:
        return
    try:
        from importlib import metadata
    except ImportError:  # pragma: no cover - Python < 3.8
        fail("importlib.metadata is unavailable; cannot verify the pin")

    try:
        dist = metadata.distribution(dist_name)
    except metadata.PackageNotFoundError:
        fail(f"distribution {dist_name!r} is not installed; refusing to continue")

    if want_version and dist.version != want_version:
        fail(
            f"{dist_name} is version {dist.version}, pinned to {want_version}; "
            "refusing to load"
        )

    if want_sha256:
        digest = hashlib.sha256()
        # Hash the distribution's own files in a stable order. RECORD may be
        # absent for some install layouts; refuse rather than skip the check,
        # because a pin that silently does not verify is worse than no pin.
        files = sorted(dist.files or [], key=str)
        if not files:
            fail(f"{dist_name} exposes no file list; cannot verify --sha256")
        for entry in files:
            try:
                digest.update(entry.locate().read_bytes())
            except OSError as exc:
                fail(f"cannot read {entry} of {dist_name}: {exc}")
        actual = digest.hexdigest()
        if actual != want_sha256:
            fail(
                f"{dist_name} content hash {actual} does not match the pinned "
                f"{want_sha256}; refusing to load"
            )
        log(f"{dist_name} {dist.version} matched its pin")


def build_manager(project, spec_module, dist_name, plugin_module):
    try:
        import pluggy
    except ImportError:
        fail("pluggy is not importable; this host cannot run without it")

    pm = pluggy.PluginManager(project)

    try:
        specs = importlib.import_module(spec_module)
    except Exception as exc:
        fail(f"cannot import hookspec module {spec_module!r}: {exc}")
    pm.add_hookspecs(specs)

    loaded = 0
    if plugin_module:
        try:
            mod = importlib.import_module(plugin_module)
        except Exception as exc:
            fail(f"cannot import plugin module {plugin_module!r}: {exc}")
        pm.register(mod)
        loaded = 1
    elif dist_name:
        try:
            loaded = pm.load_setuptools_entrypoints(project, name=dist_name)
        except Exception as exc:
            fail(f"cannot load entry points for {dist_name!r}: {exc}")

    if loaded == 0:
        # One plugin per module is the rule; zero is a misconfiguration, and
        # serving an empty tool list would look like a working-but-idle plugin.
        fail("no plugin was loaded; nothing to serve")
    if loaded > 1:
        fail(f"{loaded} plugins loaded; this host serves exactly one")

    return pm


def hook_tools(pm):
    """Reflect hookspecs into MCP tool descriptors.

    Only hooks with at least one non-wrapper implementation are exposed: a hook
    nothing implements is a tool that returns nothing, which is indistinguishable
    from a broken call.
    """
    tools, skipped = [], []
    for name in dir(pm.hook):
        if name.startswith("_"):
            continue
        caller = getattr(pm.hook, name)
        impls = getattr(caller, "get_hookimpls", lambda: [])()
        callable_impls = [i for i in impls if not getattr(i, "hookwrapper", False)
                          and not getattr(i, "wrapper", False)]
        if not callable_impls:
            skipped.append(name)
            continue

        spec = getattr(caller, "spec", None)
        argnames = list(getattr(spec, "argnames", []) or [])
        doc = ""
        if spec is not None and getattr(spec, "function", None) is not None:
            doc = inspect.getdoc(spec.function) or ""

        tools.append({
            "name": name,
            "description": doc,
            "inputSchema": {
                "type": "object",
                # Hook arguments are keyword-only in pluggy and untyped, so the
                # schema names them without asserting types it cannot know.
                "properties": {a: {} for a in argnames},
                "required": [],
            },
            "_argnames": argnames,
            "_firstresult": bool(getattr(spec, "opts", {}).get("firstresult", False))
            if spec is not None else False,
        })
    return tools, skipped


def call_hook(pm, tool, arguments):
    caller = getattr(pm.hook, tool["name"])
    allowed = set(tool["_argnames"])
    unknown = sorted(set(arguments) - allowed)
    if unknown:
        # pluggy raises on unknown kwargs; answering with a clear error beats a
        # traceback the caller cannot act on.
        raise ValueError(f"unknown argument(s) for {tool['name']}: {', '.join(unknown)}")
    missing = sorted(allowed - set(arguments))
    kwargs = dict(arguments)
    for name in missing:
        kwargs[name] = None
    result = caller(**kwargs)
    # firstresult hooks return the single value; the rest return the list.
    return result


def serve(pm, tools):
    """MCP JSON-RPC 2.0 over newline-delimited stdio frames."""
    by_name = {t["name"]: t for t in tools}
    wire_tools = [{k: v for k, v in t.items() if not k.startswith("_")} for t in tools]

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            request = json.loads(line)
        except json.JSONDecodeError:
            continue  # not addressed to us; a frame we cannot parse is not an answer

        rid = request.get("id")
        method = request.get("method")
        params = request.get("params") or {}

        if method == "initialize":
            payload = {"protocolVersion": "2024-11-05",
                       "capabilities": {"tools": {}},
                       "serverInfo": {"name": "aimee-pluggy-host", "version": "1"}}
            reply = {"jsonrpc": "2.0", "id": rid, "result": payload}
        elif method == "tools/list":
            reply = {"jsonrpc": "2.0", "id": rid, "result": {"tools": wire_tools}}
        elif method == "tools/call":
            name = params.get("name")
            tool = by_name.get(name)
            if tool is None:
                reply = {"jsonrpc": "2.0", "id": rid,
                         "error": {"code": -32601, "message": f"no such hook: {name}"}}
            else:
                try:
                    value = call_hook(pm, tool, params.get("arguments") or {})
                    reply = {"jsonrpc": "2.0", "id": rid,
                             "result": {"content": [{"type": "text",
                                                     "text": json.dumps(value, default=str)}],
                                        "structuredContent": value}}
                except Exception as exc:
                    reply = {"jsonrpc": "2.0", "id": rid,
                             "error": {"code": -32000, "message": f"{type(exc).__name__}: {exc}"}}
        elif method is not None and rid is None:
            continue  # a notification; nothing to answer
        else:
            reply = {"jsonrpc": "2.0", "id": rid,
                     "error": {"code": -32601, "message": f"unsupported method: {method}"}}

        sys.stdout.write(json.dumps(reply, default=str) + "\n")
        sys.stdout.flush()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True,
                        help="pluggy project name / entry point group")
    parser.add_argument("--spec-module", required=True,
                        help="importable module defining the hookspecs")
    parser.add_argument("--dist", help="pinned distribution providing the plugin")
    parser.add_argument("--plugin-module",
                        help="importable plugin module (alternative to --dist)")
    parser.add_argument("--version", help="required version of --dist")
    parser.add_argument("--sha256", help="required content hash of --dist")
    args = parser.parse_args()

    if not args.dist and not args.plugin_module:
        fail("one of --dist or --plugin-module is required")

    verify_pin(args.dist, args.version, args.sha256)
    pm = build_manager(args.project, args.spec_module, args.dist, args.plugin_module)
    tools, skipped = hook_tools(pm)
    if skipped:
        log(f"not exposed (no callable implementation): {', '.join(sorted(skipped))}")
    if not tools:
        log("the loaded plugin exposes no callable hooks; serving an empty tool list")
    serve(pm, tools)


if __name__ == "__main__":
    main()
