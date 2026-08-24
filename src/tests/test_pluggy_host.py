#!/usr/bin/env python3
"""Drive aimee-pluggy-host.py the way the Go MCP module does: JSON-RPC on stdio.

This is the claim under test -- that a pluggy plugin IS an MCP server, so no
pluggy-specific transport, dispatch or audit code exists anywhere above the
shim. If this passes, the Go module needs no changes to host pluggy.

Run: python3 src/tests/test_pluggy_host.py   (needs pluggy on PYTHONPATH)
"""

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
HOST = os.path.join(REPO, "scripts", "aimee-pluggy-host.py")
FIXTURES = os.path.join(HERE, "fixtures", "pluggy")

failures = []


def check(condition, message):
    if condition:
        print(f"  ok: {message}")
    else:
        print(f"  FAIL: {message}")
        failures.append(message)


class Host:
    def __init__(self, extra_args=()):
        env = dict(os.environ)
        env["PYTHONPATH"] = FIXTURES + os.pathsep + env.get("PYTHONPATH", "")
        self.proc = subprocess.Popen(
            [sys.executable, HOST, "--project", "aimee_demo",
             "--spec-module", "aimee_demo_spec",
             "--plugin-module", "aimee_demo_plugin", *extra_args],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, env=env)
        self.next_id = 1

    def call(self, method, params=None):
        rid = self.next_id
        self.next_id += 1
        frame = {"jsonrpc": "2.0", "id": rid, "method": method, "params": params or {}}
        self.proc.stdin.write(json.dumps(frame) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("host closed stdout: " + self.proc.stderr.read())
        return json.loads(line)

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


def main():
    print("test_pluggy_host:")

    host = Host()
    try:
        init = host.call("initialize", {"protocolVersion": "2024-11-05"})
        check(init.get("result", {}).get("protocolVersion") == "2024-11-05",
              "the shim completes the MCP handshake")

        listed = host.call("tools/list")
        tools = {t["name"]: t for t in listed["result"]["tools"]}
        check(set(tools) == {"greet", "pick_one"},
              f"only implemented hooks become tools (got {sorted(tools)})")
        # A wrapper is not a callable surface; a hook nothing implements would
        # be a tool that always returns nothing.
        check("wrapped_only" not in tools, "a wrapper-only hook is not exposed as a tool")
        check("never_implemented" not in tools, "an unimplemented hook is not exposed")
        check(tools["greet"]["description"] == "Return a greeting for name.",
              "the hookspec docstring becomes the tool description")
        check("name" in tools["greet"]["inputSchema"]["properties"],
              "hook argument names reach the tool schema")
        check(all(not k.startswith("_") for k in tools["greet"]),
              "internal reflection fields are not put on the wire")

        # A plain hookspec returns every implementation's result, in pluggy's
        # own order; a firstresult hookspec returns the single value.
        res = host.call("tools/call", {"name": "greet", "arguments": {"name": "ada"}})
        check(res["result"]["structuredContent"] == ["hello ada"],
              "a plain hook returns the LIST of implementation results")

        res = host.call("tools/call",
                        {"name": "pick_one", "arguments": {"options": ["a", "b"]}})
        check(res["result"]["structuredContent"] == "a",
              "a firstresult hook returns the single result")

        res = host.call("tools/call", {"name": "nope", "arguments": {}})
        check("error" in res, "an unknown hook is an error, not a silent empty result")

        res = host.call("tools/call",
                        {"name": "greet", "arguments": {"name": "ada", "bogus": 1}})
        check("error" in res and "bogus" in res["error"]["message"],
              "an unknown argument is reported by name")

        res = host.call("unsupported/method")
        check("error" in res, "an unsupported method is refused")
    finally:
        host.close()

    # Pinning is verified BEFORE import, because importing executes the code.
    env = dict(os.environ)
    env["PYTHONPATH"] = FIXTURES + os.pathsep + env.get("PYTHONPATH", "")
    proc = subprocess.run(
        [sys.executable, HOST, "--project", "aimee_demo",
         "--spec-module", "aimee_demo_spec", "--plugin-module", "aimee_demo_plugin",
         "--dist", "pluggy", "--version", "0.0.0-not-real"],
        capture_output=True, text=True, env=env, timeout=60)
    check(proc.returncode != 0 and "pinned to" in proc.stderr,
          "a version pin mismatch refuses to start")

    proc = subprocess.run(
        [sys.executable, HOST, "--project", "aimee_demo",
         "--spec-module", "aimee_demo_spec", "--plugin-module", "aimee_demo_plugin",
         "--dist", "no-such-distribution-anywhere"],
        capture_output=True, text=True, env=env, timeout=60)
    check(proc.returncode != 0 and "not installed" in proc.stderr,
          "a missing pinned distribution refuses to start")

    proc = subprocess.run(
        [sys.executable, HOST, "--project", "aimee_demo",
         "--spec-module", "aimee_demo_spec"],
        capture_output=True, text=True, env=env, timeout=60)
    check(proc.returncode != 0, "no plugin selected refuses to start")

    if failures:
        print(f"\n{len(failures)} failure(s)")
        return 1
    print("all pluggy host tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
