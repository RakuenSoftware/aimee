# runtime-web-lib.sh — Vault-only webchat migration + launch helpers.
# POSIX sh; sourced by the root server entrypoint.

WEBCHAT_PORT="${AIMEE_WEBCHAT_PORT:-8443}"
WEBCHAT_HOME="${AIMEE_HOME:-/var/lib/aimee}"
WEBCHAT_SPA="${AIMEE_WEBCHAT_SPA:-/usr/local/share/aimee-runtime-web/index.html}"
WEBCHAT_LOGIN_STORE="${WEBCHAT_HOME}/webchat/logins"
WEBCHAT_BOOTSTRAP_REPLACED="${WEBCHAT_HOME}/webchat/bootstrap-replaced"
WEBCHAT_BOOTSTRAP_USER="${WEBCHAT_HOME}/webchat/bootstrap-user"
WEBCHAT_BOOTSTRAP_CREDENTIALS="${WEBCHAT_HOME}/webchat/bootstrap-credentials"
WEBCHAT_LEGACY_TLS_KEY="${WEBCHAT_HOME}/webchat.key"
webchat_pid=""
WEBCHAT_PREPARED=0

webchat_log() { printf '[webchat] %s\n' "$*"; }

webchat_is_enabled() {
    case "$(printf '%s' "${AIMEE_RUNTIME_WEB_ENABLED:-1}" | tr 'A-Z' 'a-z')" in
        0 | false | no | off) return 1 ;;
        *) return 0 ;;
    esac
}

webchat_remove_legacy_file() {
    _rf_path="$1"
    [ -f "$_rf_path" ] || return 0
    if command -v shred >/dev/null 2>&1; then
        shred -u -n 1 -z -- "$_rf_path" 2>/dev/null || rm -f -- "$_rf_path"
    else
        rm -f -- "$_rf_path"
    fi
}

webchat_read_generated_credentials() {
    wc_generated_user=""
    wc_generated_pass=""
    [ -f "$WEBCHAT_BOOTSTRAP_CREDENTIALS" ] || return 1
    [ "$(grep -c '^username=' "$WEBCHAT_BOOTSTRAP_CREDENTIALS" 2>/dev/null || true)" -eq 1 ] || return 1
    [ "$(grep -c '^password=' "$WEBCHAT_BOOTSTRAP_CREDENTIALS" 2>/dev/null || true)" -eq 1 ] || return 1
    [ "$(awk 'NF { n++ } END { print n + 0 }' "$WEBCHAT_BOOTSTRAP_CREDENTIALS")" -eq 2 ] || return 1
    wc_generated_user="$(sed -n 's/^username=//p' "$WEBCHAT_BOOTSTRAP_CREDENTIALS" | head -n 1)"
    wc_generated_pass="$(sed -n 's/^password=//p' "$WEBCHAT_BOOTSTRAP_CREDENTIALS" | head -n 1)"
    case "$wc_generated_user" in
        aimee-[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) : ;;
        *) return 1 ;;
    esac
    case "$wc_generated_pass" in *[!0-9a-f]* | '') return 1 ;; esac
    [ "${#wc_generated_pass}" -eq 64 ]
}

# The helper accepts only a closed record-name allowlist. Secret bytes travel on
# stdin to a short-lived process running as the Vault owner.
webchat_seal_record() {
    _sr_name="$1"
    runuser -u aimee -- aimee-server --webchat-vault-seal "$_sr_name"
}

webchat_clear_legacy_shadow_hashes() {
    _ch_users=""
    if [ -f "$WEBCHAT_LOGIN_STORE" ]; then
        _ch_users="$(cut -d: -f1 "$WEBCHAT_LOGIN_STORE" 2>/dev/null || true)"
    fi
    if [ -n "${wc_generated_user:-}" ]; then
        _ch_users="$_ch_users
$wc_generated_user"
    fi
    _ch_members="$(getent group aimee 2>/dev/null | cut -d: -f4 | tr ',' '\n' || true)"
    _ch_users="$_ch_users
$_ch_members
aimee"
    printf '%s\n' "$_ch_users" | while IFS= read -r _ch_user; do
        [ -n "$_ch_user" ] || continue
        id "$_ch_user" >/dev/null 2>&1 || continue
        # Replace, do not merely prefix, the old verifier. '!' cannot
        # authenticate and contains no recoverable credential material.
        usermod --password '!' "$_ch_user" 2>/dev/null || {
            webchat_log "ERROR: could not erase legacy PAM verifier for '$_ch_user'"
            return 1
        }
    done
}

