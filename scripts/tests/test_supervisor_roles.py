"""Role-decouple coverage for scripts/aimee-llm-supervisor.sh.

Runs the REAL supervisor with PATH shims for the external commands it drives —
`wget` (records fetch URLs, creates the dest), the llama binary (records the
--port it was started on), `python3` (the gateway: dumps its env then exits so
the supervisor's `wait -n` returns), and `sha256sum` (no-op). Each mode combo
then asserts which models were downloaded, which llama-servers were launched,
and which per-role upstream the gateway was pointed at.

The invariant under test: a role that is not `local` is neither downloaded nor
launched, and when every role is external/off the plugin fetches nothing and
starts no llama-server (pure forwarder).
"""
import os
import pathlib
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
    # wget: find `-O <dest>` and the trailing URL; touch the dest, log the URL.
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
    # llama-server: record the --port; stay alive so the gateway's exit (below) is
    # what trips the supervisor's `wait -n`, after it has dumped its env.
    _write(bindir / "llama", textwrap.dedent(f"""\
        #!/usr/bin/env bash
        port=""
        while [ $# -gt 0 ]; do case "$1" in --port) port="$2"; shift 2;; *) shift;; esac; done
        echo "$port" >> "{logdir}/llama.log"
        # Detach from the captured stdout/stderr pipe before blocking, so the test's
        # subprocess sees EOF the moment the supervisor exits (not when sleep ends).
        exec sleep 30 >/dev/null 2>&1
        """))
    # gateway: dump the resolved env (per-role URLs) then exit so `wait -n` returns.
    _write(bindir / "python3", textwrap.dedent(f"""\
        #!/usr/bin/env bash
        env > "{logdir}/gateway_env.log"
        exit 0
        """))
    _write(bindir / "sha256sum", "#!/usr/bin/env bash\ncat >/dev/null 2>&1 || true\nexit 0\n")
    return bindir, logdir


