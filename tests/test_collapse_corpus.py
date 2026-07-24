import json
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
VALID_CATEGORIES = {
    "top_level_or_nested_arrays_of_primitives",
    "uniform_object_array_with_discriminator",
    "objects_with_stable_keys_and_compatible_primitive_leaves",
    "json_fenced_inside_markdown",
}

PRIMITIVE_TYPES = (str, int, float, bool, type(None))


_COMMENT_LINE_RE = re.compile(r"<!--\s*(.*?)\s*-->")


def _strip_comment_wrapper(line):
    match = _COMMENT_LINE_RE.search(line)
    if match:
        return match.group(1)
    return line


_SEGMENT_RE = re.compile(r"([a-z_]+):\s*(.*?)\s*$")


def _parse_header_line(line):
    """Parse a header line by stripping an optional <!-- ... --> wrapper (and any
    leading `#` shell-comment marker) and extracting `key:value;` segments.

    The reserved separator is `;`. To prevent silent truncation of values (e.g. a
    shape description written as ``ramp; then loop`` would otherwise be parsed as
    ``shape:ramp`` followed by an orphan fragment `` then loop``), the parser
    verifies that every `;`-separated segment matches ``key: value``. A segment
    that does not match that shape indicates a stray `;` inside a value and is
    rejected as a malformed header. Authors must NOT include `;` inside any field
    value; this invariant is enforced here and documented in the grammar.
    """
    body = _strip_comment_wrapper(line).lstrip()
    if body.startswith("#"):
        body = body[1:].lstrip()
    segments = body.split(";")
    values = {}
    for segment in segments:
        segment = segment.strip()
        if not segment:
            # Trailing/empty segment is allowed (e.g. line ends with `;`).
            continue
        match = _SEGMENT_RE.match(segment)
        assert match, (
            f"malformed header segment {segment!r} (likely a stray ';' inside a "
            f"value, e.g. 'ramp; then loop'); full line: {line!r}"
        )
        key, value = match.group(1), match.group(2)
        assert key not in values, f"duplicate header field {key!r}: {line!r}"
        values[key] = value
    return values


def metadata(path):
    source = Path(f"{path}.meta") if path.suffix == ".json" else path
    first_line = source.read_text(encoding="utf-8").splitlines()[0]
    values = _parse_header_line(first_line)
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


def _is_primitive(value):
    return isinstance(value, PRIMITIVE_TYPES)


def _primitive_type_label(value):
    """Return a canonical primitive type label for a JSON leaf value.

    bool is checked before int because isinstance(True, int) is True in Python.
    Two JSON numbers that parse as int and float respectively are considered
    different primitive types, matching the grammar's 'compatible primitive leaf
    types' requirement.
    """
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "bool"
    if isinstance(value, int):
        return "int"
    if isinstance(value, float):
        return "float"
    if isinstance(value, str):
        return "str"
    return None


def _common_string_keys(records):
    if not records:
        return set()
    candidates = None
    for record in records:
        string_keys = {k for k, v in record.items() if isinstance(v, str)}
        candidates = string_keys if candidates is None else candidates & string_keys
    return candidates or set()


def _all_keys_common(records):
    if not records:
        return set()
    candidates = None
    for record in records:
        common_keys = set(record.keys())
        candidates = common_keys if candidates is None else candidates & common_keys
    return candidates or set()


def _discriminator_for_shape2(records):
    """Lexicographically first key common to every record whose value is a string in every
    record AND whose value is distinct across every record."""
    candidates = _common_string_keys(records)
    for key in sorted(candidates):
        values = [record[key] for record in records]
        if len(set(values)) == len(values):
            return key
    return None


def _walk_arrays(payload):
    """Yield every array node reachable from payload (including payload if it is a list)."""
    if isinstance(payload, list):
        yield payload
        for head in payload:
            yield from _walk_arrays(head)


def _leaves_are_primitives(payload):
    """True iff every leaf reachable through nested arrays is a primitive."""
    queue = [payload]
    while queue:
        head = queue.pop(0)
        if isinstance(head, list):
            queue.extend(head)
            continue
        if not _is_primitive(head):
            return False
    return True


def _validate_primitives_array(payload):
    """Shape 1: top-level or nested arrays of primitives."""
    if not isinstance(payload, list) or not payload:
        return False
    return _leaves_are_primitives(payload)


