#!/bin/sh
# ci-annotate-log-tail.sh LOG TITLE [LINES]
#
# Emit a failed CI step's diagnosis as one GitHub Actions error annotation.
# Annotations show on the check run itself, so a failure is readable without
# opening the raw job log -- including one that happens before any test could
# record itself (build, template setup, a killed runner process).
#
# The tail alone is not enough: a parallel make keeps compiling other objects
# after the one that failed, so the error scrolls out of the last lines and the
# tail shows only command lines. The annotation therefore leads with every line
# that names a failure (compiler and linker errors, missing rules, make's
# "Error N", failed tests and assertions), then the last LINES (default 40)
# lines. Each line is cut to 300 characters so a compiler command line cannot
# crowd out the rest.
set -eu
log=$1
title=$2
lines=${3:-40}
if [ ! -s "$log" ]; then
  echo "::error title=$title::the step wrote no output"
  exit 0
fi
failures=$(grep -E 'error:|fatal error|No rule to make target|undefined reference|\*\*\* .*Error [0-9]+|^FAILED|FAILED:|Assertion .* failed|Segmentation fault|Aborted' "$log" |
  cut -c1-300 | awk '!seen[$0]++' | head -n 30 || true)
tail_text=$(tail -n "$lines" "$log" | cut -c1-300)
if [ -n "$failures" ]; then
  body=$(printf '%s\n--- last %s lines ---\n%s\n' "$failures" "$lines" "$tail_text")
else
  body=$tail_text
fi
# Workflow commands treat %, CR and LF specially in the message; ',' and ':'
# only matter in properties, so the title is kept to plain words.
printf '::error title=%s::%s\n' "$title" \
  "$(printf '%s\n' "$body" | tr -d '\r' | sed 's/%/%25/g' | awk 'BEGIN{ORS="%0A"}{print}')"
