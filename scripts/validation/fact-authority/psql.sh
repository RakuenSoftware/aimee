#!/bin/bash
# Run a SQL statement against the validation database. Run AS ROOT in the
# container: /root/psql.sh "select 1"
#
# LC_ALL=C on purpose: the container has no generated locales, so psql's perl
# wrapper writes a 15-line locale warning to stderr on every call. Filtering it
# afterwards is not enough — callers read results with `tail -1`, and a stray
# warning line silently becomes the "value". Suppress it at the source.
export PGPASSWORD=aimee-e2e
export LC_ALL=C
export LANG=C
exec psql -h 127.0.0.1 -U aimee -d aimee_shared -tA -q -c "$1" 2>/dev/null
