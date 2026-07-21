#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

for binary in aimee aimee-server aimee-kb aimee-gateway; do
    path="$repo_root/$binary"
    if [ ! -x "$path" ]; then
        echo "plugin-loader-disabled: missing executable $binary" >&2
        exit 1
    fi
done

for object in \
    "$repo_root/src/build/obj/modules/plugin-loader/plugin.o" \
    "$repo_root/src/build/obj/modules/plugin-loader/plugin_loader.o" \
    "$repo_root/src/build/obj/cmd_plugin.o" \
    "$repo_root/src/build/obj/server/server_plugin.o"; do
    if [ -e "$object" ]; then
        echo "plugin-loader-disabled: optional object exists: $object" >&2
        exit 1
    fi
done

deny='(/v1/plugins|/v1/dashboard/plugins|^/plugins:$|^/plugins/(enable|disable):$|^/dashboard/plugins:$|plugin\.(list|enable|disable)|plugin_loader_discover_all|plugin_registry_(load|save|json)|plugin_manifest_parse|plugin_load_and_register)'
for binary in aimee aimee-server aimee-kb aimee-gateway; do
    if strings "$repo_root/$binary" | grep -Eq "$deny"; then
        echo "plugin-loader-disabled: optional surface leaked into $binary" >&2
        strings "$repo_root/$binary" | grep -E "$deny" >&2 || true
        exit 1
    fi
done

runtime_object="$repo_root/src/build/obj/modules/module-runtime/extension.o"
if [ ! -e "$runtime_object" ]; then
    echo "plugin-loader-disabled: required extension runtime object is missing" >&2
    exit 1
fi
if nm -u "$runtime_object" | grep -Eq 'plugin_loader|plugin_registry|plugin_manifest|dlopen|dlsym'; then
    echo "plugin-loader-disabled: required extension runtime depends on optional loader" >&2
    exit 1
fi

echo "plugin-loader-disabled: ok"
