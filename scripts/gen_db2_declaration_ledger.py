#!/usr/bin/env python3
"""Inventory DB2 C declarations and bind reviewed migration dispositions."""

from __future__ import annotations

import argparse
from collections import defaultdict
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import sys
from typing import NoReturn


ROOT = Path(__file__).resolve().parent.parent
BOUNDARY = Path("src/modules/db2/c")
SOURCE_BASELINE = Path("tests/baselines/db2/source-boundary-v2.json")
REVIEW = Path("src/modules/db2/eventcontract/declaration-review.json")
OUTPUT = Path("tests/baselines/db2/declarations-v1.json")
MAX_BYTES = 1_048_576
MAX_TOKENS = 200_000
IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
KEYWORDS = {
    "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic",
    "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local", "auto",
    "break", "case", "char", "const", "continue", "default", "do", "double",
    "else", "enum", "extern", "float", "for", "goto", "if", "inline", "int",
    "long", "register", "restrict", "return", "short", "signed", "sizeof",
    "static", "struct", "switch", "typedef", "union", "unsigned", "void",
    "volatile", "while",
}
IGNORED_CALL_LIKE = {"__attribute__", "__declspec", "__asm__", "asm"}
# private-db2 stays inside the module; wire-operation crosses as a typed
# operation; linked-library crosses as neither, because it never reaches the
# store -- it is a library function that happens to live in the DB2 tree, and
# it stays linked into whichever binary calls it. The distinction from
# private-db2 is that these ARE called from outside DB2 and go on being.
DISPOSITIONS = {"private-db2", "wire-operation", "compatibility-wrapper",
                "linked-library"}
PLACEMENTS = {"retained-db2", "db3-eligible"}
FAMILIES = {
    "lifecycle", "tenancy", "memory", "index", "learning", "organization",
    "custody", "maintenance",
}
CONSUMER_CLASSES = {
    "host-generated-client", "kb-generated-client", "module-kb-contract",
    "module-placement-audit", "private-implementation-test", "server-kb-contract",
}


class LedgerError(ValueError):
    """A fail-closed inventory or review error."""


def fail(rule: str, message: str) -> NoReturn:
    raise LedgerError(f"rule={rule}: {message}")


@dataclass(frozen=True)
class Token:
    text: str
    line: int


@dataclass(frozen=True)
class Declaration:
    symbol: str
    signature: str
    header: str
    line: int


