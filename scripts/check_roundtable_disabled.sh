#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

for binary in aimee aimee-server aimee-kb aimee-gateway; do
    path="$repo_root/$binary"
    if [ ! -x "$path" ]; then
        echo "roundtable-disabled: missing executable $binary" >&2
        exit 1
    fi
done

for object in \
    "$repo_root"/src/build/obj/modules/roundtable/*.o \
    "$repo_root/src/build/obj/server/server_compute_roundtable.o" \
    "$repo_root/src/build/obj/server/server_pipeline.o" \
    "$repo_root/src/build/obj/server/server_pipeline_merge.o" \
    "$repo_root/src/build/obj/modules/workflows/wfe_live_panel.o" \
    "$repo_root/src/build/obj/modules/workflows/wfe_panel_roundtable.o" \
    "$repo_root/src/build/obj/modules/workflows/wfe_replay_worktree.o" \
    "$repo_root/src/build/obj/modules/workflows/wfe_roundtable.o" \
    "$repo_root/src/build/obj/db1/roundtable_pipeline.o"; do
    if [ -e "$object" ]; then
        echo "roundtable-disabled: optional object exists: $object" >&2
        exit 1
    fi
done

deny='(/v1/delegate/(aggregate|roundtable)|/v1/roundtables|delegate\.(aggregate|roundtable)|ensemble_review|(^|[^[:alnum:]_])pipeline_(start|advance|status|list|gate|resume|cancel)([^[:alnum:]_]|$)|roundtable_provider_configure)'
for binary in aimee aimee-server aimee-kb aimee-gateway; do
    if strings "$repo_root/$binary" | grep -Eq "$deny"; then
        echo "roundtable-disabled: optional surface leaked into $binary" >&2
        strings "$repo_root/$binary" | grep -E "$deny" >&2 || true
        exit 1
    fi
done

for object in \
    "$repo_root/src/build/obj/modules/delegates/panel_provider.o" \
    "$repo_root/src/build/obj/modules/delegates/panel_roster.o"; do
    if [ ! -e "$object" ]; then
        echo "roundtable-disabled: required panel boundary object is missing: $object" >&2
        exit 1
    fi
    if nm -u "$object" | grep -Eq 'roundtable_|delegate_(ensemble|roundtable)_'; then
        echo "roundtable-disabled: required panel boundary depends on optional implementation" >&2
        nm -u "$object" >&2
        exit 1
    fi
done

echo "roundtable-disabled: ok"
