#!/bin/sh
# Install the module runtime under every name its grants pin.
#
# The Go module runtime is a MULTICALL binary: one executable that decides which
# module it is from argv[0]. A deployment installs it once per module name, and
# every grant pins the absolute path of the name it expects.
#
# This is not cosmetic. parse_grant_file() realpath()s executable=, and
# bus_runtime_policy_load_dir REJECTS THE WHOLE DIRECTORY if any single entry is
# unresolvable -- so one missing binary is not a degraded module, it is a daemon
# that refuses to start:
#
#   event-bus: refusing to start: the module grant .../governance.grant is not
#   usable. Check its executable= path exists and is absolute.
#   error: server module bus failed to start
#
# aimee-wfe is a separate program rather than a multicall name, so it is skipped
# here and its grant removed: a grant naming a binary this rig does not install
# would take the whole directory down with it.
set -eu

SRC=/usr/local/libexec/aimee-modules/aimee-module-aimee
DEST=/usr/local/libexec/aimee-modules
GRANTS=/opt/aimee/module-grants/server

[ -x "$SRC" ] || { echo "install-modules: $SRC is missing"; exit 1; }

made=0
for grant in "$GRANTS"/*.grant; do
   exe=$(sed -n 's/^executable=//p' "$grant" | head -1)
   [ -n "$exe" ] || continue
   name=$(basename "$exe")
   case "$name" in
      aimee-wfe)
         echo "skip  $name (a separate program, not a multicall name)"
         rm -f "$grant"
         continue
         ;;
   esac
   if [ ! -x "$exe" ]; then
      mkdir -p "$(dirname "$exe")"
      cp "$SRC" "$exe"
      chmod 755 "$exe"
      made=$((made + 1))
   fi
done

echo "install-modules: $made module name(s) installed"
ls "$DEST" | wc -l | sed 's/^/install-modules: names present: /'
