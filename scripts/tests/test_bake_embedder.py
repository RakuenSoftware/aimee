"""scripts/bake-embedder.py: the retry, the selection, and the auto_map follow.

This runs where a failure is cheap. The code it covers runs inside an image build,
where the same failure costs the whole build and shows up as a Hugging Face traceback
in a CI log.
"""
import importlib.util
import json
import os
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]

spec = importlib.util.spec_from_file_location("bake", ROOT / "scripts" / "bake-embedder.py")
bake = importlib.util.module_from_spec(spec)


class FakeHub:
    """Stands in for huggingface_hub.snapshot_download, counting calls."""

    def __init__(self, script):
        self.script = list(script)
        self.calls = 0

    def __call__(self, repo, **kw):
        self.calls += 1
        outcome = self.script.pop(0) if self.script else None
        if isinstance(outcome, Exception):
            raise outcome
        return outcome or "/cache/" + repo.replace("/", "--")


def load():
    # snapshot_download is imported at module scope, so stub it before exec.
    sys.modules.setdefault("huggingface_hub", type(sys)("huggingface_hub"))
    sys.modules["huggingface_hub"].snapshot_download = lambda *a, **k: ""
    spec.loader.exec_module(bake)
    bake.time = type(sys)("time")
    bake.sleeps = []
    bake.time.sleep = bake.sleeps.append
    return bake


def test_transient_is_retried():
    m = load()
    hub = FakeHub([Exception("429 Too Many Requests for url .../bekko"), None])
    m.snapshot_download = hub
    got = m.fetch("hotchpotch/bekko-embedding-v1-a25m")
    assert got, got
    assert hub.calls == 2, hub.calls
    assert m.sleeps == [15], m.sleeps
    print("  transient 429 retried, then succeeds")


def test_permanent_is_not_retried():
    m = load()
    hub = FakeHub([Exception("RepositoryNotFoundError: 401 Client Error. Repository not found")])
    m.snapshot_download = hub
    try:
        m.fetch("nobody/nope")
    except Exception:
        pass
    else:
        sys.exit("a permanent error must propagate")
    # The point: no waiting. Sleeping to repeat a typo moves the failure away from its
    # cause and slows every build that has one.
    assert hub.calls == 1, hub.calls
    assert m.sleeps == [], m.sleeps
    print("  permanent error fails on the first attempt, no sleep")


def test_exhaustion_is_bounded():
    m = load()
    hub = FakeHub([Exception("503 Server Error")] * 6)
    m.snapshot_download = hub
    try:
        m.fetch("hotchpotch/bekko-embedding-v1-a25m")
    except Exception:
        pass
    else:
        sys.exit("an exhausted retry must propagate")
    assert hub.calls == 5, hub.calls
    assert m.sleeps == [15, 30, 60, 120], m.sleeps
    print("  exhausted retries give up after 5 attempts, backing off 15/30/60/120")


def test_unknown_embedder_is_a_build_failure():
    """An unrecognised name must stop the build, not produce a weightless image.

    A container with no weights starts fine and then cannot embed, which reaches the
    operator as a retrieval outage rather than a build error -- the failure mode this
    whole image split is meant to make impossible.
    """
    m = load()
    with tempfile.TemporaryDirectory() as d:
        reg = pathlib.Path(d) / "embedders.json"
        reg.write_text(json.dumps({"embedders": {"bekko-a25m": {"repo": "x", "dim": 384}}}))
        m.REGISTRY = str(reg)
        import os
        for bad in ("", "none", "bekko-a25", "nomic"):
            os.environ["AIMEE_EMBEDDER"] = bad
            try:
                m.main()
            except SystemExit as e:
                assert "not in the registry" in str(e), str(e)
            else:
                sys.exit(f"AIMEE_EMBEDDER={bad!r} should have failed the build")
    # 'none' specifically: that deployment runs NO embedder container, so it is not a
    # value this image can be built with.
    print("  unknown/empty/none embedder fails the build with the known list")


def test_auto_map_code_repos_are_followed():
    m = load()
    with tempfile.TemporaryDirectory() as d:
        cfg = pathlib.Path(d) / "config.json"
        cfg.write_text(json.dumps({"auto_map": {
            "AutoConfig": "nomic-ai/nomic-bert-2048--configuration_hf_nomic_bert.NomicBertConfig",
            "AutoModel": ["nomic-ai/other-repo--modeling.Thing", "not-a-ref"],
        }}))
        got = m.code_repos(d)
    assert got == {"nomic-ai/nomic-bert-2048", "nomic-ai/other-repo"}, got
    # No config.json at all is not an error: most models carry their own code.
    with tempfile.TemporaryDirectory() as d:
        assert m.code_repos(d) == set()
    print("  auto_map code repos are followed, missing config.json is not an error")


def test_nomic_external_code_is_immutably_pinned():
    registry = json.loads((ROOT / "scripts" / "embedders.json").read_text())
    nomic = registry["embedders"]["nomic-embed-text-v2-moe"]
    code_revision = (nomic.get("code_revisions") or {}).get("nomic-ai/nomic-bert-2048")
    assert code_revision == "e5042dce39060cc34bc223455f25cf1d26db4655", code_revision
    assert len(code_revision) == 40 and all(c in "0123456789abcdef" for c in code_revision)
    print("  nomic external auto_map code is bound to its compatible immutable commit")


def test_external_code_fetch_uses_registry_revision():
    m = load()
    revision = "1" * 40
    code_revision = "2" * 40
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        snapshot = root / "snapshot"
        snapshot.mkdir()
        (snapshot / "config.json").write_text(json.dumps({"auto_map": {
            "AutoModel": "example/code--modeling.Example",
        }}))
        registry = root / "embedders.json"
        registry.write_text(json.dumps({"embedders": {"example": {
            "repo": "example/model",
            "revision": revision,
            "code_revisions": {"example/code": code_revision},
        }}}))
        calls = []

        def fake_fetch(repo, **kwargs):
            calls.append((repo, kwargs))
            return str(snapshot)

        m.REGISTRY = str(registry)
        m.fetch = fake_fetch
        os.environ["AIMEE_EMBEDDER"] = "example"
        m.main()
        assert calls[1][0] == "example/code", calls
        assert calls[1][1]["revision"] == code_revision, calls

        registry.write_text(json.dumps({"embedders": {"example": {
            "repo": "example/model", "revision": revision,
        }}}))
        calls.clear()
        try:
            m.main()
        except RuntimeError as exc:
            assert "unpinned auto_map code repository example/code" in str(exc), exc
        else:
            raise AssertionError("external auto_map code without a revision was accepted")
    print("  external auto_map code fetches use immutable registry revisions")


if __name__ == "__main__":
    test_transient_is_retried()
    test_permanent_is_not_retried()
    test_exhaustion_is_bounded()
    test_unknown_embedder_is_a_build_failure()
    test_auto_map_code_repos_are_followed()
    test_nomic_external_code_is_immutably_pinned()
    test_external_code_fetch_uses_registry_revision()
    print("test_bake_embedder: ok")
