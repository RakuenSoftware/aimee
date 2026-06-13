package main

// vault.go: WP-C.2c — webchat wiring for the per-user (`webuser:`) credential
// vault. The browser holds nothing: the user re-presents their login password to
// /api/vault/unlock, which the backend forwards to aimee-server with the
// server.token bearer + an X-Aimee-Webuser assertion. aimee-server derives the
// vault KEK (scrypt) and caches it per webuser. All vault calls go over the
// token-bearing /v1 path (v1RequestWebuser) — NEVER the OpenAI proxy (proxyV1),
// which strips Authorization.

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
)

// serverToken reads the shared server.token secret (0600, in the aimee config
// dir) — the bearer aimee-server trusts for X-Aimee-Webuser assertions. Empty if
// absent.
func (s *server) serverToken() string {
	raw, err := os.ReadFile(filepath.Join(filepath.Dir(s.cfg.socketPath), "server.token"))
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(raw))
}

// v1RequestWebuser is v1Request plus the webchat trust headers: the server.token
// bearer and X-Aimee-Webuser, so aimee-server resolves the caller's webuser:
// vault. Returns an error if the server.token or username is missing (fail-
// closed: no assertion is sent without the trust secret).
func (s *server) v1RequestWebuser(ctx context.Context, username, method, path string, body []byte) (int, []byte, error) {
	token := s.serverToken()
	if token == "" || username == "" {
		return http.StatusUnauthorized, nil, errNoVaultTrust
	}
	sock := s.aimeeHTTPSockPath()
	client := &http.Client{
		Timeout: socketCallTimeout,
		Transport: &http.Transport{
			DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
				return (&net.Dialer{}).DialContext(ctx, "unix", sock)
			},
		},
	}
	var rdr io.Reader
	if body != nil {
		rdr = bytes.NewReader(body)
	}
	req, err := http.NewRequestWithContext(ctx, method, "http://aimee"+path, rdr)
	if err != nil {
		return 0, nil, err
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("X-Aimee-Webuser", username)
	resp, err := client.Do(req)
	if err != nil {
		return 0, nil, err
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	return resp.StatusCode, data, err
}

var errNoVaultTrust = &vaultErr{"vault trust unavailable (server.token or session missing)"}

type vaultErr struct{ msg string }

func (e *vaultErr) Error() string { return e.msg }

// pass an aimee-server /v1 response through to the browser, mapping transport
// failures to a stable JSON error.
func (s *server) vaultRelay(w http.ResponseWriter, st int, data []byte, err error) {
	w.Header().Set("Content-Type", "application/json")
	if err != nil {
		writeJSONError(w, http.StatusServiceUnavailable, "vault: aimee-server unavailable")
		return
	}
	if st != http.StatusOK {
		// Forward aimee-server's own error envelope (locked / unattested / etc.).
		if len(data) > 0 {
			w.WriteHeader(st)
			w.Write(data)
			return
		}
		writeJSONError(w, st, "vault: request failed")
		return
	}
	if len(data) > 0 {
		w.Write(data)
		return
	}
	w.Write([]byte(`{"status":"ok"}`))
}

// POST /api/vault/unlock {password} — derive + cache the user's vault KEK.
func (s *server) handleVaultUnlock(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req struct {
		Password string `json:"password"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Password == "" {
		writeJSONError(w, http.StatusBadRequest, "password required")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	body, _ := json.Marshal(map[string]string{"password": req.Password})
	st, data, err := s.v1RequestWebuser(ctx, currentUser(r), http.MethodPost, "/v1/vault/unlock", body)
	s.vaultRelay(w, st, data, err)
}

// POST /api/vault/password {old_password,new_password} — re-key on password change.
func (s *server) handleVaultPassword(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req struct {
		OldPassword string `json:"old_password"`
		NewPassword string `json:"new_password"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.OldPassword == "" || req.NewPassword == "" {
		writeJSONError(w, http.StatusBadRequest, "old_password and new_password required")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	body, _ := json.Marshal(map[string]string{"old_password": req.OldPassword, "new_password": req.NewPassword})
	st, data, err := s.v1RequestWebuser(ctx, currentUser(r), http.MethodPost, "/v1/vault/rekey", body)
	s.vaultRelay(w, st, data, err)
}

// /api/vault/credentials — GET list (names only), POST set {agent,cred,secret},
// DELETE remove {agent,cred}.
func (s *server) handleVaultCredentials(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()
	user := currentUser(r)
	switch r.Method {
	case http.MethodGet:
		st, data, err := s.v1RequestWebuser(ctx, user, http.MethodPost, "/v1/vault/list", []byte(`{}`))
		s.vaultRelay(w, st, data, err)
	case http.MethodPost:
		var req struct {
			Agent  string `json:"agent"`
			Cred   string `json:"cred"`
			Secret string `json:"secret"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Agent == "" || req.Cred == "" || req.Secret == "" {
			writeJSONError(w, http.StatusBadRequest, "agent, cred, secret required")
			return
		}
		body, _ := json.Marshal(map[string]string{"agent": req.Agent, "cred": req.Cred, "secret": req.Secret})
		st, data, err := s.v1RequestWebuser(ctx, user, http.MethodPost, "/v1/vault/set", body)
		s.vaultRelay(w, st, data, err)
	case http.MethodDelete:
		var req struct {
			Agent string `json:"agent"`
			Cred  string `json:"cred"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Agent == "" || req.Cred == "" {
			writeJSONError(w, http.StatusBadRequest, "agent and cred required")
			return
		}
		body, _ := json.Marshal(map[string]string{"agent": req.Agent, "cred": req.Cred})
		st, data, err := s.v1RequestWebuser(ctx, user, http.MethodPost, "/v1/vault/delete", body)
		s.vaultRelay(w, st, data, err)
	default:
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
	}
}
