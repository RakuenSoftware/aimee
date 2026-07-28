#!/usr/bin/env bash
set -euo pipefail

# aimee dependency installer (privileged).
#
# Installs the system packages aimee needs to build and run, and bootstraps the
# PostgreSQL database aimee-kb connects to. These steps need root (sudo) for the
# package manager and for postgres role/extension setup, so they live here,
# separate from install.sh — which builds and configures aimee entirely in the
# user's home directory and needs no sudo.
#
# Run this once (it is idempotent), then run ./install.sh:
#   ./install-deps.sh      # sudo: packages + postgres
#   ./install.sh           # no sudo: build, install, configure
#
# For a REMOTE aimee-kb (no local sidecar), skip the Postgres bootstrap:
#   AIMEE_KB_MODE=remote ./install-deps.sh   # packages only, no local DB
# and choose "remote" in install.sh.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# shellcheck source=distro-detect.sh
source "$SCRIPT_DIR/distro-detect.sh"

# Run a command as the postgres superuser. `sudo -n -u postgres` only works when
# sudo exists; as root (containers, cloud base images) there is no sudo, so use
# runuser/su instead. Non-root without sudo already exited in detect_sudo.
as_postgres() {
    if [ "$(id -u)" -eq 0 ]; then
        if command -v runuser &>/dev/null; then
            runuser -u postgres -- "$@"
        else
            su -s /bin/sh postgres -c "$(printf '%q ' "$@")"
        fi
    else
        sudo -n -u postgres "$@"
    fi
}

detect_pkg_manager

# Colors (if terminal supports them)
if [ -t 1 ]; then
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    RESET='\033[0m'
else
    GREEN='' YELLOW='' RESET=''
fi
info() { echo -e "${GREEN}>${RESET} $*"; }
warn() { echo -e "${YELLOW}!${RESET} $*"; }

# --- System packages ---

missing_deps=()
missing_pkgs=()
for dep in "${REQUIRED_DEPS[@]}"; do
    if ! dep_present "$dep"; then
        missing_deps+=("$dep")
        # pkg_name may emit multiple tokens (e.g. "gcc make").
        for pkg in $(pkg_name "$dep"); do
            [ -n "$pkg" ] && missing_pkgs+=("$pkg")
        done
    fi
done

if [ "${#missing_deps[@]}" -gt 0 ]; then
    case "$PKG_MGR" in
        apt|dnf|yum)
            info "Installing missing packages: ${missing_pkgs[*]}"
            if [ "$PKG_MGR" = "apt" ]; then
                $AIMEE_SUDO apt-get update -qq
            fi
            # shellcheck disable=SC2086
            $PKG_INSTALL "${missing_pkgs[@]}"
            ;;
        *)
            echo "Error: missing required dependencies: ${missing_deps[*]}"
            for dep in "${missing_deps[@]}"; do
                hint=$(pkg_install_hint "$dep")
                [ -n "$hint" ] && echo "  - $dep: $hint"
            done
            exit 1
            ;;
    esac
else
    info "All system packages already present."
fi

# Re-verify critical libraries after the install pass.
if ! command -v gcc &>/dev/null; then
    echo "Error: gcc still not found after install."
    exit 1
fi
if ! pkg-config --exists sqlite3 2>/dev/null; then
    echo "Error: libsqlite3-dev still not detected after install."
    exit 1
fi
if ! pkg-config --exists libpq 2>/dev/null; then
    echo "Error: libpq-dev still not detected after install."
    exit 1
fi
if ! pkg-config --exists libzstd 2>/dev/null; then
    echo "Error: libzstd-dev still not detected after install."
    exit 1
fi
if ! pkg-config --exists libcurl 2>/dev/null; then
    echo "Error: libcurl development headers still not detected after install."
    exit 1
fi
if ! echo '#include <security/pam_appl.h>' | "${CC:-cc}" -fsyntax-only -xc - &>/dev/null; then
    echo "Error: PAM development headers (security/pam_appl.h) still not detected after install."
    echo "Install the PAM dev package (Debian/Ubuntu: libpam0g-dev; Fedora: pam-devel)."
    exit 1
fi

if ! sqlite3 :memory: "PRAGMA compile_options" 2>/dev/null | grep -q ENABLE_FTS5; then
    echo "Error: system SQLite does not have FTS5 enabled."
    echo "Install a SQLite build with FTS5 support (default on Debian 13+)."
    exit 1
