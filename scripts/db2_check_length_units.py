"""Columns whose CHECK counts characters while the wire counts bytes.

PostgreSQL's length() and char_length() count CHARACTERS. The wire counts
BYTES. Where a column is checked at N characters and the field carrying it is
declared at N bytes, a value of N multi-byte characters passes the database
check and is then refused by the decoder -- so the row is written and can never
be read back. Not corrupted: unreachable, and nothing at write time says so.

A check in the wrong unit is worse than no check, because it looks covered.

Found by the db1 module hitting the same shape from the other direction: twelve
unbounded TEXT columns behind 1 MiB reply fields, where nothing refused an
oversized write and every later read failed forever. Their fix used
octet_length; this asks whether db2's existing checks are in the right unit to
begin with.
"""
import json
import re
import sys
from pathlib import Path

SCHEMA = Path("src/modules/db2/c/schema.sql")
CATALOG = Path("src/modules/db2/eventcontract/operations.json")

# CHECK (char_length(col) BETWEEN a AND b) or (length(col) <= n)
CHECKS = re.compile(
    r"\b(?:char_length|length)\s*\(\s*(\w+)\s*\)\s*"
    r"(?:BETWEEN\s+\d+\s+AND\s+(\d+)|<=\s*(\d+))", re.I)


def column_limits() -> dict:
    """column -> the character limits its CHECK constraints impose."""
    found: dict[str, set[int]] = {}
    for match in CHECKS.finditer(SCHEMA.read_text()):
        column = match.group(1)
        limit = int(match.group(2) or match.group(3))
        found.setdefault(column, set()).add(limit)
    return found


def field_limits() -> dict:
    """field -> the byte limits the wire declares for it."""
    catalog = json.loads(CATALOG.read_text())
    found: dict[str, set[int]] = {}

    def walk(fields):
        for field in fields or []:
            if field.get("type") == "utf8" and "maximum_bytes" in field:
                found.setdefault(field["name"], set()).add(field["maximum_bytes"])

    for operation in catalog["operations"]:
        walk(operation.get("request", {}).get("fields"))
        walk(operation.get("reply", {}).get("fields"))
        walk((operation.get("reply", {}).get("row") or {}).get("fields"))
    return found


# A regex that pins the column to ASCII. If one is present, length() and
# octet_length() cannot disagree and the unit does not matter.
#
# Negated classes do NOT qualify: `[^%:[:cntrl:]]` excludes two ASCII
# characters and admits every multi-byte sequence there is, which is exactly
# how kb_write_tier_grant.subject came to accept 576 characters into a 576-byte
# buffer.
ASCII_CLASS = re.compile(r"~\s*'(\^[^']*\$)'")
NEGATED = re.compile(r"\[\^")
NON_ASCII_SAFE = re.compile(r"[^\x00-\x7f]")


def column_predicates() -> dict:
    """column -> the text of every CHECK predicate mentioning it.

    Read per statement so a regex on one column is not credited to another.
    """
    text = SCHEMA.read_text()
    found: dict[str, list[str]] = {}
    for match in re.finditer(r"CHECK\s*\((.*?)\)\s*(?:,|\)|$)", text, re.S):
        predicate = match.group(1)
        for name in re.findall(r"\b(?:char_length|length)\s*\(\s*(\w+)\s*\)", predicate):
            found.setdefault(name, []).append(predicate)
    return found


def restricted_to_ascii(predicates: list) -> bool:
    for predicate in predicates:
        for regex in ASCII_CLASS.findall(predicate):
            if NEGATED.search(regex):
                # Admits every multi-byte sequence.
                continue
            if NON_ASCII_SAFE.search(regex):
                continue
            return True
    return False


def report_unrestricted() -> int:
    """Columns counted in characters that can hold multi-byte text."""
    predicates = column_predicates()
    suspects = sorted(name for name, texts in predicates.items()
                      if not restricted_to_ascii(texts))
    print(f"\n{len(suspects)} of {len(predicates)} character-counted column(s) have no "
          f"ASCII-restricting regex, so characters and bytes can differ:")
    for name in suspects:
        print(f"   {name}")
    if suspects:
        print("\nEach is suspect on its own. Where the value is also bounded in "
              "BYTES by a C buffer or the wire, a full-length value is written "
              "and cannot be read back.")
    actionable = report_buffer_conflicts(suspects)
    report_size_disagreements()
    return actionable


