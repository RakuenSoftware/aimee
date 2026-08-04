package main

// vault.go — webchat wiring for the one environment credential vault. The
// browser holds nothing. The PAM actor is forwarded over the kernel-attested
// UDS boundary for authorization and audit, never as a credential namespace.
// Unlock/password endpoints remain compatibility imports for historical actor
// vaults. All vault calls go over the
// local /v1 path (v1RequestWebuser) — NEVER the OpenAI proxy (proxyV1),
// which strips Authorization.

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"time"
)

// v1RequestWebuser is v1Request plus X-Aimee-Webuser. aimee-server accepts that
// assertion only from the kernel-attested root peer on its Unix socket.
func (s *server) v1RequestWebuser(ctx context.Context, username, method, path string, body []byte) (int, []byte, error) {
	return s.v1RequestWebuserTCapability(ctx, username, method, path, body, socketCallTimeout, false)
}

// v1RequestWorkflowOperator carries an explicit operator capability resolved
// by isAdmin at this trusted boundary. Keeping it separate from the username
// prevents a literal account named "admin" from acquiring operator authority.
func (s *server) v1RequestWorkflowOperator(ctx context.Context, username, method, path string, body []byte) (int, []byte, error) {
	return s.v1RequestWebuserTCapability(ctx, username, method, path, body, socketCallTimeout, true)
}

// v1RequestWebuserT is v1RequestWebuser with an explicit client timeout, for the
// few endpoints that legitimately hold the request open longer than the default
// socketCallTimeout (e.g. a synchronous one-shot LLM draft).
func (s *server) v1RequestWebuserT(ctx context.Context, username, method, path string, body []byte, timeout time.Duration) (int, []byte, error) {
	return s.v1RequestWebuserTCapability(ctx, username, method, path, body, timeout, false)
}

func (s *server) v1RequestWebuserTCapability(ctx context.Context, username, method, path string, body []byte, timeout time.Duration, workflowOperator bool) (int, []byte, error) {
	if username == "" {
		return http.StatusUnauthorized, nil, errNoVaultTrust
	}
	sock := s.aimeeHTTPSockPathFor(path)
	client := &http.Client{
		Timeout: timeout,
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
	req.Header.Set("X-Aimee-Webuser", username)
	if workflowOperator {
		req.Header.Set("X-Aimee-Workflow-Operator", "true")
	}
	resp, err := client.Do(req)
	if err != nil {
		return 0, nil, err
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	return resp.StatusCode, data, err
}

var errNoVaultTrust = &vaultErr{"vault trust unavailable (webchat session missing)"}

type vaultErr struct{ msg string }

func (e *vaultErr) Error() string { return e.msg }

// vaultSafeErrorMessage extracts ONLY a short server-generated error string from
// an aimee-server error envelope (e.g. "vault locked"), never the raw body —
// defense in depth so nothing unexpected reaches the browser.
func vaultSafeErrorMessage(data []byte) string {
	var e struct {
		Error   any    `json:"error"`
		Message string `json:"message"`
	}
	_ = json.Unmarshal(data, &e)
	if s, ok := e.Error.(string); ok && s != "" {
		return s
	}
	if m, ok := e.Error.(map[string]any); ok {
		if ms, ok := m["message"].(string); ok && ms != "" {
			return ms
		}
	}
	if e.Message != "" {
		return e.Message
	}
	return "vault: request failed"
}

// vaultRelayStatus is the browser response for a MUTATION (unlock/set/delete/
// rekey): a fixed {"status":"ok"} on success, a sanitized error otherwise. It
// NEVER echoes the upstream body — webchat is the user-facing trust boundary, so
// no aimee-server bytes are passed through verbatim.
func (s *server) vaultRelayStatus(w http.ResponseWriter, st int, data []byte, err error) {
	w.Header().Set("Content-Type", "application/json")
	if err != nil {
		writeJSONError(w, http.StatusServiceUnavailable, "vault: aimee-server unavailable")
		return
	}
	if st != http.StatusOK {
		writeJSONError(w, st, vaultSafeErrorMessage(data))
		return
	}
	w.Write([]byte(`{"status":"ok"}`))
}

// vaultRelayList is the browser response for GET (names only). It decodes the
// upstream response and re-marshals a strict {agent,cred} allowlist, so a
// secret/value/KEK field can NEVER reach the browser even if upstream regresses.
func (s *server) vaultRelayList(w http.ResponseWriter, st int, data []byte, err error) {
	w.Header().Set("Content-Type", "application/json")
	if err != nil {
		writeJSONError(w, http.StatusServiceUnavailable, "vault: aimee-server unavailable")
		return
	}
	if st != http.StatusOK {
		writeJSONError(w, st, vaultSafeErrorMessage(data))
		return
	}
	var up struct {
		Credentials []struct {
			Agent string `json:"agent"`
			Cred  string `json:"cred"`
		} `json:"credentials"`
	}
	_ = json.Unmarshal(data, &up)
	creds := make([]map[string]string, 0, len(up.Credentials))
	for _, c := range up.Credentials {
		creds = append(creds, map[string]string{"agent": c.Agent, "cred": c.Cred})
	}
	out, _ := json.Marshal(map[string]any{"status": "ok", "credentials": creds})
	w.Write(out)
}

// POST /api/vault/unlock {password} — import/unlock a historical actor vault.
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
	s.vaultRelayStatus(w, st, data, err)
}

// POST /api/vault/password {old_password,new_password} — re-key a historical actor vault.
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
	s.vaultRelayStatus(w, st, data, err)
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
		s.vaultRelayList(w, st, data, err)
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
		s.vaultRelayStatus(w, st, data, err)
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
		s.vaultRelayStatus(w, st, data, err)
	default:
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
	}
}