webchat_migrate_legacy_credentials() {
    if [ -f "$WEBCHAT_BOOTSTRAP_CREDENTIALS" ]; then
        if ! webchat_read_generated_credentials; then
            webchat_log "ERROR: legacy bootstrap credential file is invalid; refusing to delete unknown authentication state"
            return 1
        fi
        if ! printf '%s:%s' "$wc_generated_user" "$wc_generated_pass" | webchat_seal_record legacy_primary; then
            webchat_log "ERROR: could not seal the legacy bootstrap login into Vault"
            return 1
        fi
    fi
    if [ -s "$WEBCHAT_LOGIN_STORE" ]; then
        if ! webchat_seal_record legacy_hashes < "$WEBCHAT_LOGIN_STORE"; then
            webchat_log "ERROR: could not seal the legacy PAM verifier registry into Vault"
            return 1
        fi
    fi
    # Eliminate every old live verifier before removing its migration source.
    webchat_clear_legacy_shadow_hashes || return 1
    if [ -f "$WEBCHAT_BOOTSTRAP_CREDENTIALS" ]; then
        webchat_remove_legacy_file "$WEBCHAT_BOOTSTRAP_CREDENTIALS" || return 1
        webchat_log "sealed and removed the legacy plaintext bootstrap login"
    fi
    if [ -f "$WEBCHAT_LOGIN_STORE" ]; then
        webchat_remove_legacy_file "$WEBCHAT_LOGIN_STORE" || return 1
        webchat_log "sealed and removed the legacy PAM verifier registry"
    fi
    if [ -f "$WEBCHAT_LEGACY_TLS_KEY" ]; then
        if ! webchat_seal_record tls_key < "$WEBCHAT_LEGACY_TLS_KEY"; then
            webchat_log "ERROR: could not seal the legacy TLS private key into Vault"
            return 1
        fi
        webchat_remove_legacy_file "$WEBCHAT_LEGACY_TLS_KEY" || return 1
        webchat_log "sealed and removed the legacy TLS private key"
    fi
    wc_generated_user="" wc_generated_pass="" _ch_users="" _ch_members=""
}

webchat_prepare() {
    # Migration is unconditional. Disabling the browser surface must not leave
    # credentials from an older image sitting on the data volume or in shadow.
    webchat_migrate_legacy_credentials || return 1
    if ! webchat_is_enabled; then
        webchat_log "AIMEE_RUNTIME_WEB_ENABLED=0; browser UI disabled by operator"
        WEBCHAT_PREPARED=1
        return 0
    fi
    if ! command -v aimee-runtime-web >/dev/null 2>&1; then
        webchat_log "aimee-runtime-web not present (image built without WITH_RUNTIME_WEB); skipping browser UI"
        WEBCHAT_PREPARED=1
        return 0
    fi
    if ! runuser -u aimee -- aimee-server --webchat-vault-check; then
        webchat_log "ERROR: browser UI requires a complete first-boot login sealed in Vault"
        return 1
    fi
    # server.token was a persistent shared bearer. UDS peer credentials replaced
    # it; erase any legacy copy before starting services.
    webchat_remove_legacy_file "$WEBCHAT_HOME/server.token" || return 1
    WEBCHAT_PREPARED=1
}

webchat_start() {
    [ "$WEBCHAT_PREPARED" -eq 1 ] || webchat_prepare
    webchat_is_enabled || return 0
    command -v aimee-runtime-web >/dev/null 2>&1 || return 0
    if [ -n "${AIMEE_WEBCHAT_TLS_KEY:-}" ]; then
        webchat_log "ERROR: TLS private-key files are forbidden; import first-boot key material into Vault"
        return 1
    fi
    _wc_cert=""
    if [ -n "${AIMEE_WEBCHAT_TLS_CERT:-}" ]; then
        _wc_cert="--cert $AIMEE_WEBCHAT_TLS_CERT"
        webchat_log "using operator TLS certificate $AIMEE_WEBCHAT_TLS_CERT with its Vault-held key"
    fi
    webchat_log "starting Vault-authenticated aimee-runtime-web on :$WEBCHAT_PORT"
    # shellcheck disable=SC2086
    HOME=/root AIMEE_HOME="$WEBCHAT_HOME" aimee-runtime-web \
        --port "$WEBCHAT_PORT" \
        --socket "$WEBCHAT_HOME/aimee-server.sock" \
        --db "$WEBCHAT_HOME/webchat.db" \
        --spa "$WEBCHAT_SPA" \
        $_wc_cert &
    webchat_pid=$!
}

webchat_stop() {
    [ -n "$webchat_pid" ] && kill "$webchat_pid" 2>/dev/null || true
}
