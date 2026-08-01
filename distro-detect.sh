#!/usr/bin/env bash
# distro-detect.sh — detect the host package manager and map abstract
# dependency names to distro-specific package names.
# Source this file; it exports AIMEE_SUDO, PKG_MGR, PKG_INSTALL, and PKG_UPDATE.

# Privilege prefix for package/service operations. Empty when already root — a
# stock container or cloud base image typically runs as root and ships NO sudo, so
# hardcoding `sudo` made install-deps.sh die with "sudo: command not found" on the
# most common first-run environment. Non-root without sudo is a hard error: the
# caller cannot install packages at all, and failing here beats failing later.
detect_sudo() {
    if [ "$(id -u)" -eq 0 ]; then
        AIMEE_SUDO=""
    elif command -v sudo &>/dev/null; then
        AIMEE_SUDO="sudo"
    else
        echo "Error: this script installs system packages and needs root." >&2
        echo "Run it as root, or install sudo and re-run as a user with sudo rights." >&2
        exit 1
    fi
    export AIMEE_SUDO
}

detect_pkg_manager() {
    detect_sudo
    if command -v dnf &>/dev/null; then
        PKG_MGR="dnf"
        PKG_INSTALL="$AIMEE_SUDO dnf install -y"
        PKG_UPDATE="$AIMEE_SUDO dnf makecache"
    elif command -v yum &>/dev/null; then
        PKG_MGR="yum"
        PKG_INSTALL="$AIMEE_SUDO yum install -y"
        PKG_UPDATE="$AIMEE_SUDO yum makecache"
    elif command -v apt-get &>/dev/null; then
        PKG_MGR="apt"
        PKG_INSTALL="$AIMEE_SUDO apt-get install -y"
        PKG_UPDATE="$AIMEE_SUDO apt-get update -qq"
    elif command -v pacman &>/dev/null; then
        PKG_MGR="pacman"
        PKG_INSTALL="$AIMEE_SUDO pacman -S --noconfirm"
        PKG_UPDATE="$AIMEE_SUDO pacman -Sy"
    elif command -v brew &>/dev/null; then
        PKG_MGR="brew"
        PKG_INSTALL="brew install"
        PKG_UPDATE="brew update"
    else
        PKG_MGR="unknown"
        PKG_INSTALL=""
        PKG_UPDATE=""
    fi
    export PKG_MGR PKG_INSTALL PKG_UPDATE
}

# Map abstract dependency names to distro-specific package names.
pkg_name() {
    local dep="$1"
    case "$PKG_MGR" in
        dnf|yum)
            case "$dep" in
                libsqlite3-dev)      echo "sqlite-devel" ;;
                libpq-dev)           echo "libpq-devel" ;;
                libzstd-dev)         echo "libzstd-devel" ;;
                zlib1g-dev)          echo "zlib-devel" ;;
                libcurl4-openssl-dev) echo "libcurl-devel" ;;
                libpam0g-dev)        echo "pam-devel" ;;
                libsecret-1-dev)     echo "libsecret-devel" ;;
                universal-ctags)     echo "ctags" ;;
                sqlite3)             echo "sqlite" ;;
                python3-yaml)        echo "python3-pyyaml" ;;
                clang-format-19)     echo "clang-tools-extra" ;;
                build-essential)     echo "gcc make" ;;
                pkg-config)          echo "pkgconfig" ;;
                clang-tidy)          echo "clang-tools-extra" ;;
                postgresql-server)   echo "postgresql-server postgresql-contrib" ;;
                postgresql-pgvector) local _pv; _pv=$(pg_config --version 2>/dev/null | grep -oE '[0-9]+' | head -1)
                                     echo "pgvector_${_pv:-17}" ;;
                *)                   echo "$dep" ;;
            esac ;;
        apt)
            case "$dep" in
                # apt's package names match the abstract dep names in REQUIRED_DEPS
                # (build-essential / libsqlite3-dev / libpq-dev / etc), so the
                # default identity mapping covers most of them. Postgres needs
                # an explicit map to pull in the server package, not just libpq.
                postgresql-server)   echo "postgresql postgresql-contrib" ;;
                postgresql-pgvector) local _pv; _pv=$(pg_config --version 2>/dev/null | grep -oE '[0-9]+' | head -1)
                                     echo "postgresql-${_pv:-17}-pgvector" ;;
                *)                   echo "$dep" ;;
            esac ;;
        pacman)
            case "$dep" in
                libsqlite3-dev)      echo "sqlite" ;;
                libpq-dev)           echo "postgresql-libs" ;;
                libzstd-dev)         echo "zstd" ;;
                zlib1g-dev)          echo "zlib" ;;
                libcurl4-openssl-dev) echo "curl" ;;
                libpam0g-dev)        echo "pam" ;;
                libsecret-1-dev)     echo "libsecret" ;;
                universal-ctags)     echo "ctags" ;;
                sqlite3)             echo "sqlite" ;;
                python3-yaml)        echo "python-yaml" ;;
                clang-format-19)     echo "clang" ;;
                build-essential)     echo "base-devel" ;;
                pkg-config)          echo "pkgconf" ;;
                postgresql-server)   echo "postgresql" ;;
                postgresql-pgvector) echo "pgvector" ;;
                *)                   echo "$dep" ;;
            esac ;;
        brew)
            case "$dep" in
                libsqlite3-dev)      echo "sqlite" ;;
                libpq-dev)           echo "libpq" ;;
                libzstd-dev)         echo "zstd" ;;
                zlib1g-dev)          echo "zlib" ;;
                libcurl4-openssl-dev) echo "curl" ;;
                libpam0g-dev)        echo "" ;;  # PAM is built-in on macOS
                universal-ctags)     echo "universal-ctags" ;;
                sqlite3)             echo "sqlite" ;;
                python3-yaml)        echo "pyyaml" ;;  # Homebrew formula (or pip3 install pyyaml)
                clang-format-19)     echo "clang-format" ;;
                build-essential)     echo "" ;;  # Xcode CLT covers this
                clang-tidy)          echo "llvm" ;;
                postgresql-server)   echo "postgresql" ;;
                postgresql-pgvector) echo "pgvector" ;;
                *)                   echo "$dep" ;;
            esac ;;
        *)
            echo "$dep" ;;
    esac
}

