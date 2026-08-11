# optional-modules-lib.sh — operator control over which OPTIONAL modules run.
#
# Sourced by server-entrypoint.sh and aimee-kb-entrypoint.sh. Not executable on
# its own.
#
# A module is a separate process attached to the daemon's bus, started by
# module-supervisor.sh from a manifest of "<module_id>\t<executable>" lines. The
# shipped manifest is baked at image build time and therefore cannot know what a
# given operator wants turned on, so the operator needs a runtime control.
#
# Required modules are not negotiable and are never touched here — turning off
# memory or routing does not produce a working deployment, it produces a broken
# one that fails later and further away. Only the OPTIONAL set is gated.
#
# Control: AIMEE_MODULE_<ID>, where <ID> is the module id uppercased with '-'
# replaced by '_' (roundtable -> AIMEE_MODULE_ROUNDTABLE, kb-synthesis ->
# AIMEE_MODULE_KB_SYNTHESIS).
#
#   1|true|on|yes   ensure the module runs (append it if the image shipped it off)
#   0|false|off|no  ensure it does not run (drop it if the image shipped it on)
#   unset or empty  keep whatever the image shipped — the default is not changed
#
# This generalises what used to be a hard-coded special case for roundtable. That
# case only ever added a module and only ever for roundtable; the variable name it
# used already matched this convention, so existing AIMEE_MODULE_ROUNDTABLE
# settings keep working unchanged.

# Optional modules this placement can host, from the canonical inventory
# (tests/baselines/modules/canonical-inventory.yaml) filtered by the placements
# declared in src/modules/process-contracts.json.
#
# workflows is optional and server-placed but is NOT listed: it is hosted by
# /usr/local/bin/aimee-wfe rather than the module-runtime multicall binary, and
# is governed by AIMEE_WFE_ENGINE. Gating it here would silently do nothing.
#
# economizer IS listed, and without it the module is unreachable in a container.
# It is optional and default-off, so the exporter writes it no row in
# server.modules; the image installs aimee-module-economizer and nothing ever
# starts it. Every economizer caller fails open, so the symptom is not an error
# anywhere — just a deployment that quietly never reduces a prompt.
optional_modules_for_placement() {
    case "$1" in
        server) echo "governance roundtable benchmarks sandbox runtime-web economizer" ;;
        kb)     echo "kb-synthesis control-web benchmarks" ;;
        *)      echo "" ;;
    esac
}

# roundtable -> AIMEE_MODULE_ROUNDTABLE
_module_env_name() {
    printf 'AIMEE_MODULE_%s' \
        "$(printf '%s' "$1" | tr '[:lower:]' '[:upper:]' | tr '-' '_')"
}

# Read the operator's intent for one module: "on", "off", or "" (unspecified).
_module_intent() {
    _mi_var="$(_module_env_name "$1")"
    # POSIX-safe indirect read; the value may legitimately be empty.
    eval "_mi_val=\${$_mi_var-}"
    case "$(printf '%s' "${_mi_val:-}" | tr '[:upper:]' '[:lower:]')" in
        1|true|on|yes)  printf 'on' ;;
        0|false|off|no) printf 'off' ;;
        *)              printf '' ;;
    esac
    unset _mi_var _mi_val 2>/dev/null || true
}

# apply_optional_modules PLACEMENT MANIFEST WRITABLE_DIR
#
# Echoes the manifest path the supervisor should use: the original when the
# operator asked for nothing, otherwise a rewritten copy in WRITABLE_DIR. Never
# edits the shipped manifest in place — that path is read-only in the image and
# shared by every restart.
#
# This function's STDOUT is its return value, so every diagnostic below is
# redirected to stderr AT THE CALL SITE rather than trusting the caller's log()
# to do it. server-entrypoint.sh's log() printed to stdout, so the enable path --
# the only path that logs and rewrites -- put its message inside the command
# substitution at server-entrypoint.sh:537. MODULE_MANIFEST became the log line
# followed by the real path, module-supervisor.sh could not read that as a file,
# and EVERY module died rather than just the one being toggled. The operator saw
# "fatal: missing module manifest" naming a path that looked like prose.
#
# A caller cannot be relied on for this: aimee-kb-entrypoint.sh captures the same
# way at :232 and defines no log() at all, so a log() added there later would
# reintroduce it. Redirecting here makes the contract hold for any caller.
apply_optional_modules() {
    _om_placement="$1"
    _om_manifest="$2"
    _om_dir="$3"
    _om_changed=0
    _om_out="$_om_dir/${_om_placement}.modules"

    if [ ! -r "$_om_manifest" ]; then
        printf '%s' "$_om_manifest"
        return 0
    fi

    _om_tmp="$_om_out.tmp.$$"
    cp "$_om_manifest" "$_om_tmp" 2>/dev/null || {
        printf '%s' "$_om_manifest"
        return 0
    }

    for _om_id in $(optional_modules_for_placement "$_om_placement"); do
        _om_intent="$(_module_intent "$_om_id")"

        # runtime-web's browser UI has its own long-standing switch. If the
        # operator turned the UI off and said nothing about the module, the
        # module has nothing to serve, so follow the UI switch rather than
        # leaving an idle process attached to the bus.
        if [ "$_om_id" = "runtime-web" ] && [ -z "$_om_intent" ]; then
            case "$(printf '%s' "${AIMEE_RUNTIME_WEB_ENABLED:-}" | tr '[:upper:]' '[:lower:]')" in
                0|false|off|no) _om_intent=off ;;
            esac
        fi

        [ -n "$_om_intent" ] || continue

        _om_present=0
        grep -q "^${_om_id}[[:space:]]" "$_om_tmp" 2>/dev/null && _om_present=1

        if [ "$_om_intent" = on ] && [ "$_om_present" -eq 0 ]; then
            _om_bin="/usr/local/libexec/aimee-modules/aimee-module-${_om_id}"
            if [ -x "$_om_bin" ]; then
                printf '%s\t%s\n' "$_om_id" "$_om_bin" >> "$_om_tmp"
                _om_changed=1
                command -v log >/dev/null 2>&1 && \
                    log "optional module $_om_id enabled by $(_module_env_name "$_om_id")" >&2
            else
                command -v log >/dev/null 2>&1 && \
                    log "warning: $(_module_env_name "$_om_id") is set but $_om_bin is not in this image" >&2
            fi
        elif [ "$_om_intent" = off ] && [ "$_om_present" -eq 1 ]; then
            # grep -v exits 1 when it prints nothing, which happens whenever the
            # module being removed is the only line. Gating the mv on grep's
            # status would silently keep the module the operator just disabled,
            # so key off the output file existing instead.
            : > "$_om_tmp.f"
            grep -v "^${_om_id}[[:space:]]" "$_om_tmp" >> "$_om_tmp.f" 2>/dev/null || true
            mv "$_om_tmp.f" "$_om_tmp"
            _om_changed=1
            command -v log >/dev/null 2>&1 && \
                log "optional module $_om_id disabled by $(_module_env_name "$_om_id")" >&2
        fi
    done

    if [ "$_om_changed" -eq 1 ]; then
        mv "$_om_tmp" "$_om_out" 2>/dev/null || {
            rm -f "$_om_tmp"
            printf '%s' "$_om_manifest"
            return 0
        }
        chown aimee:aimee "$_om_out" 2>/dev/null || true
        printf '%s' "$_om_out"
    else
        rm -f "$_om_tmp"
        printf '%s' "$_om_manifest"
    fi
}