class SupervisorRoleTest(unittest.TestCase):
    def run_supervisor(self, modes=None, tier="cpu", extra_env=None):
        tmp = pathlib.Path(tempfile.mkdtemp(prefix="sup-roles-"))
        bindir, logdir = _make_shims(tmp)
        env = dict(os.environ)
        env["PATH"] = f"{bindir}:{env['PATH']}"
        env["AIMEE_LLM_LLAMA_BIN"] = str(bindir / "llama")
        env["AIMEE_LLM_MODELS_DIR"] = str(tmp / "models")
        env["AIMEE_LLM_TIER"] = tier
        env["AIMEE_LLM_NGL"] = "0"
        env.pop("AIMEE_LLM_STUB", None)
        for k in ("AIMEE_LLM_EMBED_MODE", "AIMEE_LLM_RERANK_MODE", "AIMEE_LLM_SYNTH_MODE",
                  "AIMEE_LLM_SYNTH_LOCAL", "AIMEE_LLM_EMBED_URL", "AIMEE_LLM_RERANK_URL",
                  "AIMEE_LLM_SYNTH_URL"):
            env.pop(k, None)
        for k, v in (modes or {}).items():
            env[k] = v
        if extra_env:
            env.update(extra_env)
        proc = subprocess.run(["bash", str(SUP)], env=env, capture_output=True,
                              text=True, timeout=30)

        def read_lines(name):
            p = logdir / name
            return p.read_text().split() if p.exists() else []

        gw = {}
        ge = logdir / "gateway_env.log"
        if ge.exists():
            for line in ge.read_text().splitlines():
                if "=" in line:
                    key, val = line.split("=", 1)
                    gw[key] = val
        return proc, read_lines("wget.log"), read_lines("llama.log"), gw

    def test_all_local_downloads_and_launches_all(self):
        proc, urls, ports, gw = self.run_supervisor()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertTrue(any("Embedding" in u for u in urls), f"embed not fetched: {urls}")
        self.assertTrue(any("gemma" in u.lower() for u in urls), f"synth not fetched: {urls}")
        self.assertTrue(any("rerank-ettin" in u for u in urls), f"rerank not fetched: {urls}")
        self.assertEqual(set(ports), {"8081", "8082", "8083"}, ports)
        self.assertEqual(gw.get("AIMEE_LLM_EMBED_URL"), "http://127.0.0.1:8081")
        self.assertEqual(gw.get("AIMEE_LLM_RERANK_URL"), "http://127.0.0.1:8082")
        self.assertEqual(gw.get("AIMEE_LLM_SYNTH_URL"), "http://127.0.0.1:8083")
        self.assertTrue(gw.get("AIMEE_LLM_RERANK_HEAD", "").endswith("cpu/rerank-head"),
                        gw.get("AIMEE_LLM_RERANK_HEAD"))

    def test_external_embed_skips_embed_download_and_server(self):
        proc, urls, ports, gw = self.run_supervisor(
            modes={"AIMEE_LLM_EMBED_MODE": "external",
                   "AIMEE_LLM_EMBED_URL": "http://ext-embed:9000"})
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertFalse(any("Embedding" in u for u in urls), f"embed should not download: {urls}")
        self.assertTrue(any("gemma" in u.lower() for u in urls), urls)     # synth still local
        self.assertTrue(any("rerank-ettin" in u for u in urls), urls)      # rerank still local
        self.assertNotIn("8081", ports, ports)
        self.assertEqual(set(ports), {"8082", "8083"}, ports)
        self.assertEqual(gw.get("AIMEE_LLM_EMBED_URL"), "http://ext-embed:9000")

    def test_synth_off_gates_the_role(self):
        proc, urls, ports, gw = self.run_supervisor(
            modes={"AIMEE_LLM_SYNTH_MODE": "off"})
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertFalse(any("gemma" in u.lower() for u in urls), f"synth should not download: {urls}")
        self.assertNotIn("8083", ports, ports)
        self.assertEqual(gw.get("AIMEE_LLM_SYNTH_URL", "SENTINEL"), "")  # empty => gated

    def test_all_external_downloads_nothing_and_starts_no_server(self):
        proc, urls, ports, gw = self.run_supervisor(modes={
            "AIMEE_LLM_EMBED_MODE": "external", "AIMEE_LLM_EMBED_URL": "http://e:1",
            "AIMEE_LLM_RERANK_MODE": "external", "AIMEE_LLM_RERANK_URL": "http://r:2",
            "AIMEE_LLM_SYNTH_MODE": "external", "AIMEE_LLM_SYNTH_URL": "http://s:3",
        })
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(urls, [], f"nothing should download: {urls}")
        self.assertEqual(ports, [], f"no llama-server should start: {ports}")

    def test_external_without_url_fails_fast(self):
        proc, _urls, _ports, _gw = self.run_supervisor(
            modes={"AIMEE_LLM_EMBED_MODE": "external"})  # no AIMEE_LLM_EMBED_URL
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("mode=external requires", proc.stderr)

    def test_legacy_synth_local_zero_is_external(self):
        proc, urls, ports, gw = self.run_supervisor(
            modes={"AIMEE_LLM_SYNTH_LOCAL": "0", "AIMEE_LLM_SYNTH_URL": "http://legacy-synth:8"})
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertFalse(any("gemma" in u.lower() for u in urls), urls)
        self.assertNotIn("8083", ports, ports)
        self.assertEqual(gw.get("AIMEE_LLM_SYNTH_URL"), "http://legacy-synth:8")

    def test_invalid_mode_rejected(self):
        proc, _u, _p, _g = self.run_supervisor(modes={"AIMEE_LLM_EMBED_MODE": "bogus"})
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("invalid AIMEE_LLM_EMBED_MODE", proc.stderr)


if __name__ == "__main__":
    unittest.main()