# char field[512] or char field[SOMETHING_MAX + 1]
BUFFER = re.compile(r"\bchar\s+(\w+)\s*\[\s*([A-Z0-9_]+)\s*(?:\+\s*1\s*)?\]")
DEFINE = re.compile(r"#define\s+([A-Z0-9_]+)\s+(\d+)")


def c_buffers() -> dict:
    """field name -> the byte sizes C holds it in."""
    sizes: dict[str, int] = {}
    fields: dict[str, set] = {}
    for path in sorted(Path("src").rglob("*.h")):
        try:
            text = path.read_text()
        except (OSError, UnicodeDecodeError):
            continue
        for name, value in DEFINE.findall(text):
            sizes[name] = int(value)
        for name, extent in BUFFER.findall(text):
            if extent.isdigit():
                # char buf[N] holds N-1 bytes plus a NUL.
                fields.setdefault(name, set()).add(int(extent) - 1)
            elif extent in sizes:
                fields.setdefault(name, set()).add(sizes[extent])
    return fields


def report_buffer_conflicts(suspects: list) -> int:
    """Suspect columns a C buffer bounds in bytes at the same number."""
    buffers = c_buffers()
    limits = column_limits()
    actionable = []
    for name in suspects:
        held = buffers.get(name)
        if not held:
            continue
        shared = (limits.get(name) or set()) & held
        if shared:
            actionable.append((name, sorted(shared)))

    print(f"\n{len(actionable)} of those are bounded in BYTES by a C buffer at the "
          f"same number:")
    for name, shared in actionable:
        print(f"   {name:30s} {shared}  (char {name}[N+1])")
    if actionable:
        print("\nThese are the actionable ones: the database accepts a value the "
              "buffer cannot hold.")
    return len(actionable)


def report_size_disagreements() -> int:
    """Columns whose readers are sized differently from each other or the column.

    A unit fix aligns the CHECK with one buffer. Where two readers disagree
    about the number, aligning to either is a decision about what the value may
    be, and making it silently is how the disagreement gets buried.
    """
    buffers = c_buffers()
    limits = column_limits()
    disagreements = []
    for name, held in sorted(buffers.items()):
        checked = limits.get(name)
        if not checked:
            continue
        sizes = set(held)
        if len(sizes) > 1:
            disagreements.append((name, sorted(checked), sorted(sizes), "readers differ"))
        elif sizes and not (sizes & checked):
            disagreements.append((name, sorted(checked), sorted(sizes), "column differs"))

    if not disagreements:
        print("\nno checked column has readers that disagree about its size")
        return 0
    print(f"\n{len(disagreements)} column(s) whose size is not agreed:")
    for name, checked, sizes, why in disagreements:
        print(f"   {name:30s} column={checked} readers={sizes}  ({why})")
    print("\nThese are questions about what the value MAY BE, not about units. "
          "Aligning to whichever reader was found first would bury the "
          "disagreement rather than answer it.")
    return len(disagreements)


def main() -> int:
    columns = column_limits()
    fields = field_limits()
    print(f"{len(columns)} checked column(s), {len(fields)} declared utf8 field(s)")

    suspects = []
    for name, char_limits in sorted(columns.items()):
        byte_limits = fields.get(name)
        if not byte_limits:
            continue
        # The dangerous case is the same number in two units: a value of N
        # multi-byte characters passes the column check and exceeds the field.
        shared = char_limits & byte_limits
        if shared:
            suspects.append((name, sorted(shared)))

    if not suspects:
        print("\nno column is checked in characters at the same number the wire "
              "declares in bytes")
        return 1 if report_unrestricted() else 0

    print(f"\n{len(suspects)} column(s) checked in CHARACTERS at the same number "
          f"the wire declares in BYTES:")
    for name, limits in suspects:
        print(f"   {name:34s} {limits}")
    print("\nA value of that many multi-byte characters is accepted by the "
          "database and refused by the wire, so the row cannot be read back.")
    report_unrestricted()
    return 1


if __name__ == "__main__":
    sys.exit(main())
