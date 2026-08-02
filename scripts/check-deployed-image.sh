#!/usr/bin/env bash
# check-deployed-image.sh: is this host running the image it is supposed to be?
#
# A deployment drifts off its published channel in two ways, and both are silent:
#
#   1. A local pin. Setting AIMEE_SERVER_IMAGE to a locally built tag takes the
#      host off the registry entirely. `docker compose pull` then fails with
#      "pull access denied for aimee-server, repository does not exist" and the
#      host keeps serving whatever it already had. Four such pins appeared on one
#      box in a single day; it sat two hours on a build that predated the fix it
#      was supposed to be running.
#
#   2. A recreate without a pull. `docker compose up -d` reuses the cached image,
#      so a container restarted after a publish comes back on the OLD build. The
#      tag still reads :testing and everything looks correct.
#
# Neither shows up in `docker ps`, which reports the tag rather than the digest.
# This compares what is RUNNING against what the registry currently publishes.
#
# Usage: check-deployed-image.sh [service...]      (default: aimee-server aimee-kb)
#   AIMEE_IMAGE_TAG   channel to check against (default: testing)
#   AIMEE_IMAGE_REPO  registry prefix           (default: ghcr.io/rakuensoftware)
#
# Exit: 0 all services current, 1 drift found, 2 could not determine.
set -uo pipefail

TAG="${AIMEE_IMAGE_TAG:-testing}"
REPO="${AIMEE_IMAGE_REPO:-ghcr.io/rakuensoftware}"
SERVICES=("${@:-}")
[ -z "${SERVICES[0]:-}" ] && SERVICES=(aimee-server aimee-kb)

command -v docker >/dev/null || { echo "check-deployed-image: docker not found"; exit 2; }

rc=0
for svc in "${SERVICES[@]}"; do
   container=$(docker ps --filter "name=${svc}" --format '{{.Names}}' | head -1)
   if [ -z "$container" ]; then
      echo "check-deployed-image: ${svc}: not running — skipped"
      continue
   fi

   image=$(docker inspect "$container" --format '{{.Config.Image}}' 2>/dev/null)
   want="${REPO}/${svc}:${TAG}"

   # A pin to anything that is not the published repo is drift by definition:
   # the host can no longer receive a published build at all.
   case "$image" in
      "${REPO}/"*) ;;
      *)
         echo "check-deployed-image: ${svc}: PINNED to '${image}', not '${want}'"
         echo "  this host cannot pull published builds while pinned; remove"
         echo "  AIMEE_SERVER_IMAGE (or equivalent) from the compose .env"
         rc=1
         continue
         ;;
   esac

   running=$(docker inspect "$container" --format '{{.Image}}' 2>/dev/null)
   # RepoDigests is what the registry served; empty means a local build.
   published=$(docker image inspect "$want" --format '{{index .RepoDigests 0}}' 2>/dev/null || true)
   if [ -z "$published" ]; then
      echo "check-deployed-image: ${svc}: local image '${want}' has no registry digest"
      echo "  it was built here rather than pulled; run 'docker compose pull ${svc}'"
      rc=1
      continue
   fi

   local_id=$(docker image inspect "$want" --format '{{.Id}}' 2>/dev/null || true)
   if [ "$running" != "$local_id" ]; then
      echo "check-deployed-image: ${svc}: STALE — running an older image than the"
      echo "  local '${want}'. A container was recreated without a pull, or the tag"
      echo "  moved after it started. Run 'docker compose pull ${svc} && up -d ${svc}'."
      rc=1
      continue
   fi

   built=$(docker image inspect "$want" --format '{{.Created}}' 2>/dev/null)
   echo "check-deployed-image: ${svc}: ok (${want}, built ${built})"
done

exit $rc
