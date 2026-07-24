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


_SEGMENT_RE = re.compile(r"([a-z_]+):\s*(.*?)\s*$")


def _parse_header_line(line):
    """Parse a single-line `.meta` header into `key:value` pairs.

    The reserved separator is `;`. To prevent silent truncation of values (e.g. a
    shape description written as ``ramp; then loop`` would otherwise be parsed as
    ``shape:ramp`` followed by an orphan fragment `` then loop``), the parser
    verifies that every `;`-separated segment matches ``key: value``. A segment
    that does not match that shape indicates a stray `;` inside a value and is
    rejected as a malformed header. Authors must NOT include `;` inside any field
    value; this invariant is enforced here and documented in the grammar.
    """
    segments = line.strip().split(";")
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
    """Return the parsed `.meta` sidecar for a fixture payload file.

    The `.meta` file is never part of the generated payload; all offsets are
    relative to byte 0 of the payload body (`path.read_bytes()`).
    """
    meta = Path(f"{path}.meta")
    assert meta.exists(), f"missing sidecar metadata: {meta}"
    first_line = meta.read_text(encoding="utf-8").splitlines()[0]
    values = _parse_header_line(first_line)
    missing = set(REQUIRED_FIELDS) - set(values)
    assert not missing, f"incomplete fixture header (missing {missing}): {meta}"
    return values


def _payload_bytes(path):
    """Return the generated payload bytes for a fixture (the body without `.meta`)."""
    return path.read_bytes()


def test_every_fixture_has_complete_metadata():
    for tree in (ROOT / "collapse_legit", ROOT / "collapse_collapse"):
        for path in tree.rglob("*"):
            if path.is_file() and path.suffix != ".meta":
                metadata(path)


def test_every_fixture_has_a_meta_sidecar():
    for tree in (ROOT / "collapse_legit", ROOT / "collapse_collapse"):
        for path in tree.rglob("*"):
            if path.is_file() and path.suffix != ".meta":
                assert Path(f"{path}.meta").exists(), (
                    f"fixture {path} must have a sibling .meta sidecar"
                )


def test_fire_oracles_are_absolute_byte_exact_verbatim_periods():
    for path in (ROOT / "collapse_collapse").rglob("*"):
        if not path.is_file() or path.suffix == ".meta":
            continue
        values = metadata(path)
        if values.get("connective_tissue", "").strip().lower() == "yes":
            continue
        assert values["expected"].strip() == "fire"
        offset = int(values["expected_loop_start_offset"])
        span = int(values["expected_loop_span_bytes"])
        repetitions = int(values["expected_repetitions"])
        payload = _payload_bytes(path)
        period = payload[offset:offset + span]
        assert repetitions >= 4
        assert len(period) == span
        assert payload[offset:offset + span * repetitions] == period * repetitions


def test_interleaved_oracles_have_non_repeating_connective_tissue():
    for path in (ROOT / "collapse_collapse").rglob("*"):
        if not path.is_file() or path.suffix == ".meta":
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
        payload = _payload_bytes(path)
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


def _is_primitive(value):
    """True iff value is one of the JSON primitive leaves: string, number, boolean, or null.

    The check uses the canonical type label so that bool is not treated as int
    (Python's bool is a subclass of int).
    """
    return _primitive_type_label(value) is not None


def _shape1_signature(value):
    """Return a structural signature for a shape-1 value, or None if it is not a
    rectangular array of primitives.

    * primitive -> ("primitive",)
    * list of uniform children -> ("list", length, child_signature)
    * mixed/empty/non-primitive -> None

    Empty arrays are not allowed in the grammar, so they return None.
    """
    if _is_primitive(value):
        return ("primitive",)
    if not isinstance(value, list) or not value:
        return None
    child_sigs = [_shape1_signature(child) for child in value]
    if any(sig is None for sig in child_sigs):
        return None
    first = child_sigs[0]
    if not all(sig == first for sig in child_sigs):
        return None
    return ("list", len(value), first)