def _duplicates(label: str):
    def reject(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in pairs:
            if key in result:
                fail("json-duplicate-key", f"{label}: duplicate key {key!r}")
            result[key] = value
        return result
    return reject


def load_json(path: Path) -> object:
    if path.is_symlink():
        fail("json-symlink", f"refusing JSON symlink {path}")
    try:
        raw = path.read_bytes()
    except OSError as exc:
        fail("input", f"cannot read {path}: {exc}")
    if len(raw) > MAX_BYTES:
        fail("input-size", f"{path} exceeds {MAX_BYTES} bytes")
    if raw.startswith(b"\xef\xbb\xbf"):
        fail("json-bom", f"{path} begins with a UTF-8 BOM")
    try:
        return json.loads(raw.decode("utf-8", "strict"), object_pairs_hook=_duplicates(str(path)))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        fail("json-parse", f"{path}: {exc}")


def _without_directives(text: str, label: str) -> str:
    lines = text.splitlines(keepends=True)
    result: list[str] = []
    continuing = False
    for line in lines:
        directive = continuing or line.lstrip().startswith("#")
        if directive:
            body = line[:-1] if line.endswith("\n") else line
            continuing = body.rstrip().endswith("\\")
            result.append("\n" if line.endswith("\n") else "")
        else:
            result.append(line)
    if continuing:
        fail("preprocessor-continuation", f"{label}: unterminated preprocessor continuation")
    return "".join(result)


def tokenize(text: str, label: str, *, drop_directives: bool = True) -> list[Token]:
    """Tokenize the C subset needed for a fail-closed declaration inventory."""
    if "\x00" in text:
        fail("c-nul", f"{label}: NUL byte is forbidden")
    if drop_directives:
        text = _without_directives(text, label)
    tokens: list[Token] = []
    index = 0
    line = 1
    length = len(text)

    def add(value: str, source_line: int) -> None:
        tokens.append(Token(value, source_line))
        if len(tokens) > MAX_TOKENS:
            fail("c-token-limit", f"{label}: exceeds {MAX_TOKENS} tokens")

    while index < length:
        char = text[index]
        if char.isspace():
            line += char == "\n"
            index += 1
            continue
        if char == "/" and index + 1 < length and text[index + 1] == "/":
            index += 2
            while index < length and text[index] != "\n":
                index += 1
            continue
        if char == "/" and index + 1 < length and text[index + 1] == "*":
            start = line
            index += 2
            while index + 1 < length and text[index:index + 2] != "*/":
                line += text[index] == "\n"
                index += 1
            if index + 1 >= length:
                fail("c-comment", f"{label}:{start}: unterminated block comment")
            index += 2
            continue
        if char in {'"', "'"}:
            quote = char
            start = line
            content_start = index + 1
            index += 1
            while index < length:
                if text[index] == "\\":
                    index += 2
                    continue
                if text[index] == quote:
                    content = text[content_start:index]
                    index += 1
                    break
                if text[index] == "\n":
                    fail("c-literal", f"{label}:{start}: newline in literal")
                index += 1
            else:
                fail("c-literal", f"{label}:{start}: unterminated literal")
            add("<linkage-C>" if quote == '"' and content == "C" else "<literal>", start)
            continue
        if char.isalpha() or char == "_":
            start = index
            while index < length and (text[index].isalnum() or text[index] == "_"):
                index += 1
            add(text[start:index], line)
            continue
        if char.isdigit():
            start = index
            while index < length and (text[index].isalnum() or text[index] in "._"):
                index += 1
            add(text[start:index], line)
            continue
        if index + 2 < length and text[index:index + 3] in {"...", "<<=", ">>="}:
            add(text[index:index + 3], line)
            index += 3
            continue
        if index + 1 < length and text[index:index + 2] in {
            "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
            "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##",
        }:
            add(text[index:index + 2], line)
            index += 2
            continue
        add(char, line)
        index += 1
    return tokens


def _normalize(tokens: list[Token]) -> str:
    return " ".join(token.text for token in tokens)


def _declaration(segment: list[Token], header: str) -> Declaration | None:
    texts = [token.text for token in segment]
    if not segment or "(" not in texts or "typedef" in texts or "static" in texts:
        return None
    paren = 0
    bracket = 0
    candidates: list[tuple[int, str]] = []
    top_level_equals = False
    for index, token in enumerate(segment):
        text = token.text
        if text == "[":
            bracket += 1
        elif text == "]":
            bracket -= 1
            if bracket < 0:
                fail("c-bracket", f"{header}:{token.line}: unmatched ']'")
        elif text == "(":
            if paren == 0 and bracket == 0 and index > 0:
                name = segment[index - 1].text
                if (IDENTIFIER.fullmatch(name) and name not in KEYWORDS and
                        name not in IGNORED_CALL_LIKE and not name.isupper()):
                    candidates.append((index - 1, name))
                elif (index + 2 < len(segment) and
                      IDENTIFIER.fullmatch(segment[index + 1].text) and
                      segment[index + 2].text == ")"):
                    parenthesized = segment[index + 1].text
                    if parenthesized not in KEYWORDS and not parenthesized.isupper():
                        candidates.append((index + 1, parenthesized))
            paren += 1
        elif text == ")":
            paren -= 1
            if paren < 0:
                fail("c-parenthesis", f"{header}:{token.line}: unmatched ')'")
        elif text == "=" and paren == 0 and bracket == 0:
            top_level_equals = True
    if paren or bracket:
        fail("c-declaration-balance", f"{header}:{segment[0].line}: unbalanced declaration")
    if top_level_equals:
        return None
    unique = list(dict.fromkeys(name for _, name in candidates))
    if not unique:
        if texts[0] == "_Static_assert":
            return None
        rule = "unsupported-extern" if "extern" in texts else "unsupported-declaration"
        fail(rule, f"{header}:{segment[0].line}: unsupported parenthesized declaration")
    if len(unique) != 1:
        fail("ambiguous-declaration", f"{header}:{segment[0].line}: candidates={unique}")
    name = unique[0]
    name_index = next(index for index, candidate in candidates if candidate == name)
    return Declaration(name, _normalize(segment), header, segment[name_index].line)


def declarations_from_text(text: str, header: str) -> list[Declaration]:
    tokens = tokenize(text, header)
    result: list[Declaration] = []
    segment: list[Token] = []
    braces = 0
    linkage = 0
    for token in tokens:
        if token.text == "{":
            if braces == 0:
                if [item.text for item in segment] == ["extern", "<linkage-C>"]:
                    linkage += 1
                    segment = []
                    continue
                if segment and segment[0].text == "extern" and \
                        any(item.text in {"<literal>", "<linkage-C>"} for item in segment):
                    fail("c-linkage", f"{header}:{segment[0].line}: unsupported linkage block")
                segment = []
            braces += 1
            continue
        if token.text == "}":
            if braces == 0 and linkage:
                linkage -= 1
                segment = []
                continue
            braces -= 1
            if braces < 0:
                fail("c-brace", f"{header}:{token.line}: unmatched '}}'")
            if braces == 0:
                segment = []
            continue
        if braces:
            continue
        if token.text == ";":
            declaration = _declaration(segment, header)
            if declaration is not None:
                result.append(declaration)
            segment = []
        else:
            segment.append(token)
    if braces:
        fail("c-brace", f"{header}: unclosed top-level brace")
    if linkage:
        fail("c-linkage", f"{header}: unclosed extern linkage block")
    if any(token.text not in {"extern", "<linkage-C>"} for token in segment):
        fail("c-trailing", f"{header}:{segment[0].line}: unsupported trailing tokens")
    return result


def _headers(root: Path) -> list[Path]:
    boundary = root / BOUNDARY
    if not boundary.is_dir() or boundary.is_symlink():
        fail("boundary", f"{BOUNDARY} must be a real directory")
    headers = sorted(boundary.glob("*.h"))
    if any(path.is_symlink() or not path.is_file() for path in headers):
        fail("header-symlink", "DB2 headers must be regular files")
    return headers


def extract_declarations(root: Path) -> dict[str, dict[str, object]]:
    grouped: dict[str, list[Declaration]] = defaultdict(list)
    for path in _headers(root):
        try:
            raw = path.read_bytes()
            text = raw.decode("utf-8", "strict")
        except (OSError, UnicodeError) as exc:
            fail("header-input", f"cannot read {path}: {exc}")
        if len(raw) > MAX_BYTES:
            fail("header-size", f"{path} exceeds {MAX_BYTES} bytes")
        relative = path.relative_to(root).as_posix()
        for declaration in declarations_from_text(text, relative):
            grouped[declaration.symbol].append(declaration)

    result: dict[str, dict[str, object]] = {}
    for symbol in sorted(grouped):
        rows = sorted(grouped[symbol], key=lambda row: (row.header, row.line, row.signature))
        signatures = sorted({row.signature for row in rows})
        if len(signatures) != 1:
            fail("conflicting-declaration", f"{symbol} has conflicting signatures")
        signature = signatures[0]
        locations = [{"header": row.header, "line": row.line} for row in rows]
        if len({(row.header, row.line) for row in rows}) != len(rows):
            fail("duplicate-location", f"{symbol} repeats at one source location")
        result[symbol] = {
            "signature": signature,
            "signature_sha256": hashlib.sha256(signature.encode("utf-8")).hexdigest(),
            "locations": locations,
        }
    return result


def _identifier_uses(path: Path) -> set[str]:
    try:
        raw = path.read_bytes()
        text = raw.decode("utf-8", "strict")
    except (OSError, UnicodeError) as exc:
        fail("consumer-input", f"cannot read {path}: {exc}")
    if len(raw) > MAX_BYTES:
        fail("consumer-size", f"{path} exceeds {MAX_BYTES} bytes")
    return {
        token.text for token in tokenize(text, str(path), drop_directives=False)
        if IDENTIFIER.fullmatch(token.text)
    }


def consumer_index(
    root: Path, declarations: dict[str, dict[str, object]],
) -> tuple[dict[str, list[str]], list[dict[str, str]]]:
    baseline = load_json(root / SOURCE_BASELINE)
    if not isinstance(baseline, dict) or not isinstance(baseline.get("consumers"), list):
        fail("source-baseline", f"{SOURCE_BASELINE} has no consumers array")
    symbols = set(declarations)
    result: dict[str, list[str]] = defaultdict(list)
    classifications: list[dict[str, str]] = []
    previous = ""
    for index, row in enumerate(baseline["consumers"]):
        if (not isinstance(row, dict) or set(row) != {"path", "classification", "includes"} or
                not isinstance(row.get("path"), str) or
                row.get("classification") not in CONSUMER_CLASSES or
                not isinstance(row.get("includes"), list)):
            fail("source-baseline", f"consumer {index} has an invalid shape/classification")
        relative = row["path"]
        if relative <= previous:
            fail("source-baseline-order", "consumer paths must be sorted and unique")
        previous = relative
        path = PurePosixPath(relative)
        if path.is_absolute() or ".." in path.parts:
            fail("consumer-path", f"unsafe consumer path {relative!r}")
        source = root.joinpath(*path.parts)
        current = root
        for part in path.parts:
            current /= part
            if current.is_symlink():
                fail("consumer-path", f"consumer path contains a symlink: {relative!r}")
        if not source.exists():
            continue
        if not source.is_file():
            fail("consumer-path", f"consumer path is not a regular file: {relative!r}")
        classifications.append({
            "path": relative,
            "classification": row["classification"],
        })
        uses = _identifier_uses(source) & symbols
        for symbol in uses:
            result[symbol].append(relative)
    return ({symbol: sorted(paths) for symbol, paths in result.items()}, classifications)


def _review_rows(value: object, declarations: dict[str, dict[str, object]]) -> dict[str, dict[str, str]]:
    if not isinstance(value, dict) or set(value) != {
        "schema_version", "module", "declarations_complete", "reviews",
    }:
        fail("review-shape", "review document has invalid top-level keys")
    if value["schema_version"] != 1 or value["module"] != "db2" or \
            type(value["declarations_complete"]) is not bool:
        fail("review-version", "review document identity/version is invalid")
    if not isinstance(value["reviews"], list):
        fail("review-shape", "reviews must be an array")
    result: dict[str, dict[str, str]] = {}
    previous = ""
    for index, row in enumerate(value["reviews"]):
        expected = {
            "symbol", "signature_sha256", "disposition", "family", "db3_placement", "reason",
        }
        if not isinstance(row, dict) or set(row) != expected:
            fail("review-shape", f"review {index} has invalid keys")
        if not all(isinstance(row[key], str) for key in expected):
            fail("review-type", f"review {index} fields must be strings")
        symbol = row["symbol"]
        if symbol <= previous:
            fail("review-order", "reviews must be sorted by unique symbol")
        previous = symbol
        declaration = declarations.get(symbol)
        if declaration is None:
            fail("review-symbol", f"review names unknown declaration {symbol!r}")
        if row["signature_sha256"] != declaration["signature_sha256"]:
            fail("review-signature", f"review signature for {symbol} is stale")
        if row["disposition"] not in DISPOSITIONS or row["family"] not in FAMILIES or \
                row["db3_placement"] not in PLACEMENTS or not row["reason"]:
            fail("review-value", f"review for {symbol} has an invalid disposition")
        if len(row["reason"].encode("utf-8")) > 512:
            fail("review-value", f"review reason for {symbol} exceeds 512 UTF-8 bytes")
        if symbol.startswith("pgvec_") and (row["disposition"] != "private-db2" or
                                              row["db3_placement"] != "retained-db2"):
            fail("pgvector-placement", f"{symbol} must remain private and retained in DB2")
        if row["disposition"] == "private-db2" and row["db3_placement"] != "retained-db2":
            fail("private-placement", f"private declaration {symbol} must remain in DB2")
        result[symbol] = row
    return result


def build(root: Path) -> dict[str, object]:
    declarations = extract_declarations(root)
    consumers, consumer_classes = consumer_index(root, declarations)
    classes = {row["path"]: row["classification"] for row in consumer_classes}
    review_doc = load_json(root / REVIEW)
    reviews = _review_rows(review_doc, declarations)
    rows: list[dict[str, object]] = []
    pending = 0
    reviewed = 0
    internal = 0
    test_only = 0
    for symbol, declaration in declarations.items():
        paths = consumers.get(symbol, [])
        production_consumed = any(
            classes[path] != "private-implementation-test" for path in paths
        )
        review = reviews.get(symbol)
        if review is not None:
            status = "reviewed"
            reviewed += 1
        elif production_consumed:
            status = "audit-pending"
            pending += 1
        elif paths:
            status = "private-test-only"
            test_only += 1
        else:
            status = "internal-unconsumed"
            internal += 1
        row: dict[str, object] = {
            "symbol": symbol,
            **declaration,
            "consumers": paths,
            "status": status,
        }
        if review is not None:
            row["review"] = {
                key: review[key]
                for key in ("disposition", "family", "db3_placement", "reason")
            }
        rows.append(row)
    complete = review_doc["declarations_complete"]
    if complete and pending:
        fail("premature-completeness", f"{pending} externally consumed declarations remain pending")
    if complete:
        required = {
            row["symbol"] for row in rows
            if any(classes[path] != "private-implementation-test" for path in row["consumers"])
        }
        if not required.issubset(reviews):
            fail("review-completeness", "complete review must cover every production declaration")
    payload = {
        "schema_version": 1,
        "module": "db2",
        "declarations_complete": complete,
        "consumer_classes": consumer_classes,
        "summary": {
            "headers": len(_headers(root)),
            "declarations": len(rows),
            "reviewed": reviewed,
            "audit_pending": pending,
            "internal_unconsumed": internal,
            "private_test_only": test_only,
        },
        "declarations": rows,
    }
    canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"),
                           ensure_ascii=True).encode("ascii")
    payload["fingerprint"] = hashlib.sha256(canonical).hexdigest()
    return payload


def output_bytes(root: Path) -> bytes:
    return (json.dumps(build(root), indent=2, sort_keys=True) + "\n").encode("utf-8")


def run(root: Path, write: bool) -> None:
    expected = output_bytes(root)
    target = root / OUTPUT
    current = root
    for part in OUTPUT.parent.parts:
        current /= part
        if current.is_symlink():
            fail("output-symlink", f"output path contains symlink {current}")
    if target.is_symlink():
        fail("output-symlink", f"refusing output symlink {target}")
    if write:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(expected)
        return
    try:
        actual = target.read_bytes()
    except OSError as exc:
        fail("generated-input", f"cannot read {OUTPUT}: {exc}")
    if actual != expected:
        fail("generated-drift", f"{OUTPUT} is not generated from the DB2 declaration surface")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)
    try:
        run(args.root.resolve(), args.write)
    except (LedgerError, OSError, UnicodeError, ValueError) as exc:
        print(f"gen_db2_declaration_ledger: error: {exc}", file=sys.stderr)
        return 1
    action = "wrote" if args.write else "ok"
    print(f"gen_db2_declaration_ledger: {action} ({OUTPUT})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
