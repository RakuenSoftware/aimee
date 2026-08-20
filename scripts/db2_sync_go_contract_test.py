"""Bring the Go contract test up to the catalog.

The Go test pins the whole operation order in one guard and then covers each
operation individually. The guard is a transcription of the catalog, so every
operation added moves the positions below it -- which has already produced two
defects: an index rewritten in one place and not the other, so a test asserted
against its neighbour's fixture and passed.

This regenerates the guard from the catalog and adds a request round trip for
every described operation that does not have one. The hand-written cases for
the earlier formats are left alone: they check things a generated case cannot,
like which of two decoders accepts a request.

    python3 scripts/db2_sync_go_contract_test.py
"""
import json
import re
import sys
from pathlib import Path

CATALOG = Path("src/modules/db2/eventcontract/operations.json")
TEST = Path("server-go/db2/contract_test.go")
INITIALISMS = {"id": "ID", "kb": "KB", "ok": "OK", "pg": "PG", "url": "URL"}


def go_name(value: str) -> str:
    return "".join(INITIALISMS.get(part, part.title()) for part in value.split("_"))


def go_local(value: str) -> str:
    name = go_name(value)
    return name[0].lower() + name[1:]


def main() -> int:
    catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    operations = catalog["operations"]
    text = TEST.read_text(encoding="utf-8")

    # --- the guard, rewritten from the catalog -------------------------------
    start = text.index("\tif len(baseline.Operations) != ")
    end = text.index("\t\tt.Fatalf(\"unexpected operations", start)
    lines = [f"\tif len(baseline.Operations) != {len(operations)} ||"]
    for index, operation in enumerate(operations):
        terminator = " ||" if index + 1 < len(operations) else " {"
        lines.append(f'\t\tbaseline.Operations[{index}].Name != "{operation["name"]}"{terminator}')
    text = text[:start] + "\n".join(lines) + "\n" + text[end:]

    # --- a request round trip for every described operation ------------------
    described = [operation for operation in operations
                 if operation["wire_format"] == "db2-envelope-generic-v1"]
    cases = []
    for operation in described:
        name = str(operation["name"])
        if f"func Test{go_name(name)}MatchesEverySharedCVector(" in text:
            continue
        fields = operation["request"]["fields"]
        arguments = ", ".join(
            f"operation.Request.{go_name(str(field['name']))}" for field in fields)
        results = ", ".join(go_local(str(field["name"])) for field in fields)
        comparisons = " ||\n\t\t".join(
            f"{go_local(str(field['name']))} != operation.Request.{go_name(str(field['name']))}"
            for field in fields)
        blanks = ", ".join("_" for _ in fields)
        cases.append(f'''func Test{go_name(name)}MatchesEverySharedCVector(t *testing.T) {{
	operation := loadWireBaseline(t).Operations[operationIndex(t, "{name}")]

	request, err := Encode{go_name(name)}Request({arguments})
	if err != nil || hex.EncodeToString(request) != operation.Request.Positive {{
		t.Fatalf("request encode: %v %x", err, request)
	}}
	{results}, err := Decode{go_name(name)}Request(request)
	if err != nil || {comparisons} {{
		t.Fatalf("request decode: %v", err)
	}}
	for _, vector := range operation.Request.Negative {{
		if {blanks}, err := Decode{go_name(name)}Request(decodeHex(t, vector.Hex)); err == nil {{
			t.Fatalf("request %s decoded", vector.Mutation)
		}}
	}}
}}

''')

    if cases:
        anchor = "func TestEntityEdgePruneOrphansMatchesEverySharedCVector(t *testing.T) {"
        if text.count(anchor) != 1:
            print("db2_sync_go_contract_test: no anchor for the new cases", file=sys.stderr)
            return 1
        text = text.replace(anchor, "".join(cases) + anchor)

    # --- struct fields the new schemas need ----------------------------------
    wanted: dict[str, str] = {}
    for operation in described:
        for field in operation["request"]["fields"]:
            tag = str(field["name"])
            kind = {"utf8": "string", "u32": "uint32", "u64": "uint64",
                    "f64": "float64"}[field["type"]]
            wanted[tag] = kind
    # The wide request struct, not the envelope fixture's narrow one: both are
    # spelled `Operations []struct {`, and the first is the wrong one.
    request_start = text.rindex("\tOperations []struct {", 0,
                                text.index("\t\t\tSourceSession"))
    request_end = text.index('\t\t} `json:"request"`', request_start)
    additions = []
    for tag, kind in wanted.items():
        if f'json:"{tag}"' in text[request_start:request_end]:
            continue
        additions.append(f'\t\t\t{go_name(tag):<27}{kind:<9}`json:"{tag}"`\n')
    if additions:
        # The request struct's own Negative block, found from inside it: the
        # same text opens the reply struct too, and anchoring on the first
        # occurrence put the fields in whichever struct came first.
        # gofmt pads the field name to align the struct, so the anchor is the
        # line that opens the negative-vector list rather than its name.
        index = text.rindex("\t\t\tNegative", request_start, request_end)
        text = text[:index] + "".join(additions) + text[index:]

    TEST.write_text(text, encoding="utf-8")
    print(f"db2_sync_go_contract_test: guard pins {len(operations)} operations; "
          f"{len(cases)} case(s) added")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
