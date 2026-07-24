import re
from pathlib import Path


ROOT = Path(__file__).parent / "fixtures"
REQUIRED_FIELDS = (
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
        r"([a-z_]+):\s*([^;]+?)(?=;|\s*-->|$)",
        first_line,
    ))
    missing = set(REQUIRED_FIELDS) - set(values)
    assert not missing, f"incomplete fixture header (missing {missing}): {source}"
    return values


def test_every_fixture_has_complete_metadata():
    for tree in (ROOT / "collapse_legit", ROOT / "collapse_collapse"):
        for path in tree.rglob("*"):
            if path.is_file() and path.suffix != ".meta":
                metadata(path)


def test_fire_oracles_are_absolute_byte_exact_verbatim_periods():
    for path in (ROOT / "collapse_collapse").glob("*"):
        if not path.is_file():
            continue
        values = metadata(path)
        if values.get("connective_tissue", "").strip().lower() == "yes":
            continue
        assert values["expected"].strip() == "fire"
        offset = int(values["expected_loop_start_offset"])
        span = int(values["expected_loop_span_bytes"])
        repetitions = int(values["expected_repetitions"])
        payload = path.read_bytes()
        period = payload[offset:offset + span]
        assert repetitions >= 4
        assert len(period) == span
        assert payload[offset:offset + span * repetitions] == period * repetitions


def test_interleaved_oracles_have_non_repeating_connective_tissue():
    for path in (ROOT / "collapse_collapse").glob("*"):
        if not path.is_file():
            continue
        values = metadata(path)
        if values.get("connective_tissue", "").strip().lower() != "yes":
            continue
        assert values["expected"].strip() == "fire"
        assert "expected_loop_iteration_offsets" in values, (
            f"interleaved fixture must declare expected_loop_iteration_offsets: {path}"
        )
        offset = int(values["expected_loop_start_offset"])
        span = int(values["expected_loop_span_bytes"])
        repetitions = int(values["expected_repetitions"])
        iteration_offsets = [
            int(x.strip())
            for x in values["expected_loop_iteration_offsets"].split(",")
        ]
        assert len(iteration_offsets) == repetitions, (
            f"expected_loop_iteration_offsets count must equal expected_repetitions: {path}"
        )
        assert iteration_offsets[0] == offset, (
            f"first iteration offset must equal expected_loop_start_offset: {path}"
        )
        payload = path.read_bytes()
        period = payload[offset:offset + span]
        assert len(period) == span
        for i, off in enumerate(iteration_offsets):
            assert payload[off:off + span] == period, (
                f"iteration {i} at offset {off} does not match period: {path}"
            )
        connector_segments = []
        for i in range(len(iteration_offsets) - 1):
            start = iteration_offsets[i] + span
            end = iteration_offsets[i + 1]
            connector_segments.append(payload[start:end])
        assert len(set(connector_segments)) == len(connector_segments), (
            f"connective tissue between iterations must be non-repeating (all distinct): {path}"
        )


def test_no_fire_oracles_use_sentinel_values():
    for path in (ROOT / "collapse_legit").rglob("*"):
        if not path.is_file() or path.suffix == ".meta":
            continue
        values = metadata(path)
        assert values["expected"].strip() == "no-fire"
        assert int(values["expected_loop_start_offset"]) == -1
        assert int(values["expected_loop_span_bytes"]) == -1
        assert int(values["expected_repetitions"]) == 0


REQUIRED_JSON_SHAPES = {
    "top_level_or_nested_arrays_of_primitives",
    "uniform_object_array_with_discriminator",
    "objects_with_stable_keys_and_compatible_primitive_leaves",
    "json_fenced_inside_markdown",
}


def _classify_json_shape(meta_text):
    text = meta_text.lower()
    if "fenced" in text and ("json" in text or "markdown" in text):
        return "json_fenced_inside_markdown"
    if "discriminator" in text:
        return "uniform_object_array_with_discriminator"
    if "stable keys" in text or "stable-key" in text or "stable key" in text:
        return "objects_with_stable_keys_and_compatible_primitive_leaves"
    if "primitive" in text:
        return "top_level_or_nested_arrays_of_primitives"
    return None


def test_required_json_shape_categories_present():
    json_dir = ROOT / "collapse_legit" / "json"
    present = set()
    for path in sorted(json_dir.rglob("*")):
        if not path.is_file():
            continue
        if path.suffix == ".meta":
            present.add(_classify_json_shape(path.read_text(encoding="utf-8")))
        elif path.suffix in {".json", ".md"}:
            header = metadata(path)["shape"]
            present.add(_classify_json_shape(header))
    missing = REQUIRED_JSON_SHAPES - present
    assert not missing, (
        f"required JSON shape categories missing from corpus: {sorted(missing)}; "
        f"present: {sorted(present)}"
    )
