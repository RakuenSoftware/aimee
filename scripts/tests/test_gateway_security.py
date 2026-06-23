#!/usr/bin/env python3
"""Unit tests for the aimee-llm gateway's §1a security controls (auth, bind guard,
derived scope, audit, circuit breaker). Pure stdlib — no model, no network."""
import importlib.util
import io
import os
import unittest
from unittest import mock

GW = os.path.join(os.path.dirname(__file__), "..", "aimee_llm_gateway.py")


def _gw():
    spec = importlib.util.spec_from_file_location("aimee_llm_gateway", GW)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


class Auth(unittest.TestCase):
    def setUp(self):
        self.gw = _gw()

    def test_open_when_no_token(self):
        self.gw.AUTH_TOKEN = ""
        self.gw.check_auth(None)  # no raise

    def test_valid_bearer(self):
        self.gw.AUTH_TOKEN = "s3cr3t"
        self.gw.check_auth("Bearer s3cr3t")
        self.gw.check_auth("bearer s3cr3t")  # scheme is case-insensitive

    def test_missing_or_wrong_401(self):
        self.gw.AUTH_TOKEN = "s3cr3t"
        for h in (None, "", "s3cr3t", "Bearer nope", "Basic s3cr3t"):
            with self.assertRaises(self.gw.GatewayError) as e:
                self.gw.check_auth(h)
            self.assertEqual(e.exception.status, 401)


class Scope(unittest.TestCase):
    def setUp(self):
        self.gw = _gw()

    def test_rejects_caller_supplied(self):
        with self.assertRaises(self.gw.GatewayError) as e:
            self.gw.derive_scope({"X-Aimee-Scope": "curator"}.get)
        self.assertEqual(e.exception.status, 403)
        self.assertEqual(e.exception.body["error"]["code"], "scope_spoof")

    def test_derives_default(self):
        self.gw.SCOPE_DEFAULT = "curator"
        self.assertEqual(self.gw.derive_scope({}.get), "curator")


class Bind(unittest.TestCase):
    def setUp(self):
        self.gw = _gw()

    def test_loopback_ok(self):
        self.assertEqual(self.gw.validate_bind("127.0.0.1", ""), "ok")

    def test_wildcard_with_auth_ok(self):
        self.assertEqual(self.gw.validate_bind("0.0.0.0", "tok"), "ok")

    def test_wildcard_no_auth_warns(self):
        self.assertEqual(self.gw.validate_bind("0.0.0.0", ""), "warn")

    def test_strict_wildcard_no_auth_raises(self):
        for b in ("0.0.0.0", "::", ""):
            with self.assertRaises(RuntimeError):
                self.gw.validate_bind(b, "", strict=True)


class Audit(unittest.TestCase):
    def setUp(self):
        self.gw = _gw()

    def test_metadata_only_no_creds(self):
        buf = io.StringIO()
        with mock.patch.object(self.gw.sys, "stderr", buf):
            self.gw.audit("chat", "curator", "ok", bytes_in=10, bytes_out=20)
        line = buf.getvalue()
        self.assertIn("AIMEE-AUDIT", line)
        self.assertIn('"scope": "curator"', line)
        self.assertIn('"bytes_in": 10', line)
        # no token/content fields leak
        self.assertNotIn("Authorization", line)
        self.assertNotIn("Bearer", line)

    def test_sanitizer_drops_sensitive_meta(self):
        buf = io.StringIO()
        with mock.patch.object(self.gw.sys, "stderr", buf):
            self.gw.audit("chat", "curator", "ok", bytes_in=5,
                          authorization="Bearer leak", prompt="secret text",
                          messages=[{"x": 1}], api_key="k")
        line = buf.getvalue()
        self.assertIn('"bytes_in": 5', line)
        for leaked in ("leak", "secret text", "authorization", "prompt", "messages", "api_key"):
            self.assertNotIn(leaked, line)


class Breaker(unittest.TestCase):
    def setUp(self):
        self.gw = _gw()

    def _clock(self):
        return self.t

    def test_opens_on_consecutive_failures(self):
        self.t = 0.0
        cb = self.gw.CircuitBreaker(threshold=3, recovery=60, clock=self._clock)
        for _ in range(2):
            self.assertTrue(cb.allow())
            cb.record(False)
        self.assertEqual(cb.state, "closed")
        self.assertTrue(cb.allow())
        cb.record(False)  # 3rd consecutive
        self.assertEqual(cb.state, "open")
        self.assertFalse(cb.allow())  # blocked while open

    def test_half_open_probe_then_close(self):
        self.t = 0.0
        cb = self.gw.CircuitBreaker(threshold=2, recovery=30, clock=self._clock)
        cb.record(False)
        cb.record(False)
        self.assertEqual(cb.state, "open")
        self.assertFalse(cb.allow())
        self.t = 31.0  # recovery elapsed
        self.assertTrue(cb.allow())  # half-open probe allowed
        self.assertEqual(cb.state, "half-open")
        cb.record(True)
        self.assertEqual(cb.state, "closed")

    def test_half_open_failure_reopens(self):
        self.t = 0.0
        cb = self.gw.CircuitBreaker(threshold=1, recovery=10, clock=self._clock)
        cb.record(False)
        self.assertEqual(cb.state, "open")
        self.t = 11.0
        self.assertTrue(cb.allow())
        cb.record(False)  # probe fails
        self.assertEqual(cb.state, "open")

    def test_success_resets_consecutive(self):
        self.t = 0.0
        cb = self.gw.CircuitBreaker(threshold=3, clock=self._clock)
        for _ in range(4):  # dilute so the windowed error-rate stays <=50%
            cb.record(True)
        cb.record(False)
        cb.record(False)  # consec=2 (< threshold)
        cb.record(True)   # resets consec to 0
        cb.record(False)
        cb.record(False)  # consec=2 again — never reaches 3
        self.assertEqual(cb.state, "closed")

    def test_error_rate_trip(self):
        """>50% error rate over the window opens even without N consecutive."""
        self.t = 0.0
        cb = self.gw.CircuitBreaker(threshold=99, clock=self._clock)
        cb.record(False)
        cb.record(True)
        cb.record(False)
        cb.record(False)  # 3 of 4 = 75% > 50%
        self.assertEqual(cb.state, "open")


if __name__ == "__main__":
    unittest.main()