def _validate_primitives_array(payload):
    """Shape 1: top-level or nested rectangular arrays of primitives.

    The payload must be a non-empty list whose structural signature is a uniform
    list of primitives, possibly nested to a uniform depth. Jagged arrays such as
    `[1, [2]]` or `[[1], [[2]]]` are rejected because their leaves are not at a
    single depth.
    """
    if not isinstance(payload, list) or not payload:
        return False
    sig = _shape1_signature(payload)
    if sig is None or sig[0] != "list":
        return False
    # Walk to the leaf signature to confirm it is a primitive.
    while sig[0] == "list":
        sig = sig[2]
    return sig == ("primitive",)


def _object_array_signature(records):
    """Return a structural signature for a list of objects, or None if they are not
    uniform-shape objects with primitive leaves.

    The signature is (frozenset(keys), dict(key -> primitive_type_label)).
    """
    if not records or not all(isinstance(r, dict) for r in records):
        return None
    keys = frozenset(records[0].keys())
    if not all(frozenset(r.keys()) == keys for r in records):
        return None
    key_types = {}
    for key in keys:
        labels = [_primitive_type_label(r[key]) for r in records]
        if None in labels or len(set(labels)) != 1:
            return None
        key_types[key] = labels[0]
    return (keys, key_types)


def _discriminator_for_records(records):
    """Lexicographically first key common to every record whose value is a string in every
    record AND whose value is distinct across every record."""
    sig = _object_array_signature(records)
    if sig is None:
        return None
    keys, key_types = sig
    string_keys = [k for k in sorted(keys) if key_types[k] == "str"]
    for key in string_keys:
        values = [record[key] for record in records]
        if len(set(values)) == len(values):
            return key
    return None


def _shape2_records(records):
    """True iff `records` is a non-empty list of uniform objects with a required string
    discriminator and primitive leaves.
    """
    if not records or not all(isinstance(r, dict) for r in records):
        return False
    sig = _object_array_signature(records)
    if sig is None:
        return False
    return _discriminator_for_records(records) is not None


def _shape3_records(records):
    """True iff `records` is a non-empty list of objects with stable keys and compatible
    primitive leaf types across every record.
    """
    if not records or not all(isinstance(r, dict) for r in records):
        return False
    return _object_array_signature(records) is not None


def _validate_uniform_object_array(payload, records_validator):
    """Validate that `payload` is a (possibly nested) uniform array of objects.

    The outer container must be homogeneous: either every top-level element is an
    object, or every top-level element is a non-empty array of objects. A mixed
    container such as `[{"a": 1}, [{"b": 2}]]` is rejected, as is a heterogeneous
    outer container that merely contains one valid nested object array.
    """
    if not isinstance(payload, list) or not payload:
        return False
    if all(isinstance(el, dict) for el in payload):
        return records_validator(payload)
    if all(isinstance(el, list) and el for el in payload):
        signatures = [_object_array_signature(el) for el in payload]
        if any(sig is None for sig in signatures):
            return False
        first = signatures[0]
        if not all(sig == first for sig in signatures):
            return False
        return all(records_validator(el) for el in payload)
    return False


def _validate_discriminator_array(payload):
    """Shape 2: arrays of uniform-shape objects with required string discriminator.

    The complete payload must belong to the shape; a heterogeneous outer container
    that contains one valid nested object array is rejected.
    """
    return _validate_uniform_object_array(payload, _shape2_records)


def _validate_stable_keys_array(payload):
    """Shape 3: arrays of objects with stable keys and compatible primitive leaf types.

    The complete payload must belong to the shape; a heterogeneous outer container
    that contains one valid nested object array is rejected.
    """
    return _validate_uniform_object_array(payload, _shape3_records)


# Canonical fenced JSON: exact info string `json`, complete wrapper, no extra text.
_FENCE_RE = re.compile(r"^\s*```json\n(.*)\n```\s*$", re.DOTALL)


