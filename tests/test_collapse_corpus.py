import re
from pathlib import Path


ROOT = Path(__file__).parent / "fixtures"
FIELDS = (
    "shape",
    "expected",
    "expected_loop_start_offset",
    "expected_loop_span_bytes",
    "expected_repetitions",
)


def metadata(path):
    source = Path(f"{path}.meta") if path.suffix == ".json" else path
    first_line = source.read_text(encoding="utf-8").splitlines()[0]
    values = dict(re.findall(
        r"(shape|expected|expected_loop_start_offset|expected_loop_span_bytes|expected_repetitions):\s*([^;]+?)(?=;|\s*-->|$)",
        first_line,
    ))
    assert set(values) == set(FIELDS), f"incomplete fixture header: {source}"
    return values


def test_every_fixture_has_complete_metadata():
    for tree in (ROOT / "collapse_legit", ROOT / "collapse_collapse"):
        for path in tree.rglob("*"):
            if path.is_file() and path.suffix != ".meta":
                metadata(path)


def test_fire_oracles_are_absolute_byte_exact_verbatim_periods():
    for path in (ROOT / "collapse_collapse").glob("*"):
        values = metadata(path)
        assert values["expected"].strip() == "fire"
        offset = int(values["expected_loop_start_offset"])
        span = int(values["expected_loop_span_bytes"])
        repetitions = int(values["expected_repetitions"])
        payload = path.read_bytes()
        period = payload[offset:offset + span]
        assert repetitions >= 4
        assert len(period) == span
        assert payload[offset:offset + span * repetitions] == period * repetitions


def test_no_fire_oracles_use_sentinel_values():
    for path in (ROOT / "collapse_legit").rglob("*"):
        if not path.is_file() or path.suffix == ".meta":
            continue
        values = metadata(path)
        assert values["expected"].strip() == "no-fire"
        assert int(values["expected_loop_start_offset"]) == -1
        assert int(values["expected_loop_span_bytes"]) == -1
        assert int(values["expected_repetitions"]) == 0
