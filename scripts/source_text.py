"""Read code as code, not as prose about code.

WHY THIS EXISTS. A guard that greps a source file for a token cannot tell the
token from a sentence discussing it, and the sentence is usually the more
quotable of the two. Both directions are real, and both were found in this
tree on the same day:

  FALSE CLEAN   check-memory-store-degradation decided a file "degrades
                gracefully" if AIMEE_DB2_DISABLED appeared anywhere in it. A
                file that reached the store silently, carrying a TODO comment
                promising to branch on that macro and to LOG_WARN, was
                classified as already doing both. The guard read the intent as
                the implementation.

  FALSE ALARM   check-log-prefix-ownership reported a module as emitting
                another module's log prefix when the only occurrence was a
                comment WARNING that it must never do so. A guard that fails on
                the sentence explaining its own rule makes deleting the
                explanation the cheapest way to pass.

The false alarm is the more corrosive of the two: false cleans hide a defect,
but a guard that punishes rationale actively removes the reasoning future
readers need.

QUOTES ARE TRACKED, NOT IGNORED. A '//' inside a string literal -- the one in
"http://host/" -- does not open a comment, and blanking from there would
truncate the literal, which in these checks is the very thing being read.

Importable because sys.path[0] is the script's own directory. That holds for a
plain `python3 scripts/foo.py` and NOT under `python3 -I`, which isolates the
path; a caller that needs -I must inline the helper instead.
"""

__all__ = ["strip_comments", "strip_comments_text"]


def strip_comments(line: str, in_block: bool) -> tuple[str, bool]:
    """Blank C/Go comments in one line; return it and the block-comment state.

    Carrying `in_block` across lines is what makes a multi-line /* ... */
    comment disappear entirely rather than only its first line.
    """
    out: list[str] = []
    i = 0
    quote = ""
    n = len(line)
    while i < n:
        ch = line[i]
        if in_block:
            if line.startswith("*/", i):
                in_block = False
                i += 2
                continue
            i += 1
            continue
        if quote:
            out.append(ch)
            # A backslash escape consumes the next character, so a literal
            # \" does not read as the end of the string.
            if ch == "\\" and i + 1 < n:
                out.append(line[i + 1])
                i += 2
                continue
            if ch == quote:
                quote = ""
            i += 1
            continue
        if ch in "\"'`":
            quote = ch
            out.append(ch)
            i += 1
            continue
        if line.startswith("//", i):
            break
        if line.startswith("/*", i):
            in_block = True
            i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out), in_block


def strip_comments_text(text: str) -> str:
    """Whole-file form, for checks that search the file rather than its lines."""
    in_block = False
    kept = []
    for raw in text.splitlines():
        line, in_block = strip_comments(raw, in_block)
        kept.append(line)
    return "\n".join(kept)
