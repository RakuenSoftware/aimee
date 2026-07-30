"""Role- and tier-decouple coverage for scripts/aimee-llm-supervisor.sh.

Runs the REAL supervisor with PATH shims for the external commands it drives —
`curl` (records fetch URLs, creates the dest), the llama binary (records its full
argv, one line per launch), `python3` (the gateway: dumps its env then exits so
the supervisor's `wait -n` returns), and `sha256sum` (no-op). Each combo then
asserts which models were downloaded, which llama-servers were launched (and with
which per-role tier model / --ngl / --parallel), and which per-role upstream the
gateway was pointed at.

Invariants under test:
  - a role that is not `local` is neither downloaded nor launched;
  - all-external/off downloads nothing and starts no llama-server;
  - each local role is sized by its OWN tier (one instance can mix e.g. a cpu
    embedder and a gpu-large synth), with GPU offload forced off on a cpu tier.
"""
import os
import pathlib
import re
import shlex
import subprocess
import sys
import tempfile
import textwrap
import unittest

SUP = pathlib.Path(__file__).resolve().parents[1] / "aimee-llm-supervisor.sh"
GATEWAY = pathlib.Path(__file__).resolve().parents[1] / "aimee_llm_gateway.py"
REAL_PYTHON = sys.executable


def _descriptor(model=None):
    """The registry entry as the supervisor sees it. Read through the same descriptor
    call rather than restating coordinates here — a test that hardcodes them is a second
    copy of exactly the data scripts/embedders.json exists to centralise."""
    argv = [REAL_PYTHON, str(GATEWAY), "--embedder-descriptor"] + ([model] if model else [])
    out = subprocess.run(argv, capture_output=True, text=True, check=True).stdout
    fields = {}
    for line in out.splitlines():
        key, _, value = line.partition("=")
        fields[key] = shlex.split(value)[0] if value else ""
    return fields


EMBED = _descriptor()


def _write(path, body):
    path.write_text(body)
    path.chmod(0o755)


def _make_shims(tmp):
    bindir = tmp / "bin"
    logdir = tmp / "log"
    bindir.mkdir()
    logdir.mkdir()
    _write(bindir / "curl", textwrap.dedent(f"""\
        #!/usr/bin/env bash
        dest=""; url=""
        while [ $# -gt 0 ]; do
          case "$1" in
            -o) dest="$2"; shift 2;;
            -*) shift;;
            *) url="$1"; shift;;
          esac
        done
        [ -n "$dest" ] && : > "$dest"
        echo "$url" >> "{logdir}/fetch.log"
        [ "${{FETCH_FAIL_AFTER_WRITE:-0}}" = 1 ] && exit 1
        exit 0
        """))
    # llama-server: record the full argv (one line), then detach from the captured
    # pipe and block, so the supervisor's exit — not `sleep` — ends the test call.
    _write(bindir / "llama", textwrap.dedent(f"""\
        #!/usr/bin/env bash
        echo "$*" >> "{logdir}/llama.log"
        exec sleep 30 >/dev/null 2>&1
        """))
    # python3 wears two hats here. Serving the gateway is shimmed (dump the env, exit,
    # so the supervisor's `wait -n` returns) — but the same interpreter also answers
    # `--embedder-descriptor`, which the supervisor evals for the embedder's coordinates,
    # pooling and context. That call must run the REAL gateway: shimming it out would
    # test a supervisor that resolves its embedder from nothing.
    _write(bindir / "python3", textwrap.dedent(f"""\
        #!/usr/bin/env bash
        for arg in "$@"; do
          if [ "$arg" = "--embedder-descriptor" ]; then exec {REAL_PYTHON} "$@"; fi
        done
        env > "{logdir}/gateway_env.log"
        # Grace period, not decoration. This shim exiting is what makes the supervisor's
        # `wait -n` return, and the supervisor then kills its children — so exiting
        # immediately races the just-forked llama shims before they append their argv,
        # and a launch intermittently vanishes from llama.log. Outlive that write.
        sleep 0.25
        exit 0
        """))
    _write(bindir / "sha256sum", "#!/usr/bin/env bash\ncat >/dev/null 2>&1 || true\nexit 0\n")
    return bindir, logdir


def _port(argv):
    m = re.search(r"--port (\d+)", argv)
    return m.group(1) if m else None


def _ngl(argv):
    m = re.search(r"-ngl (\S+)", argv)
    return m.group(1) if m else None


