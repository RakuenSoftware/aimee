#!/usr/bin/env python3
"""Provision one plugin-module instance: allocate its identity, write its grant.

A plugin module cannot allocate its own identity. Two things have to be handed to
it, and both are authorization decisions:

  * `principal_ref` -- its bus identity. The grant policy keys on it, so two
    instances sharing one ref share one grant.

That is the ONLY allocation. The instance's event kinds are DERIVED from the ref
by the canonical module rule, kind = 4096 + ref*256 + stage
(docs/modules/README.md), the same rule db1, git, roundtable and economizer
assert in their tests. bus_host_serve_kind() (core/event_bus/bus_route.c:109)
binds one kind to exactly ONE serving slot, so a duplicated kind means a module
is denied at attach with nothing in its own log to say why -- and deriving kinds
from an already-unique ref makes that impossible by construction.

This script used to allocate an event base independently, out of a range at
11264. That was a second allocation authority for one namespace, and it was
wrong: each ref reserves a whole 256-kind block, and 4096 + 28*256 = 11264 is
postgres's block. The range overlapped postgres (28), db2 (29) and db1 (30). A
live run against a real aimee-kb reproduced it -- the plugin was refused at
attach, and in the other order it would have denied postgres instead.

Rerunning for an existing instance is an update, not a second allocation.

The plugin's ARGV is deliberately not scanned here: the daemon runs the OSV
malware gate at admission time (mcp_osv_gate.c), on the argv the module actually
reports, so a scan performed here would be checking a different string.

Usage:
  provision-plugin-module.py --instance github \\
      --argv '["npx","-y","@example/mcp-server"]' \\
      --permission read \\
      --module-bin /usr/local/lib/aimee/aimee-module

Prints the environment the instance must be started with.
"""

import argparse
import json
import os
import re
import sys

# MUST match server-go/modules/mcp/mcp.go and src/headers/module_commands.h.
KIND_ORIGIN = 4096
KIND_STRIDE = 256
STAGE_INVOKE = 1
STAGE_DECLARE = 2

# Principal refs reserved for plugin instances. Canonical module refs are 1..30
# (tests/baselines/modules/canonical-inventory.yaml) and are handed out in order,
# so this band leaves ample room before it is reached; the reservation is recorded
# in that file so no future module is assigned into it. The band is wider than the
# real ceiling -- BUS_HOST_MAX_KINDS is 256 kinds per host, shared with every other
# module -- so the band is never what binds.
# The band MUST match tests/baselines/modules/canonical-inventory.yaml, which
# enforces that no module ref falls inside it.
#
# There was a second band here, [456,512), for external vector database
# providers. It went with them: the searches worth answering outside PostgreSQL
# came to one curator lookup, because memory visibility is a rank over EXISTS
# subqueries, kb reads its generation from a joined table, and code search is
# one leg of a fusion whose other legs are relational. The band stays RESERVED
# in the inventory rather than reused, so a ref from a grant written before the
# removal can never collide with a plugin allocated after it.
KINDS = {
    "plugin": {
        "first": 200,
        "limit": 456,
        "prefix": "mcp",
        "what": "plugin instances",
    },
}

INSTANCE_RE = re.compile(r"^[a-z][a-z0-9]*(?:[-_][a-z0-9]+)*$")
GRANT_RE = re.compile(r"^\s*([a-z_]+)\s*=\s*(.*?)\s*$")


def die(message):
    print(f"provision-plugin-module: {message}", file=sys.stderr)
    sys.exit(2)


def read_grants(policy_dir):
    """Parse every .grant in the policy dir into {filename: {key: value}}."""
    out = {}
    if not os.path.isdir(policy_dir):
        return out
    for name in sorted(os.listdir(policy_dir)):
        if not name.endswith(".grant"):
            continue
        fields = {}
        with open(os.path.join(policy_dir, name), encoding="utf-8") as fh:
            for line in fh:
                m = GRANT_RE.match(line)
                if m:
                    fields[m.group(1)] = m.group(2)
        out[name] = fields
    return out


def kinds_for(ref):
    """The (invoke, declare) event kinds a principal ref owns."""
    base = KIND_ORIGIN + ref * KIND_STRIDE
    return base + STAGE_INVOKE, base + STAGE_DECLARE


def taken(grants, self_name):
    """Refs and event kinds already claimed by OTHER instances."""
    refs, kinds = set(), set()
    for name, fields in grants.items():
        if name == self_name:
            continue  # our own previous allocation is ours to keep
        try:
            refs.add(int(fields.get("principal_ref", "0")))
        except ValueError:
            pass
        for k in (fields.get("serve") or "").split(","):
            k = k.strip()
            if k.isdigit():
                kinds.add(int(k))
    return refs, kinds


