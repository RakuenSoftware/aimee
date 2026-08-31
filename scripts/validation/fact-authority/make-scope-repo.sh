#!/bin/bash
# Create the git repository that gives aimee-server an ACTIVE SCOPE.
#
# ingress_preinject_resolve_active_scope() derives the project identity from the
# server's cwd via workspace_repo_identity(). With no repository there is no
# scope, and ingress_preinject_build() returns NULL before assembling anything --
# deliberately, so agent ingress cannot silently broaden to global recall. The
# visible effect is that no envelope is built and the memory module's RERANK
# confidence tier is never requested, which looks like the module being idle.
# Run AS ROOT in the container.
set -u
[ -d /root/proj/.git ] && { echo "scope repo already present"; exit 0; }
command -v git >/dev/null 2>&1 || {
  export DEBIAN_FRONTEND=noninteractive
  apt-get install -y -qq git >/dev/null 2>&1
}
command -v git >/dev/null 2>&1 || { echo "git unavailable; cannot create scope repo" >&2; exit 1; }

mkdir -p /root/proj
cd /root/proj || exit 1
git init -q
git config user.email e2e@example.com
git config user.name  e2e
echo "validation project for the fact-authority e2e" > README.md
git add -A
git commit -qm "e2e scope"
echo "scope repo created at /root/proj"