fi

# --- PostgreSQL bootstrap ---

# Bring PostgreSQL up: start the system service if it isn't running, then
# create the aimee_shared database (with pg_trgm) so aimee-kb's auto-bootstrap
# in kb_main has something to connect to. Idempotent — safe to run on every
# install. Runs after the package install pass so postgres is on the system.
bootstrap_postgres() {
    if [ "${AIMEE_KB_MODE:-local}" = "remote" ]; then
        info "postgres: AIMEE_KB_MODE=remote; skipping local database setup (using a remote aimee-kb)"
        return
    fi
    if ! command -v psql >/dev/null 2>&1; then
        warn "postgres: psql not found; skipping database setup"
        return
    fi

    if command -v systemctl >/dev/null 2>&1; then
        if ! systemctl is-active --quiet postgresql 2>/dev/null; then
            info "postgres: starting postgresql service"
            $AIMEE_SUDO systemctl enable --now postgresql 2>/dev/null || \
                warn "postgres: failed to start postgresql service via systemctl"
        fi
    elif command -v brew >/dev/null 2>&1 && brew services list 2>/dev/null | grep -q postgresql; then
        if ! brew services list | awk '$1 ~ /^postgresql/ && $2 == "started" {found=1} END {exit !found}'; then
            info "postgres: starting postgresql via brew services"
            brew services start postgresql 2>/dev/null || \
                warn "postgres: failed to start postgresql via brew services"
        fi
    fi

    # Wait a moment for the socket to come up before probing.
    for _ in 1 2 3 4 5; do
        psql -lqt 2>/dev/null | grep -qw aimee_shared && break
        psql -lqt 2>/dev/null >/dev/null 2>&1 && break
        sleep 1
    done

    # Create database if missing. Try the user's own role first; on systems
    # where postgres-as-root is the only superuser path (apt-installed pg on
    # Debian/Ubuntu) fall back to running as the postgres user.
    if ! psql -lqt 2>/dev/null | cut -d'|' -f1 | tr -d ' ' | grep -qx aimee_shared; then
        info "postgres: creating aimee_shared database"
        if ! createdb aimee_shared 2>/dev/null; then
            local user
            user="${USER:-$(id -un)}"
            as_postgres createuser --createdb "$user" 2>/dev/null || true
            as_postgres createdb -O "$user" aimee_shared 2>/dev/null || \
                warn "postgres: createdb aimee_shared failed; create it manually"
        fi
    fi

    # pg_trgm is required for the canonical index's trigram lookups. Try the
    # user role first; fall back to sudo for the same reason as createdb.
    if ! psql -d aimee_shared -tAc "SELECT 1 FROM pg_extension WHERE extname='pg_trgm'" 2>/dev/null | grep -q 1; then
        info "postgres: enabling pg_trgm extension on aimee_shared"
        psql -d aimee_shared -v ON_ERROR_STOP=1 \
             -c 'CREATE EXTENSION IF NOT EXISTS pg_trgm;' 2>/dev/null || \
            as_postgres psql -d aimee_shared -v ON_ERROR_STOP=1 \
                 -c 'CREATE EXTENSION IF NOT EXISTS pg_trgm;' 2>/dev/null || \
            warn "postgres: failed to enable pg_trgm; install superuser must run \
'CREATE EXTENSION pg_trgm;' on aimee_shared"
    fi

    if ! psql -d aimee_shared -tAc "SELECT 1 FROM pg_extension WHERE extname='vector'" 2>/dev/null | grep -q 1; then
        info "postgres: enabling vector extension on aimee_shared"
        psql -d aimee_shared -v ON_ERROR_STOP=1 \
             -c 'CREATE EXTENSION IF NOT EXISTS vector;' 2>/dev/null || \
            as_postgres psql -d aimee_shared -v ON_ERROR_STOP=1 \
                 -c 'CREATE EXTENSION IF NOT EXISTS vector;' 2>/dev/null || \
            warn "postgres: failed to enable vector; install superuser must run \
'CREATE EXTENSION vector;' on aimee_shared"
    fi
}

# Postgres database setup. The aimee-kb daemon's startup auto-bootstrap
# (kb_bootstrap_db2_resolve) will also retry these steps the first time it's
# launched, but doing them at install time gives users a clean "scan works
# right after install.sh" experience instead of a delayed failure.
bootstrap_postgres

info "Dependencies ready. Next: ./install.sh"
