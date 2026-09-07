"""The embedder image must invoke the tested, bounded Hub download helper."""
from pathlib import Path
import runpy

ROOT = Path(__file__).resolve().parents[2]
dockerfile = (ROOT / "Dockerfile.embedder").read_text(encoding="utf-8")
assert "COPY scripts/bake-embedder.py /usr/local/bin/bake-embedder.py" in dockerfile
assert '/usr/local/bin/bake-embedder.py\n' in dockerfile
# Retry behavior is exercised against the actual helper, including wrapped
# transport errors, permanent failures, and bounded exponential backoff.
runpy.run_path(str(ROOT / "scripts/tests/test_bake_embedder.py"), run_name="__main__")
print("test_dockerfile_hub_retry: ok")