def _validate_discriminator_array(payload):
    """Shape 2: arrays (top-level or nested) of uniform-shape objects with required string
    discriminator and primitive leaves. The discriminator key value must be distinct across
    every record in the same array; the key must be common to all records."""
    found_any = False
    for array in _walk_arrays(payload):
        if not array or not all(isinstance(el, dict) for el in array):
            continue
        common_keys = _all_keys_common(array)
        if not common_keys:
            return False
        discriminator = _discriminator_for_shape2(array)
        if not discriminator:
            return False
        for record in array:
            if set(record.keys()) != common_keys:
                return False
            for key, value in record.items():
                if key == discriminator:
                    if not isinstance(value, str):
                        return False
                else:
                    if not _is_primitive(value):
                        return False
        found_any = True
    return found_any


def _validate_stable_keys_array(payload):
    """Shape 3: arrays (top-level or nested) of objects with stable keys,
    primitive leaves, and compatible per-key primitive leaf types across records.

    Compatible primitive leaf types means the JSON type label for each key is the
    same in every record. A key that is int in one record and str in another
    (e.g. [{"x": 1}, {"x": "a"}]) excludes the array from shape 3.
    """
    found_any = False
    for array in _walk_arrays(payload):
        if not array or not all(isinstance(el, dict) for el in array):
            continue
        common_keys = _all_keys_common(array)
        if not common_keys:
            return False
        key_types = {key: None for key in common_keys}
        for record in array:
            if set(record.keys()) != common_keys:
                return False
            for key, value in record.items():
                if not _is_primitive(value):
                    return False
                label = _primitive_type_label(value)
                if key_types[key] is None:
                    key_types[key] = label
                elif key_types[key] != label:
                    return False
        found_any = True
    return found_any


_FENCE_RE = re.compile(r"```json\s*\n(.*?)\n```", re.DOTALL)


def _extract_fenced_json(md_text):
    return [match.strip() for match in _FENCE_RE.findall(md_text)]


INNER_JSON_SHAPE_VALIDATORS = (
    _validate_primitives_array,
    _validate_discriminator_array,
    _validate_stable_keys_array,
)


def _validate_json_fenced(payload):
    """Shape 4: the fenced payload is a markdown wrapper; each inner array value must
    independently satisfy one of shapes 1, 2, or 3."""
    if not isinstance(payload, list) or not payload:
        return False
    for inner in _walk_arrays(payload):
        if not inner or not isinstance(inner, list):
            continue
        if any(_validator(inner) for _validator in INNER_JSON_SHAPE_VALIDATORS):
            return True
    return False


_VALIDATORS = {
    "top_level_or_nested_arrays_of_primitives": _validate_primitives_array,
    "uniform_object_array_with_discriminator": _validate_discriminator_array,
    "objects_with_stable_keys_and_compatible_primitive_leaves": _validate_stable_keys_array,
    "json_fenced_inside_markdown": _validate_json_fenced,
}


def _json_payloads_for(path):
    """Parse and return all JSON payloads (fenced markdown returns one per fence)."""
    text = path.read_text(encoding="utf-8")
    if path.suffix == ".json":
        return [json.loads(text)]
    if path.suffix == ".md":
        return [json.loads(body) for body in _extract_fenced_json(text)]
    return []


def _category_from_meta(path):
    """Read the declared `category:` field from a fixture header."""
    source = Path(f"{path}.meta") if path.suffix == ".json" else path
    first_line = source.read_text(encoding="utf-8").splitlines()[0]
    match = re.search(r"category:\s*([a-z_]+)", first_line)
    assert match, f"fixture under tests/fixtures/collapse_legit/json/ must declare category: {path}"
    category = match.group(1)
    assert category in VALID_CATEGORIES, (
        f"unknown category {category!r}: {path}; valid: {sorted(VALID_CATEGORIES)}"
    )
    return category


