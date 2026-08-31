#!/usr/bin/env python3
"""Provisioning must never hand two instances the same identity.

A duplicate principal_ref means two plugins share one grant; a duplicate event
kind means bus_host_serve_kind() denies the second at attach with nothing in its
own logs to explain why. Both are silent-at-the-wrong-layer failures, so the
allocator is tested rather than trusted.

Run: python3 src/tests/test_provision_plugin_module.py
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
TOOL = os.path.join(REPO, "scripts", "provision-plugin-module.py")

failures = []


def check(cond, msg):
    if cond:
        print(f"  ok: {msg}")
    else:
        print(f"  FAIL: {msg}")
        failures.append(msg)


def provision(cfg, instance, module_bin, extra=()):
    return subprocess.run(
        [sys.executable, TOOL, "--instance", instance,
         "--argv", '["python3","/opt/plugin.py"]',
         "--module-bin", module_bin, "--config-dir", cfg, *extra],
        capture_output=True, text=True, timeout=60)


def grant_fields(cfg, instance, daemon="server", prefix="mcp"):
    path = os.path.join(cfg, "modules.d", daemon, f"{prefix}-{instance}.grant")
    fields = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            m = re.match(r"^([a-z_]+)=(.*)$", line.strip())
            if m:
                fields[m.group(1)] = m.group(2)
    return fields


def env_of(stdout):
    out = {}
    for line in stdout.splitlines():
        if "=" in line and not line.startswith("#"):
            k, _, v = line.partition("=")
            out[k.strip()] = v.strip()
    return out


def main():
    print("test_provision_plugin_module:")
    cfg = tempfile.mkdtemp(prefix="aimee-prov-")
    module_bin = os.path.join(cfg, "aimee-module")
    with open(module_bin, "w", encoding="utf-8") as fh:
        fh.write("#!/bin/sh\n")
    os.chmod(module_bin, 0o755)

    try:
        refs, kinds = set(), set()
        for name in ("github", "jira", "linear", "slack", "notion"):
            r = provision(cfg, name, module_bin)
            check(r.returncode == 0, f"provisioned {name}")
            if r.returncode != 0:
                print(r.stderr)
                continue
            f = grant_fields(cfg, name)
            ef = grant_fields(cfg, name + "-egress")
            ref = int(f["principal_ref"])
            serve = [int(k) for k in f["serve"].split(",")]

            check(ref not in refs, f"{name} got an unused principal_ref ({ref})")
            check(not (set(serve) & kinds), f"{name} got unused event kinds ({serve})")
            check(len(serve) == 2 and serve[1] == serve[0] + 1,
                  f"{name} got an adjacent invoke/declare pair")
            # The kinds are DERIVED from the ref by the canonical module rule,
            # 4096 + ref*256 + stage. There is no second allocation to get wrong.
            check(serve == [4096 + ref * 256 + 1, 4096 + ref * 256 + 2],
                  f"{name} kinds are derived from its principal_ref")
            check(200 <= ref < 456, f"{name} ref is in the reserved plugin band")
            # db1 holds the highest canonical ref (30); its block ends here. A
            # plugin kind at or below it would sit inside a canonical module's
            # block -- the collision this derivation removes.
            check(serve[0] > 4096 + 30 * 256 + 255,
                  f"{name} kinds clear every canonical module block")
            check(f["executable"] == module_bin, f"{name} grant names the real executable")
            check(int(ef["principal_ref"]) == ref + 512,
                  f"{name} egress identity is uniquely derived from its module identity")
            check(ef["request"] == "12291-12294" and ef["serve"] == "",
                  f"{name} egress identity may request only the governed transport stages")
            check(ef["executable"] == module_bin,
                  f"{name} egress grant is pinned to the same executable")

            env = env_of(r.stdout)
            check(env.get("AIMEE_MODULE_PRINCIPAL_REF") == str(ref),
                  f"{name} env ref agrees with its grant")
            # AIMEE_MODULE_EVENT_BASE is retired: the ref determines the kinds,
            # so emitting a base would reintroduce a second source of truth.
            check("AIMEE_MODULE_EVENT_BASE" not in env,
                  f"{name} env no longer carries a separate event base")
            # Least privilege unless asked otherwise.
            check(env.get("AIMEE_MCP_PLUGIN_PERMISSION") == "read",
                  f"{name} defaults to the read ceiling")

            refs.add(ref)
            kinds.update(serve)

        # Re-provisioning is an UPDATE. Handing an existing instance a second
        # identity would strand its old grant and silently change who it is.
        before = grant_fields(cfg, "github")
        r = provision(cfg, "github", module_bin, extra=("--permission", "write"))
        check(r.returncode == 0, "re-provisioning an existing instance succeeds")
        after = grant_fields(cfg, "github")
        check(before["principal_ref"] == after["principal_ref"],
              "re-provisioning keeps the same principal_ref")
        check(before["serve"] == after["serve"], "re-provisioning keeps the same event kinds")
        check(env_of(r.stdout).get("AIMEE_MCP_PLUGIN_PERMISSION") == "write",
              "re-provisioning still applies the new permission")

        # A relative --module-bin can never match /proc/<pid>/exe, so the module
        # would be denied at attach for a reason nothing reports.
        r = subprocess.run(
            [sys.executable, TOOL, "--instance", "rel", "--argv", '["x"]',
             "--module-bin", "aimee-module", "--config-dir", cfg],
            capture_output=True, text=True, timeout=60)
        check(r.returncode != 0 and "absolute" in r.stderr,
              "a relative --module-bin is refused")

        # The instance name becomes a command group, and the registry accepts
        # only [a-z0-9_].
        for bad in ("Github", "has space", "9leading", ""):
            r = subprocess.run(
                [sys.executable, TOOL, "--instance", bad, "--argv", '["x"]',
                 "--module-bin", module_bin, "--config-dir", cfg],
                capture_output=True, text=True, timeout=60)
            check(r.returncode != 0, f"instance name {bad!r} is refused")

        for bad in ('[]', '"notalist"', '["ok",""]', 'not json'):
            r = subprocess.run(
                [sys.executable, TOOL, "--instance", "argvtest", "--argv", bad,
                 "--module-bin", module_bin, "--config-dir", cfg],
                capture_output=True, text=True, timeout=60)
            check(r.returncode != 0, f"--argv {bad!r} is refused")

        # A remote (SSE) instance travels the same wire field, prefixed so the
        # OSV gate does not scan a URL as if it were a package launch.
        r = subprocess.run(
            [sys.executable, TOOL, "--instance", "remote", "--sse-url",
             "https://mcp.example.com/sse", "--bearer-env", "MCP_TOKEN",
             "--module-bin", module_bin, "--config-dir", cfg],
            capture_output=True, text=True, timeout=60)
        check(r.returncode != 0, "an unscoped MCP bearer name is refused")
        r = subprocess.run(
            [sys.executable, TOOL, "--instance", "remote", "--sse-url",
             "https://mcp.example.com/sse", "--bearer-env", "AIMEE_MCP_717_TOKEN",
             "--module-bin", module_bin, "--config-dir", cfg],
            capture_output=True, text=True, timeout=60)
        check(r.returncode == 0, "an SSE instance provisions")
        argv = json.loads(env_of(r.stdout).get("AIMEE_MCP_PLUGIN_ARGV", "[]"))
        check(argv[:1] == ["sse:https://mcp.example.com/sse"], "the SSE endpoint is prefixed")
        egress_ref = int(grant_fields(cfg, "remote-egress")["principal_ref"])
        handle = f"mcp:{egress_ref}"
        check(argv[1:] == [handle], "only an opaque caller-scoped credential handle is carried")
        check(handle == "mcp:717",
              "the handle deterministically resolves only to AIMEE_MCP_717_TOKEN in Vault")

        for bad in (["--sse-url", "ftp://x/y"], ["--argv", '["x"]', "--sse-url", "https://x/y"], []):
            r = subprocess.run(
                [sys.executable, TOOL, "--instance", "badremote", "--module-bin", module_bin,
                 "--config-dir", cfg, *bad],
                capture_output=True, text=True, timeout=60)
            check(r.returncode != 0, f"refused bad transport args {bad}")

        # --dry-run must not write anything.
        r = provision(cfg, "nowrite", module_bin, extra=("--dry-run",))
        check(r.returncode == 0, "dry run succeeds")
        check(not os.path.exists(os.path.join(cfg, "modules.d", "server", "mcp-nowrite.grant")),
              "dry run wrote no grant")
        check(not os.path.exists(os.path.join(cfg, "modules.d", "server", "mcp-nowrite-egress.grant")),
              "dry run wrote no egress grant")
    finally:
        shutil.rmtree(cfg, ignore_errors=True)

    if failures:
        print(f"\n{len(failures)} failure(s)")
        return 1
    print("all provisioning tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
