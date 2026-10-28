#!/bin/sh
# ci-annotate-log-tail.sh LOG TITLE [LINES]
#
# Emit the last LINES (default 60) lines of a failed CI step's output as one
# GitHub Actions error annotation. Annotations show on the check run itself,
# so a failure is readable without opening the raw job log -- including one
# that happens before any test could record itself (build, template setup,
# a killed runner process).
set -eu
log=$1
title=$2
lines=${3:-60}
if [ ! -s "$log" ]; then
  echo "::error title=$title::the step wrote no output"
  exit 0
fi
# Workflow commands treat %, CR and LF specially in the message; ',' and ':'
# only matter in properties, so the title is kept to plain words.
printf '::error title=%s::%s\n' "$title" \
  "$(tail -n "$lines" "$log" | tr -d '\r' | sed 's/%/%25/g' | awk 'BEGIN{ORS="%0A"}{print}')"