def test_required_json_shape_categories_present():
    json_dir = ROOT / "collapse_legit" / "json"
    declared = {}
    for path in sorted(json_dir.rglob("*")):
        if not path.is_file() or path.suffix == ".meta":
            continue
        category = _category_from_meta(path)
        payloads = _json_payloads_for(path)
        assert payloads, f"no JSON payload found for fixture: {path}"
        validator = _VALIDATORS[category]
        for payload in payloads:
            assert validator(payload), (
                f"fixture payload does not satisfy structural invariant of declared "
                f"category {category!r}: {path}; payload={payload!r}"
            )
        declared.setdefault(category, []).append(path)
    missing = VALID_CATEGORIES - set(declared)
    assert not missing, (
        f"required JSON shape categories missing from corpus: {sorted(missing)}; "
        f"declared: {sorted(declared)}"
    )


MIN_MEANINGFUL_FIRE_PERIOD = 3
N_REPETITIONS = 4


def _period_is_trivial(period):
    return all(c in (ord(" "), ord("\n"), ord("\t")) for c in period)


def _mechanical_fire_periods(payload, n=N_REPETITIONS, min_span=MIN_MEANINGFUL_FIRE_PERIOD):
    """Yield every contiguous verbatim period (span, start_offset) of length >= min_span
    that is repeated >= n consecutive times in `payload` and is not pure whitespace.

    The default thresholds mirror the grammar's N=4 repetition threshold and use a
    minimum period of 3 bytes so trivial single- and two-byte coincidences (a run
    of newlines, runs of indent spaces) do not produce false positives.
    """
    L = len(payload)
    max_span = L // n
    findings = []
    for span in range(min_span, max_span + 1):
        end = L - span * n + 1
        for start in range(end):
            period = payload[start:start + span]
            if _period_is_trivial(period):
                continue
            if all(payload[start + i * span:start + (i + 1) * span] == period for i in range(n)):
                findings.append((span, start, period))
    return findings


def test_no_fire_bodies_have_no_mechanical_fire_period():
    """Negative invariant: every no-fire fixture's body must not contain a contiguous
    verbatim period of length >= 3 bytes repeated >= N=4 times. This is the structural
    condition that a future detector could fire on mechanically. A fixture whose header
    declares no-fire but whose bytes meet the fire condition is malformed: the no-fire
    label is a consequence of the structure, not a label-only assertion.

    Whitespace-only periods (e.g. a run of newlines or indent spaces) are excluded
    because they are not semantically meaningful repetitions.
    """
    for path in (ROOT / "collapse_legit").rglob("*"):
        if not path.is_file() or path.suffix == ".meta":
            continue
        source = Path(f"{path}.meta") if path.suffix == ".json" else path
        first_line = source.read_text(encoding="utf-8").splitlines()[0]
        values = _parse_header_line(first_line)
        if values.get("expected", "").strip() != "no-fire":
            continue
        payload = path.read_bytes()
        periods = _mechanical_fire_periods(payload)
        assert not periods, (
            f"no-fire fixture body contains a mechanical fire period "
            f"(span >= 3 bytes, repeated >= 4 times); the no-fire label is a "
            f"consequence of structure, not a label-only assertion. Offending "
            f"periods: {[(s, off, bytes(p)) for s, off, p in periods[:5]]}; "
            f"fixture: {path}"
        )


def test_shape3_rejects_per_key_type_changes():
    """Shape 3's 'compatible primitive leaf types' requirement means a single key
    must keep the same primitive type across every record. Mixed int/str, int/float,
    str/bool, and str/null all break shape-3 membership.
    """
    bad_cases = [
        [{"x": 1}, {"x": "a"}],            # int -> str
        [{"x": 1}, {"x": 1.5}],            # int -> float
        [{"x": "a"}, {"x": True}],         # str -> bool
        [{"x": "a"}, {"x": None}],         # str -> null
        [{"x": 1, "y": 2}, {"x": 1, "y": "b"}],  # mixed only on y
    ]
    for payload in bad_cases:
        assert not _validate_stable_keys_array(payload), (
            f"expected shape-3 rejection for per-key type change: {payload!r}"
        )

    good_cases = [
        [{"x": 1, "y": 2}, {"x": 3, "y": 4}],
        [{"x": 1, "y": "a"}, {"x": 2, "y": "b"}],
        [{"x": None}, {"x": None}],
        [{"x": True}, {"x": False}],
        [{"x": 1.5}, {"x": 2.5}],
    ]
    for payload in good_cases:
        assert _validate_stable_keys_array(payload), (
            f"expected shape-3 acceptance for uniform per-key types: {payload!r}"
        )