# Return a human-readable install hint for a package name.
pkg_install_hint() {
    local dep="$1"
    local pkg
    pkg=$(pkg_name "$dep")
    [ -z "$pkg" ] && return
    case "$PKG_MGR" in
        dnf)    echo "dnf install $pkg" ;;
        yum)    echo "yum install $pkg" ;;
        apt)    echo "apt-get install $pkg" ;;
        pacman) echo "pacman -S $pkg" ;;
        brew)   echo "brew install $pkg" ;;
        *)      echo "install $pkg" ;;
    esac
}

# Abstract dependency names — pkg_name() maps them to distro packages.
# Shared by install-deps.sh (installs them, needs sudo) and install.sh
# (checks they are present before building, no sudo).
REQUIRED_DEPS=(
    build-essential
    pkg-config
    libsqlite3-dev
    libpq-dev
    libzstd-dev
    zlib1g-dev
    libcurl4-openssl-dev
    libpam0g-dev
    universal-ctags
    sqlite3
    ripgrep
    python3-yaml
    clang-format-19
    postgresql-server
    postgresql-pgvector
)

# Check whether an abstract dep is satisfied on the current system.
dep_present() {
    case "$1" in
        build-essential)      command -v gcc &>/dev/null ;;
        pkg-config)           command -v pkg-config &>/dev/null ;;
        libsqlite3-dev)       pkg-config --exists sqlite3 2>/dev/null ;;
        libpq-dev)            pkg-config --exists libpq 2>/dev/null ;;
        libzstd-dev)          pkg-config --exists libzstd 2>/dev/null ;;
        zlib1g-dev)           pkg-config --exists zlib 2>/dev/null ;;
        libcurl4-openssl-dev) pkg-config --exists libcurl 2>/dev/null ;;
        # No pkg-config .pc for PAM; probe the header the way cgo will, which
        # also resolves the macOS SDK location. If no C compiler is present yet
        # the probe fails and we (harmlessly) queue libpam0g-dev for install.
        libpam0g-dev)         echo '#include <security/pam_appl.h>' \
                                  | "${CC:-cc}" -fsyntax-only -xc - &>/dev/null ;;
        universal-ctags)      command -v ctags &>/dev/null ;;
        sqlite3)              command -v sqlite3 &>/dev/null ;;
        ripgrep)              command -v rg &>/dev/null ;;
        # PyYAML backs the docs-gen / api-conformance checks (make lint,
        # `aimee git verify`); the shipped binaries and runtime sidecars do not
        # need it, but the build/verify workflow does.
        python3-yaml)         python3 -c 'import yaml' &>/dev/null ;;
        # clang-format backs the formatting lint (make lint, `aimee git
        # verify`). The Makefile pins clang-format-19; accept any clang-format
        # on PATH so the dep is treated as satisfied where the binary is named
        # plainly (the Makefile honors CLANG_FORMAT=... for non-default names).
        clang-format-19)      command -v clang-format-19 &>/dev/null || \
                              command -v clang-format &>/dev/null ;;
        # Server presence: psql + the postgres binary. Detecting on the path
        # alone misses installs where the binary lives in /usr/lib/postgresql/*
        # without being in PATH, so probe both common locations.
        postgresql-server)    command -v psql &>/dev/null && \
                              { command -v postgres &>/dev/null || \
                                ls /usr/lib/postgresql/*/bin/postgres &>/dev/null; } ;;
        # pgvector package installs a .control file under the PG extension dir.
        postgresql-pgvector)  ls /usr/share/postgresql/*/extension/vector.control \
                                  2>/dev/null | grep -q . ;;
        *)                    return 1 ;;
    esac
}
