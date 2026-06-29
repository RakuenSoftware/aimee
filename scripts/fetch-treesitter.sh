#!/bin/sh
# fetch-treesitter.sh — fetch the tree-sitter runtime + grammars for the §2 opt-in
# tree-sitter extraction front-end (code_treesitter.c, built with AIMEE_TREESITTER=1).
#
# These are multi-MB GENERATED parsers, so they are fetched here rather than committed
# (see .gitignore). Pinned to specific commits for reproducibility. Idempotent: skips a
# grammar that is already present. Add a language by appending a `fetch <repo> <sha>`
# line and registering it in code_treesitter.c.
set -e

VENDOR="$(cd "$(dirname "$0")/../src/vendor" && pwd)"

fetch() {
    name="$1"; url="$2"; sha="$3"; dest="$VENDOR/$name"
    if [ -e "$dest/.fetched" ]; then
        echo "fetch-treesitter: $name present, skipping"
        return 0
    fi
    echo "fetch-treesitter: $name @ $sha"
    rm -rf "$dest"
    # Never block on an interactive credential prompt in an automated build; the
    # exact commit SHA is checked out (content-addressed) and re-verified by sha256
    # at the end, so a moved ref cannot ship a changed parser.
    GIT_TERMINAL_PROMPT=0 git clone -q "$url" "$dest"
    git -C "$dest" checkout -q "$sha"
    rm -rf "$dest/.git"
    touch "$dest/.fetched"
}

# Runtime (one amalgamated compilation unit via lib/src/lib.c).
fetch tree-sitter   https://github.com/tree-sitter/tree-sitter   cbee4672665173d1702d836353ef7648dc2b2fac
# Grammars — one parser.c each (TypeScript and PHP ship two: a base + a JSX/embedded
# variant), plus a src/scanner.c for those with an external scanner. The set mirrors the
# hand-rolled supported languages (src/extractors.c). C first — aimee dogfoods its source.
fetch tree-sitter-c          https://github.com/tree-sitter/tree-sitter-c          b780e47fc780ddc8da13afa35a3f4ed5c157823d
fetch tree-sitter-cpp        https://github.com/tree-sitter/tree-sitter-cpp        8b5b49eb196bec7040441bee33b2c9a4838d6967
fetch tree-sitter-c-sharp    https://github.com/tree-sitter/tree-sitter-c-sharp    af29416d729b7a6603101b513604392d8f675e3b
fetch tree-sitter-python     https://github.com/tree-sitter/tree-sitter-python     26855eabccb19c6abf499fbc5b8dc7cc9ab8bc64
fetch tree-sitter-go         https://github.com/tree-sitter/tree-sitter-go         2346a3ab1bb3857b48b29d779a1ef9799a248cd7
fetch tree-sitter-javascript https://github.com/tree-sitter/tree-sitter-javascript 58404d8cf191d69f2674a8fd507bd5776f46cb11
fetch tree-sitter-typescript https://github.com/tree-sitter/tree-sitter-typescript 75b3874edb2dc714fb1fd77a32013d0f8699989f
fetch tree-sitter-rust       https://github.com/tree-sitter/tree-sitter-rust       77a3747266f4d621d0757825e6b11edcbf991ca5
fetch tree-sitter-java       https://github.com/tree-sitter/tree-sitter-java       e10607b45ff745f5f876bfa3e94fbcc6b44bdc11
fetch tree-sitter-ruby       https://github.com/tree-sitter/tree-sitter-ruby       ad907a69da0c8a4f7a943a7fe012712208da6dee
fetch tree-sitter-php        https://github.com/tree-sitter/tree-sitter-php        38216983c07bf9e1b56e16acde53b25adaeab61c
fetch tree-sitter-lua        https://github.com/tree-sitter-grammars/tree-sitter-lua 10fe0054734eec83049514ea2e718b2a56acd0c9
fetch tree-sitter-bash       https://github.com/tree-sitter/tree-sitter-bash       a06c2e4415e9bc0346c6b86d401879ffb44058f7
# Swift's generated parser.c is not on the default branch; use the *-with-generated-files tag.
fetch tree-sitter-swift      https://github.com/alex-pinkus/tree-sitter-swift      caa99d7d3c14aac03b5f16fc86fedf8755570760
fetch tree-sitter-kotlin     https://github.com/fwcd/tree-sitter-kotlin           c8ac3d2627240160b999a2c100de3babbdb8f419
fetch tree-sitter-dart       https://github.com/UserNobody14/tree-sitter-dart      a9bdfa3db2fbc9b9f12c93450d04a671f33a5102
fetch tree-sitter-css        https://github.com/tree-sitter/tree-sitter-css        dda5cfc5722c429eaba1c910ca32c2c0c5bb1a3f

echo "fetch-treesitter: done -> $VENDOR/tree-sitter*"

# Integrity: verify the fetched, compiled grammar/runtime sources against the
# recorded sha256 manifest. The pinned commit SHAs above already fix the content
# (git checkout is content-addressed); this is defense-in-depth against a moved
# ref / upstream history rewrite / a SHA edited in this script without updating the
# hashes — any mismatch fails the build instead of silently shipping a changed
# parser. Regenerate after an intentional grammar bump (resolve the manifest path
# BEFORE the cd, and write via a temp file so the input is not truncated mid-read):
#   M=scripts/treesitter-sha256.txt; A="$PWD/$M"
#   (cd src/vendor && sha256sum $(awk '{print $2}' "$A")) > "$M.tmp" && mv "$M.tmp" "$M"
MANIFEST="$(cd "$(dirname "$0")" && pwd)/treesitter-sha256.txt"
echo "fetch-treesitter: verifying sha256 ($MANIFEST)"
if ! (cd "$VENDOR" && sha256sum --quiet -c "$MANIFEST"); then
    echo "fetch-treesitter: SHA256 VERIFICATION FAILED — refusing to build" >&2
    exit 1
fi
echo "fetch-treesitter: sha256 OK"