def allocate(grants, self_name, kind):
    """Pick the lowest free principal ref. Its kinds follow from it.

    `kinds` is still consulted, but only as a consistency check: a ref whose
    derived kinds are already served by some OTHER grant means that grant was
    written by hand or by an older scheme, and silently reusing the ref would
    reintroduce exactly the collision this derivation removes.
    """
    refs, kinds = taken(grants, self_name)
    band = KINDS[kind]

    for ref in range(band["first"], band["limit"]):
        if ref in refs:
            continue
        invoke, declare = kinds_for(ref)
        if invoke in kinds or declare in kinds:
            continue
        return ref, invoke, declare
    die(f"no free principal_ref remains in the band reserved for {band['what']} "
        f"[{band['first']},{band['limit']})")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--instance", required=True,
                    help="instance name; becomes the command group (e.g. 'github')")
    ap.add_argument("--argv",
                    help="local plugin: the command line, as a JSON array of strings")
    ap.add_argument("--sse-url",
                    help="remote plugin: the MCP SSE endpoint (alternative to --argv)")
    ap.add_argument("--bearer-env",
                    help="with --sse-url: NAME of the env var holding the bearer token. "
                         "The name travels, never the secret -- the declared argv is "
                         "reported over the bus and logged.")
    ap.add_argument("--module-bin", required=True,
                    help="absolute path to the aimee-module executable")
    ap.add_argument("--permission", default="read",
                    choices=["read", "write", "execute", "dangerous"],
                    help="ceiling for this instance's tools (default: read, least privilege)")
    ap.add_argument("--cwd", default="", help="working directory for the plugin")
    ap.add_argument("--daemon", default="server", help="daemon hosting it (default: server)")
    ap.add_argument("--kind", default="plugin", choices=sorted(KINDS),
                    help="what is being provisioned (default: plugin)")
    ap.add_argument("--config-dir", default=os.path.expanduser("~/.config/aimee"),
                    help="aimee config directory")
    ap.add_argument("--dry-run", action="store_true", help="print, write nothing")
    args = ap.parse_args()

    if not INSTANCE_RE.match(args.instance):
        die(f"instance {args.instance!r} must match [a-z][a-z0-9]*([-_][a-z0-9]+)* -- it becomes a "
            "command group, and the registry accepts only [a-z0-9_]")

    if bool(args.argv) == bool(args.sse_url):
        die("give exactly one of --argv (a local process) or --sse-url (a remote endpoint)")

    if args.sse_url:
        if not args.sse_url.startswith(("http://", "https://")):
            die("--sse-url must be an http(s) URL")
        # SSEPrefix in server-go/modules/mcp/mcp.go. The prefix keeps one wire
        # field doing one job: the daemon's OSV gate reads argv[0] as an
        # executable, and a bare URL there would be scanned as a package launch.
        argv = ["sse:" + args.sse_url]
        if args.bearer_env:
            argv.append(args.bearer_env)
    else:
        try:
            argv = json.loads(args.argv)
        except json.JSONDecodeError as exc:
            die(f"--argv is not valid JSON: {exc}")
        if not isinstance(argv, list) or not argv or not all(isinstance(a, str) and a for a in argv):
            die("--argv must be a non-empty JSON array of non-empty strings")
        if args.bearer_env:
            die("--bearer-env applies to --sse-url only")

    if not os.path.isabs(args.module_bin):
        # The grant checks the peer's /proc/<pid>/exe against this exact string,
        # so a relative path can never match and the module is denied at attach.
        die("--module-bin must be an absolute path (the grant compares it to /proc/<pid>/exe)")
    if not args.dry_run and not os.path.exists(args.module_bin):
        die(f"--module-bin {args.module_bin} does not exist")

    policy_dir = os.path.join(args.config_dir, "modules.d", args.daemon)
    band = KINDS[args.kind]
    grant_name = f"{band['prefix']}-{args.instance}.grant"
    grants = read_grants(policy_dir)

    existing = grants.get(grant_name)
    if existing and existing.get("principal_ref", "").isdigit():
        ref = int(existing["principal_ref"])
        if not band["first"] <= ref < band["limit"]:
            die(f"{grant_name} names principal_ref={ref}, outside the band reserved "
                f"for {band['what']} [{band['first']},{band['limit']}); remove it and rerun")
        invoke, declare = kinds_for(ref)
        # Rewrite the serve list from the derivation rather than trusting what is
        # on disk. A grant written by the old scheme names kinds in postgres's,
        # db2's or db1's block; keeping them would leave the instance either
        # denied at attach or, worse, denying a core module.
        old = sorted(int(k) for k in (existing.get("serve") or "").split(",")
                     if k.strip().isdigit())
        if old and old != [invoke, declare]:
            print(f"rewriting {grant_name}: serve={','.join(str(k) for k in old)} -> "
                  f"{invoke},{declare} (kinds are now derived from principal_ref)",
                  file=sys.stderr)
        print(f"reusing the existing allocation for {args.instance}: principal_ref={ref}",
              file=sys.stderr)
    else:
        ref, invoke, declare = allocate(grants, grant_name, args.kind)

    publish = ""
    subscribe = ""
    serve = f"{invoke},{declare}"

    grant = "\n".join([
        "version=1",
        "principal_class=1",
        f"principal_ref={ref}",
        "uid=self",
        f"executable={args.module_bin}",
        f"publish={publish}",
        f"subscribe={subscribe}",
        "request=",
        f"serve={serve}",
    ]) + "\n"

    grant_path = os.path.join(policy_dir, grant_name)
    if args.dry_run:
        print(f"--- would write {grant_path} ---")
        print(grant, end="")
    else:
        os.makedirs(policy_dir, mode=0o700, exist_ok=True)
        with open(grant_path, "w", encoding="utf-8") as fh:
            fh.write(grant)
        os.chmod(grant_path, 0o600)
        print(f"wrote {grant_path}", file=sys.stderr)

    # The executable name is what selects the module: aimee-module is a multicall
    # binary dispatching on argv[0].
    link = f"aimee-module-mcp-{args.instance}"
    print("")
    print(f"# start {link} with this environment (it must symlink to {args.module_bin}):")
    print(f"AIMEE_MODULE_PRINCIPAL_REF={ref}")
    print(f"AIMEE_MCP_PLUGIN_ARGV={json.dumps(argv)}")
    print(f"AIMEE_MCP_PLUGIN_PERMISSION={args.permission}")
    if args.cwd:
        print(f"AIMEE_MCP_PLUGIN_CWD={args.cwd}")
    print("")
    print("# the plugin is NOT started until the daemon's OSV gate admits its argv;")
    print("# check state with: curl -s $AIMEE_API/v1/dashboard/metrics | jq .plugins")
    return 0


if __name__ == "__main__":
    sys.exit(main())
