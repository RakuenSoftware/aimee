#!/bin/sh
# Core readiness plus the explicitly enabled, bundled dashboard. Read-only.
set -eu
if [ "${AIMEE_INSTANCE_ROLE:-server}" = kb ]; then
    curl -fsS --max-time 4 http://127.0.0.1:8741/v1/health >/dev/null
    exit
fi
body=$(curl -sS --max-time 4 --unix-socket "${AIMEE_HOME:-/var/lib/aimee}/aimee-http.sock" http://localhost/v1/ready)
printf '%s' "$body" | grep -q '"ready":true'
case "$(printf '%s' "${AIMEE_RUNTIME_WEB_ENABLED:-1}" | tr 'A-Z' 'a-z')" in
    0|false|no|off) exit 0 ;;
esac
if command -v aimee-runtime-web >/dev/null 2>&1; then
    # The local browser has a deployment-owned certificate and redirects an
    # unauthenticated visitor to login. A listener with a failed Vault startup
    # cannot complete this TLS/HTTP check.
    curl -kfsS --max-time 2 -o /dev/null "https://127.0.0.1:${AIMEE_WEBCHAT_PORT:-8443}/"
fi
