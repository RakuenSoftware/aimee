"""Role- and tier-decouple coverage for scripts/aimee-llm-supervisor.sh.

Runs the REAL supervisor with PATH shims for the external commands it drives —
`wget` (records fetch URLs, creates the dest), the llama binary (records its full
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
import subprocess
import tempfile
import textwrap
import unittest

SUP = pathlib.Path(__file__).resolve().parents[1] / "aimee-llm-supervisor.sh"


def _write(path, body):
    path.write_text(body)
    path.chmod(0o755)


def _make_shims(tmp):
    bindir = tmp / "bin"
    logdir = tmp / "log"
    bindir.mkdir()
    logdir.mkdir()
    _write(bindir / "wget", textwrap.dedent(f"""\
        #!/usr/bin/env bash
        dest=""; url=""
        while [ $# -gt 0 ]; do
          case "$1" in
            -O) dest="$2"; shift 2;;
            -*) shift;;
            *) url="$1"; shift;;
          esac
        done
        [ -n "$dest" ] && : > "$dest"
        echo "$url" >> "{logdir}/wget.log"
        exit 0
        """))
    # llama-server: record the full argv (one line), then detach from the captured
    # pipe and block, so the supervisor's exit — not `sleep` — ends the test call.
    _write(bindir / "llama", textwrap.dedent(f"""\
        #!/usr/bin/env bash
        echo "$*" >> "{logdir}/llama.log"
        exec sleep 30 >/dev/null 2>&1
        """))
    _write(bindir / "python3", textwrap.dedent(f"""\
        #!/usr/bin/env bash
        env > "{logdir}/gateway_env.log"
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
        env.pop("AIMEE_LLM_STUB", None)
        for k in ("AIMEE_LLM_TIER", "AIMEE_LLM_EMBED_MODE", "AIMEE_LLM_RERANK_MODE",
                  "AIMEE_LLM_SYNTH_MODE", "AIMEE_LLM_SYNTH_LOCAL", "AIMEE_LLM_EMBED_URL",
                  "AIMEE_LLM_RERANK_URL", "AIMEE_LLM_SYNTH_URL", "AIMEE_LLM_EMBED_TIER",
                  "AIMEE_LLM_RERANK_TIER", "AIMEE_LLM_SYNTH_TIER"):
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
        return proc, lines("wget.log"), lines("llama.log"), gw

    def _launched(self, llama):
        return {_port(a): a for a in llama}

    # ---- role mode (local | external | off) --------------------------------
    def test_all_local_downloads_and_launches_all(self):
        proc, urls, llama, gw = self.run_supervisor()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertTrue(any("Embedding-0.6B" in u for u in urls), urls)   # cpu embed
        self.assertTrue(any("gemma-4-E4B" in u for u in urls), urls)      # cpu synth
        self.assertTrue(any("rerank-ettin-68m" in u for u in urls), urls) # cpu rerank
        self.assertEqual(set(self._launched(llama)), {"8081", "8082", "8083"})
        self.assertEqual(gw.get("AIMEE_LLM_EMBED_URL"), "http://127.0.0.1:8081")
        self.assertEqual(gw.get("AIMEE_LLM_SYNTH_URL"), "http://127.0.0.1:8083")
        self.assertTrue(gw.get("AIMEE_LLM_RERANK_HEAD", "").endswith("cpu/rerank-head"))

    def test_external_embed_skips_embed_download_and_server(self):
        proc, urls, llama, gw = self.run_supervisor(
            modes={"AIMEE_LLM_EMBED_MODE": "external",
                   "AIMEE_LLM_EMBED_URL": "http://ext-embed:9000"})
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertFalse(any("Embedding" in u for u in urls), urls)
        self.assertTrue(any("gemma" in u.lower() for u in urls), urls)
        self.assertEqual(set(self._launched(llama)), {"8082", "8083"})
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
            "AIMEE_LLM_RERANK_MODE": "external", "AIMEE_LLM_RERANK_URL": "http://r:2",
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
        self.assertTrue(any("Embedding-4B" in u for u in urls), urls)     # gpu embed
        self.assertTrue(any("gemma-4-26B" in u for u in urls), urls)      # mid synth
        self.assertTrue(any("rerank-ettin-400m" in u for u in urls), urls)
        launched = self._launched(llama)
        self.assertEqual(_ngl(launched["8081"]), "99")                    # gpu embed offloads
        self.assertIn("--parallel 2", launched["8083"])                   # mid => SLOTS=2

    def test_mixed_per_role_tiers_in_one_instance(self):
        # cpu embedder beside a gpu-large synth on the same instance.
        proc, urls, llama, gw = self.run_supervisor(modes={
            "AIMEE_LLM_EMBED_TIER": "cpu",
            "AIMEE_LLM_RERANK_TIER": "cpu",
            "AIMEE_LLM_SYNTH_TIER": "large",
            "AIMEE_LLM_NGL": "99",
        })
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertTrue(any("Embedding-0.6B" in u for u in urls), urls)   # cpu embed model
        self.assertTrue(any("gemma-4-26B" in u for u in urls), urls)      # large synth model
        self.assertTrue(any("rerank-ettin-68m" in u for u in urls), urls)
        launched = self._launched(llama)
        self.assertEqual(_ngl(launched["8081"]), "0")                     # cpu embed: no offload
        self.assertEqual(_ngl(launched["8083"]), "99")                    # gpu synth: offload
        self.assertIn("--parallel 4", launched["8083"])                   # large => SLOTS=4
        self.assertTrue(gw.get("AIMEE_LLM_RERANK_HEAD", "").endswith("cpu/rerank-head"))

    def test_invalid_tier_rejected(self):
        proc, *_ = self.run_supervisor(modes={"AIMEE_LLM_SYNTH_TIER": "bogus"})
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("invalid AIMEE_LLM_SYNTH_TIER", proc.stderr)


if __name__ == "__main__":
    unittest.main()