def _extract_fenced_json(md_text):
    """Return the single fenced JSON body if `md_text` is exactly one canonical
    ` ```json ... ``` ` block, else return an empty list.

    The grammar requires the info string to be exactly `json`, the opening fence
    to be on its own line, the closing fence to be on its own line, and no extra
    text before or after the fence (leading/trailing whitespace only is allowed).
    """
    match = _FENCE_RE.match(md_text)
    if match:
        return [match.group(1)]
    return []


INNER_JSON_SHAPE_VALIDATORS = (
    _validate_primitives_array,
    _validate_discriminator_array,
    _validate_stable_keys_array,
)


def _validate_json_fenced(payload):
    """Shape 4: the fenced payload is a markdown wrapper; the parsed inner content must
    independently satisfy one of shapes 1, 2, or 3.
    """
    return any(_validator(payload) for _validator in INNER_JSON_SHAPE_VALIDATORS)


_VALIDATORS = {
    "top_level_or_nested_arrays_of_primitives": _validate_primitives_array,
    "uniform_object_array_with_discriminator": _validate_discriminator_array,
    "objects_with_stable_keys_and_compatible_primitive_leaves": _validate_stable_keys_array,
    "json_fenced_inside_markdown": _validate_json_fenced,
}


def _json_payloads_for(path):
    """Parse and return all JSON payloads (fenced markdown returns one per canonical fence)."""
    text = path.read_text(encoding="utf-8")
    if path.suffix == ".json":
        return [json.loads(text)]
    if path.suffix == ".md":
        bodies = _extract_fenced_json(text)
        assert bodies, f"no canonical ```json fence found in {path}"
        return [json.loads(body) for body in bodies]
    return []


def _category_from_meta(path):
    """Read the declared `category:` field from a fixture `.meta` sidecar."""
    meta = Path(f"{path}.meta")
    first_line = meta.read_text(encoding="utf-8").splitlines()[0]
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
        values = metadata(path)
        if values.get("expected", "").strip() != "no-fire":
            continue
        payload = _payload_bytes(path)
        periods = _mechanical_fire_periods(payload)
        assert not periods, (
            f"no-fire fixture body contains a mechanical fire period "
            f"(span >= 3 bytes, repeated >= 4 times); the no-fire label is a "
            f"consequence of structure, not a label-only assertion. Offending "
            f"periods: {[(s, off, bytes(p)) for s, off, p in periods[:5]]}; "
            f"fixture: {path}"
        )


def test_shape1_rejects_jagged_and_mixed_depth_arrays():
    """Shape 1 requires rectangular arrays of primitives: every array at the same
    depth has the same length and every leaf is at the same depth. Jagged or
    mixed-depth arrays must be rejected.
    """
    invalid = [
        [1, [2]],
        [[1], [[2]]],
        [[1, 2], [3, 4, 5]],
        [[1, 2], [3]],
        [[], [1]],
        [[1], []],
    ]
    for payload in invalid:
        assert not _validate_primitives_array(payload), (
            f"expected shape-1 rejection for jagged/mixed-depth array: {payload!r}"
        )

    valid = [
        [1, 2, 3],
        [[1, 2], [3, 4]],
        [[[1, 2]], [[3, 4]]],
        [True, False, None, "x", 1.5],
    ]
    for payload in valid:
        assert _validate_primitives_array(payload), (
            f"expected shape-1 acceptance for rectangular primitives: {payload!r}"
        )


