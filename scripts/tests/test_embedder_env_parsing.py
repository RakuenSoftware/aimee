"""An EMPTY environment variable must not kill the embedder at import.

Compose passes an unset variable through as an empty string, not as absence:

    EMBEDDER_THREADS: "${EMBEDDER_THREADS:-}"

so os.environ.get("EMBEDDER_THREADS", "0") returns "" and int("") raises
ValueError at module scope. The embedder then dies before binding :8760, the KB
never binds :8741, its health check fails forever, and the deployment reports a
knowledge base that will not come up.

Observed on a fresh production install: the wizard deployed aimee-kb-a25m
correctly and it sat unhealthy through every restart. The traceback named the
line, but the variable LOOKED unset -- it was present and empty, which is the
one case the default argument does not cover.

This is the default path. Any deployment that does not explicitly set the
variable hits it, so it has to be tested at the value level rather than trusted
to a default argument.
"""
import importlib.util
import os
import pathlib
import sys
import unittest

MODULE = pathlib.Path(__file__).resolve().parents[2] / "scripts" / "embedder-server.py"


def load_env_int():
    """Import _env_int alone, without the module's torch/model side effects."""
    src = MODULE.read_text(encoding="utf-8")
    start = src.index("def _env_int(")
    end = src.index("PORT = ")
    ns = {"os": os, "sys": sys}
    exec(compile(src[start:end], str(MODULE), "exec"), ns)
    return ns["_env_int"]


class TestEnvInt(unittest.TestCase):
    def setUp(self):
        self._env_int = load_env_int()
        self._saved = {k: os.environ.get(k) for k in ("T_PROBE",)}

    def tearDown(self):
        for k, v in self._saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v

    def test_empty_string_uses_the_default(self):
        """The bug: present-but-empty is what compose actually passes."""
        os.environ["T_PROBE"] = ""
        self.assertEqual(self._env_int("T_PROBE", 15), 15)

    def test_whitespace_only_uses_the_default(self):
        os.environ["T_PROBE"] = "   "
        self.assertEqual(self._env_int("T_PROBE", 15), 15)

    def test_absent_uses_the_default(self):
        os.environ.pop("T_PROBE", None)
        self.assertEqual(self._env_int("T_PROBE", 15), 15)

    def test_a_real_value_still_wins(self):
        os.environ["T_PROBE"] = "7"
        self.assertEqual(self._env_int("T_PROBE", 15), 7)
        os.environ["T_PROBE"] = " 7 "
        self.assertEqual(self._env_int("T_PROBE", 15), 7)

    def test_junk_falls_back_rather_than_raising(self):
        """An embedder that refuses to start is worse than one that ignores a
        bad value and logs the default it used."""
        os.environ["T_PROBE"] = "banana"
        self.assertEqual(self._env_int("T_PROBE", 15), 15)

    def test_callable_default_is_only_called_when_needed(self):
        """EMBEDDER_THREADS defaults to min(8, usable_cpus()), which must not be
        computed when the operator supplied a value."""
        calls = []

        def expensive():
            calls.append(1)
            return 99

        os.environ["T_PROBE"] = "3"
        self.assertEqual(self._env_int("T_PROBE", expensive), 3)
        self.assertEqual(calls, [], "the default was computed despite an explicit value")

        os.environ["T_PROBE"] = ""
        self.assertEqual(self._env_int("T_PROBE", expensive), 99)
        self.assertEqual(len(calls), 1)


if __name__ == "__main__":
    unittest.main()