class SupervisorRoleTest(unittest.TestCase):
    def run_supervisor(self, modes=None, extra_env=None):
        tmp = pathlib.Path(tempfile.mkdtemp(prefix="sup-roles-"))
        bindir, logdir = _make_shims(tmp)
        env = dict(os.environ)
        env["PATH"] = f"{bindir}:{env['PATH']}"
        env["AIMEE_LLM_LLAMA_BIN"] = str(bindir / "llama")
        env["AIMEE_LLM_MODELS_DIR"] = str(tmp / "models")
        env["AIMEE_LLM_NGL"] = "0"
        # The image path /opt/aimee/... doesn't exist in a checkout.
        env["AIMEE_LLM_GATEWAY"] = str(GATEWAY)
        env.pop("AIMEE_LLM_STUB", None)
        for k in ("AIMEE_LLM_TIER", "AIMEE_LLM_EMBED_MODE",
                  "AIMEE_LLM_SYNTH_MODE", "AIMEE_LLM_SYNTH_LOCAL", "AIMEE_LLM_EMBED_URL",
                  "AIMEE_LLM_SYNTH_URL", "AIMEE_LLM_EMBED_TIER", "AIMEE_LLM_SYNTH_TIER",
                  # An operator override in the ambient env would mask the registry.
                  "AIMEE_LLM_EMBED_MODEL", "AIMEE_LLM_EMBED_POOLING", "AIMEE_LLM_EMBED_CTX"):
            env.pop(k, None)
        for k, v in (modes or {}).items():
            env[k] = v
        if extra_env:
            env.update(extra_env)
        proc = subprocess.run(["bash", str(SUP)], env=env, capture_output=True,
                              text=True, timeout=30)

        def lines(name):
            p = logdir / name
            return p.read_text().splitlines() if p.exists() else []

        gw = {}
        ge = logdir / "gateway_env.log"
        if ge.exists():
            for line in ge.read_text().splitlines():
                if "=" in line:
                    key, val = line.split("=", 1)
                    gw[key] = val
        return proc, lines("fetch.log"), lines("llama.log"), gw

    def _launched(self, llama):
        return {_port(a): a for a in llama}

    # ---- role mode (local | external | off) --------------------------------
    def test_all_local_downloads_and_launches_all(self):
        proc, urls, llama, gw = self.run_supervisor()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertTrue(any(EMBED["EMBED_FILE"] in u for u in urls), urls)  # registry embed
        self.assertTrue(any("gemma-4-E4B" in u for u in urls), urls)      # cpu synth
        self.assertEqual(set(self._launched(llama)), {"8081", "8083"})
        self.assertEqual(gw.get("AIMEE_LLM_EMBED_URL"), "http://127.0.0.1:8081")
        self.assertEqual(gw.get("AIMEE_LLM_SYNTH_URL"), "http://127.0.0.1:8083")

    def test_completed_download_survives_resumer_failure(self):
        # A resumer may report non-zero when it sees an already-complete file left
        # by a process that died just before the ready marker was written. A valid
        # pinned artifact must still be accepted instead of restart-looping.
        proc, urls, llama, _gw = self.run_supervisor(
            modes={"AIMEE_LLM_SYNTH_MODE": "off"},
            extra_env={"FETCH_FAIL_AFTER_WRITE": "1"})
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(len(urls), 1, urls)
        self.assertEqual(set(self._launched(llama)), {"8081"})
        self.assertIn("complete despite downloader status", proc.stderr)

    def test_external_embed_skips_embed_download_and_server(self):
        proc, urls, llama, gw = self.run_supervisor(
            modes={"AIMEE_LLM_EMBED_MODE": "external",
                   "AIMEE_LLM_EMBED_URL": "http://ext-embed:9000"})
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertFalse(any(EMBED["EMBED_FILE"] in u for u in urls), urls)
        self.assertTrue(any("gemma" in u.lower() for u in urls), urls)
        self.assertEqual(set(self._launched(llama)), {"8083"})
        self.assertEqual(gw.get("AIMEE_LLM_EMBED_URL"), "http://ext-embed:9000")

    def test_synth_off_gates_the_role(self):
        proc, urls, llama, gw = self.run_supervisor(modes={"AIMEE_LLM_SYNTH_MODE": "off"})
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertFalse(any("gemma" in u.lower() for u in urls), urls)
        self.assertNotIn("8083", self._launched(llama))
        self.assertEqual(gw.get("AIMEE_LLM_SYNTH_URL", "X"), "")

    def test_all_external_downloads_nothing_and_starts_no_server(self):
        proc, urls, llama, _gw = self.run_supervisor(modes={
            "AIMEE_LLM_EMBED_MODE": "external", "AIMEE_LLM_EMBED_URL": "http://e:1",
            "AIMEE_LLM_SYNTH_MODE": "external", "AIMEE_LLM_SYNTH_URL": "http://s:3",
        })
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(urls, [], urls)
        self.assertEqual(llama, [], llama)

    def test_external_without_url_fails_fast(self):
        proc, *_ = self.run_supervisor(modes={"AIMEE_LLM_EMBED_MODE": "external"})
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("mode=external requires", proc.stderr)

    def test_legacy_synth_local_zero_is_external(self):
        proc, urls, llama, gw = self.run_supervisor(
            modes={"AIMEE_LLM_SYNTH_LOCAL": "0", "AIMEE_LLM_SYNTH_URL": "http://legacy:8"})
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertFalse(any("gemma" in u.lower() for u in urls), urls)
        self.assertNotIn("8083", self._launched(llama))
        self.assertEqual(gw.get("AIMEE_LLM_SYNTH_URL"), "http://legacy:8")

    def test_invalid_mode_rejected(self):
        proc, *_ = self.run_supervisor(modes={"AIMEE_LLM_EMBED_MODE": "bogus"})
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("invalid AIMEE_LLM_EMBED_MODE", proc.stderr)

    # ---- per-role tier -----------------------------------------------------
    def test_global_tier_applies_to_all_roles(self):
        proc, urls, llama, _gw = self.run_supervisor(
            modes={"AIMEE_LLM_TIER": "mid", "AIMEE_LLM_NGL": "99"})
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertTrue(any(EMBED["EMBED_FILE"] in u for u in urls), urls)  # registry embed
        self.assertTrue(any("gemma-4-26B" in u for u in urls), urls)      # mid synth
        launched = self._launched(llama)
        self.assertEqual(_ngl(launched["8081"]), "99")                    # gpu embed offloads
        self.assertIn("--parallel 2", launched["8083"])                   # mid => SLOTS=2

    def test_mixed_per_role_tiers_in_one_instance(self):
        # cpu embedder beside a gpu-large synth on the same instance.
        proc, urls, llama, gw = self.run_supervisor(modes={
            "AIMEE_LLM_EMBED_TIER": "cpu",
            "AIMEE_LLM_SYNTH_TIER": "large",
            "AIMEE_LLM_NGL": "99",
        })
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertTrue(any(EMBED["EMBED_FILE"] in u for u in urls), urls)  # registry embed
        self.assertTrue(any("gemma-4-26B" in u for u in urls), urls)      # large synth model
        launched = self._launched(llama)
        self.assertEqual(_ngl(launched["8081"]), "0")                     # cpu embed: no offload
        self.assertEqual(_ngl(launched["8083"]), "99")                    # gpu synth: offload
        self.assertIn("--parallel 4", launched["8083"])                   # large => SLOTS=4

    # ---- the embedder registry drives provisioning + serving flags ----------
    def test_serving_flags_come_from_the_registry(self):
        # pooling and context are the two silent failures: the wrong pooling yields
        # well-formed wrong vectors, and a context past the model's trained positions
        # embeds the tail to noise. Both must trace to the registry entry, not a default.
        proc, _urls, llama, gw = self.run_supervisor()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        embed = self._launched(llama)["8081"]
        self.assertIn(f"--pooling {EMBED['EMBED_POOLING']}", embed)
        self.assertIn(f"--ctx-size {EMBED['EMBED_CONTEXT']}", embed)
        self.assertEqual(gw.get("AIMEE_LLM_EMBED_POOLING"), EMBED["EMBED_POOLING"])
        self.assertEqual(gw.get("AIMEE_LLM_EMBED_MODEL"), EMBED["EMBED_MODEL_KEY"])

    def test_every_tier_serves_the_same_embedder(self):
        # The uniform-dimension claim: tier is a GPU-offload decision only, so an index
        # built on one tier stays readable on another. If tier ever reintroduced a
        # per-tier model, this is the test that catches the silent re-embed requirement.
        fetched = {}
        for tier in ("cpu", "small", "mid", "large"):
            proc, urls, _llama, _gw = self.run_supervisor(modes={"AIMEE_LLM_EMBED_TIER": tier})
            self.assertEqual(proc.returncode, 0, proc.stderr)
            fetched[tier] = [u for u in urls if EMBED["EMBED_FILE"] in u]
        self.assertEqual(len({tuple(v) for v in fetched.values()}), 1, fetched)

    def test_unregistered_embedder_aborts_the_boot(self):
        proc, urls, llama, _gw = self.run_supervisor(
            extra_env={"AIMEE_LLM_EMBED_MODEL": "some-unmeasured-model"})
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("is not in", proc.stderr)
        self.assertEqual(urls, [], "nothing may be fetched for an unregistered embedder")
        self.assertEqual(llama, [], llama)

    def test_registered_but_unprovisioned_embedder_aborts_the_boot(self):
        # An entry may exist for a model that was measured but never deployed; its
        # coordinates are blank. Serving it must fail here, not in the middle of a fetch.
        proc, urls, _llama, _gw = self.run_supervisor(
            extra_env={"AIMEE_LLM_EMBED_MODEL": "bekko-a25m"})
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("cannot be served", proc.stderr)
        self.assertEqual(urls, [], urls)

    def test_invalid_tier_rejected(self):
        proc, *_ = self.run_supervisor(modes={"AIMEE_LLM_SYNTH_TIER": "bogus"})
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("invalid AIMEE_LLM_SYNTH_TIER", proc.stderr)


if __name__ == "__main__":
    unittest.main()