def test_shape2_rejects_heterogeneous_outer_containers():
    """Shape 2 must be rejected when the outer container is heterogeneous, even if it
    contains a valid nested object array.
    """
    invalid = [
        [{"kind": "a", "value": 1}, [{"kind": "b", "value": 2}]],
        [{"kind": "a"}, [{"kind": "b"}, {"kind": "c"}]],
        [[{"kind": "a"}], {"kind": "b"}],
    ]
    for payload in invalid:
        assert not _validate_discriminator_array(payload), (
            f"expected shape-2 rejection for heterogeneous outer container: {payload!r}"
        )

    valid = [
        [{"kind": "a", "value": 1}, {"kind": "b", "value": 2}],
        [[{"kind": "a"}], [{"kind": "b"}]],
        [[{"kind": "a", "x": 1}, {"kind": "b", "x": 2}],
         [{"kind": "c", "x": 3}, {"kind": "d", "x": 4}]],
    ]
    for payload in valid:
        assert _validate_discriminator_array(payload), (
            f"expected shape-2 acceptance for uniform object array: {payload!r}"
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


def test_shape3_rejects_heterogeneous_outer_containers():
    """Shape 3 must be rejected when the outer container is heterogeneous, even if it
    contains a valid nested object array.
    """
    invalid = [
        [{"x": 1}, [{"x": 2}]],
        [{"x": 1}, [{"x": 2}, {"x": 3}]],
        [[{"x": 1}], {"x": 2}],
    ]
    for payload in invalid:
        assert not _validate_stable_keys_array(payload), (
            f"expected shape-3 rejection for heterogeneous outer container: {payload!r}"
        )


def test_booleans_are_distinct_primitive_type_from_int_and_string():
    """Python's bool is a subclass of int, so naive isinstance checks can accept a boolean
    where the grammar requires a string or integer. The canonical type-label logic must
    keep bool distinct from int and str in every structural position.
    """
    # bool is a primitive on its own.
    assert _is_primitive(True)
    assert _is_primitive(False)
    assert _primitive_type_label(True) == "bool"
    assert _primitive_type_label(False) == "bool"
    assert _primitive_type_label(1) == "int"
    assert _primitive_type_label(1.5) == "float"
    assert _primitive_type_label("x") == "str"

    # A boolean discriminator must not satisfy the string-discriminator requirement.
    assert not _validate_discriminator_array([{"kind": True}, {"kind": False}])

    # A boolean leaf must not be treated as the same primitive type as an integer leaf.
    assert not _validate_stable_keys_array([{"x": 1}, {"x": True}])
    assert not _validate_stable_keys_array([{"x": True}, {"x": 1}])

    # A uniform boolean column is still a valid primitive column.
    assert _validate_stable_keys_array([{"x": True}, {"x": False}])


def test_fenced_json_validates_complete_payload_not_any_inner_array():
    """Shape 4 requires the fenced inner content to independently satisfy one of shapes
    1-3. A heterogeneous payload that contains one valid nested array but also invalid
    siblings must not be accepted as shape 4 just because a reachable inner array matches.
    """
    payload = [
        [1, 2, 3],  # valid shape-1 on its own
        {"a": "x"},  # an object is not a primitive; whole payload is not shape-1
    ]
    assert not _validate_json_fenced(payload)
    # Sanity: the inner array alone is valid shape-1.
    assert _validate_primitives_array([1, 2, 3])

    # A list of valid shape-1 arrays is itself shape-1 (nested arrays of primitives).
    valid_nested = [[1, 2, 3], [4, 5, 6]]
    assert _validate_json_fenced(valid_nested)


def test_fenced_json_requires_exact_json_info_string_and_canonical_wrapper():
    """The fence parser must reject malformed fences: trailing whitespace in the info
    string, wrong case, text before/after the fence, or a fence that appears anywhere
    inside other content.
    """
    # Trailing space after `json` is rejected.
    assert _extract_fenced_json("```json \n[1]\n```") == []
    # Wrong case is rejected.
    assert _extract_fenced_json("```JSON\n[1]\n```") == []
    # Extra text before the fence is rejected.
    assert _extract_fenced_json("intro\n```json\n[1]\n```") == []
    # Extra text after the fence is rejected.
    assert _extract_fenced_json("```json\n[1]\n```\noutro") == []
    # A fence buried in the middle of text is rejected.
    assert _extract_fenced_json("before\n```json\n[1]\n```\nafter") == []
    # A canonical fence is accepted.
    assert _extract_fenced_json("```json\n[1]\n```") == ["[1]"]
    # Leading and trailing whitespace around a canonical fence is accepted.
    assert _extract_fenced_json("  \n```json\n[1]\n```\n  ") == ["[1]"]
